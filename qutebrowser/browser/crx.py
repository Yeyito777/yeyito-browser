"""CRX3 Chrome extension file parsing and extraction."""

import io
import json
import os
import shutil
import struct
import zipfile

from qutebrowser.utils import log, utils


CRX_MAGIC = b'Cr24'


def parse_crx3(data: bytes) -> bytes:
    """Strip CRX3 header and return the zip payload.

    CRX3 format:
        bytes 0-3:   magic "Cr24"
        bytes 4-7:   version (uint32 LE, must be 3)
        bytes 8-11:  header_length (uint32 LE)
        bytes 12..12+header_length: signed header proto
        remaining:   zip data
    """
    if len(data) < 12:
        raise ValueError("File too small to be a CRX file")

    magic = data[:4]
    if magic != CRX_MAGIC:
        raise ValueError(f"Not a CRX file (magic: {magic!r})")

    version = struct.unpack('<I', data[4:8])[0]
    if version != 3:
        raise ValueError(f"Unsupported CRX version: {version}")

    header_len = struct.unpack('<I', data[8:12])[0]
    zip_start = 12 + header_len

    if zip_start > len(data):
        raise ValueError(
            f"CRX header length ({header_len}) exceeds file size ({len(data)})"
        )

    return data[zip_start:]


def _sanitize_name(name: str) -> str:
    """Sanitize an extension name for use as a directory name."""
    safe = utils.sanitize_filename(name, replacement='-')
    safe = safe.replace(' ', '-')
    # Collapse multiple dashes and strip leading/trailing
    while '--' in safe:
        safe = safe.replace('--', '-')
    safe = safe.strip('-').lower()
    return safe or 'unknown-extension'


def extract_crx(crx_path: str, target_dir: str) -> str:
    """Extract a CRX file to target_dir/<extension-name>/.

    Reads the CRX file, strips the header, extracts the zip payload,
    reads manifest.json to determine the extension name, and extracts
    to a sanitized subdirectory.

    Args:
        crx_path: Path to the .crx file.
        target_dir: Parent directory for extracted extensions
                    (e.g. config/extensions/).

    Returns:
        Path to the extracted extension directory.
    """
    with open(crx_path, 'rb') as f:
        data = f.read()

    zip_data = parse_crx3(data)

    with zipfile.ZipFile(io.BytesIO(zip_data)) as zf:
        try:
            manifest_raw = zf.read('manifest.json')
        except KeyError:
            raise ValueError("Extension zip has no manifest.json")

        manifest = json.loads(manifest_raw)
        ext_name = manifest.get('name', 'unknown-extension')
        # Handle Chrome i18n message references like "__MSG_appName__"
        if ext_name.startswith('__MSG_') and ext_name.endswith('__'):
            ext_name = manifest.get('short_name', ext_name)
        if ext_name.startswith('__MSG_') and ext_name.endswith('__'):
            ext_name = 'unknown-extension'

        safe_name = _sanitize_name(ext_name)
        ext_dir = os.path.join(target_dir, safe_name)

        log.misc.debug(f"Extracting CRX to: {ext_dir}")

        if os.path.exists(ext_dir):
            shutil.rmtree(ext_dir)

        os.makedirs(ext_dir, exist_ok=True)
        zf.extractall(ext_dir)

    return ext_dir
