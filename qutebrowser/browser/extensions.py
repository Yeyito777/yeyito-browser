# SPDX-FileCopyrightText: Florian Bruhin (The Compiler) <mail@qutebrowser.org>
#
# SPDX-License-Identifier: GPL-3.0-or-later

"""Chrome extension management commands."""

import os
import shutil

from qutebrowser.api import cmdutils
from qutebrowser.completion.models import miscmodels
from qutebrowser.utils import message, standarddir, log


def _get_extensions_dir():
    """Return the path to the extensions directory."""
    return os.path.join(standarddir.config(), 'extensions')


def _on_disk_extensions():
    """Return a dict of {dirname: manifest_path} for on-disk extensions."""
    ext_dir = _get_extensions_dir()
    if not os.path.isdir(ext_dir):
        return {}
    result = {}
    for entry in os.scandir(ext_dir):
        if entry.is_dir():
            manifest = os.path.join(entry.path, 'manifest.json')
            if os.path.isfile(manifest):
                result[entry.name] = entry.path
    return result


def _loaded_extensions():
    """Return a dict of {path: info} for loaded extensions, or None."""
    try:
        from qutebrowser.browser.webengine import webenginesettings
        ext_manager = webenginesettings._get_extension_manager(
            webenginesettings.default_profile)
    except Exception:
        return None
    if ext_manager is None:
        return None
    result = {}
    for info in ext_manager.extensions():
        result[info.path()] = info
    return result


def _read_manifest_name(ext_path):
    """Read the extension name from manifest.json."""
    import json
    manifest_path = os.path.join(ext_path, 'manifest.json')
    try:
        with open(manifest_path) as f:
            manifest = json.load(f)
        name = manifest.get('name', '')
        if name.startswith('__MSG_') and name.endswith('__'):
            name = manifest.get('short_name', name)
        if name.startswith('__MSG_') and name.endswith('__'):
            name = ''
        return name
    except (OSError, ValueError):
        return ''


@cmdutils.register()
@cmdutils.argument('detail', choices=['verbose'],
                   completion=miscmodels.extensions_detail)
def extensions(detail: str = None) -> None:
    """List all installed Chrome extensions and their load status.

    Args:
        detail: Level of detail ("verbose" shows Chromium extension IDs).
    """
    verbose = detail == 'verbose'
    on_disk = _on_disk_extensions()
    loaded = _loaded_extensions()

    if loaded is None:
        message.error("Extension manager not available "
                      "(requires QtWebEngine 6.10+)")
        return

    lines = []

    # On-disk extensions (user-installed)
    for dirname, ext_path in sorted(on_disk.items()):
        loaded_info = loaded.get(ext_path)
        if loaded_info:
            name = loaded_info.name()
            line = f"  {dirname}: {name}"
            if verbose:
                line += f" [{loaded_info.id()}]"
            lines.append(line)
        else:
            name = _read_manifest_name(ext_path)
            label = f"  {dirname}: {name}" if name else f"  {dirname}"
            lines.append(f"{label} (not loaded)")

    # Built-in extensions (loaded but not in extensions/ dir)
    ext_dir = _get_extensions_dir()
    for path, info in sorted(loaded.items(), key=lambda x: x[1].name()):
        if path.startswith(ext_dir):
            continue
        line = f"  {info.name()}"
        if verbose:
            line += f" [{info.id()}]"
        line += " (built-in)"
        lines.append(line)

    if not lines:
        message.info("No extensions installed")
        return

    header = f"{len(on_disk)} extension(s) installed:"
    message.info('\n'.join([header] + lines))


@cmdutils.register()
@cmdutils.argument('name', completion=miscmodels.extension)
def uninstall_extension(name: str) -> None:
    """Uninstall a Chrome extension by removing it from disk.

    The extension will be unloaded if possible. A restart may be
    needed for full removal.

    Args:
        name: The extension directory name (tab-completable).
    """
    ext_dir = _get_extensions_dir()
    ext_path = os.path.join(ext_dir, name)

    if not os.path.isdir(ext_path):
        raise cmdutils.CommandError(
            f"Extension not found: {name}")

    manifest = os.path.join(ext_path, 'manifest.json')
    if not os.path.isfile(manifest):
        raise cmdutils.CommandError(
            f"Not a valid extension (no manifest.json): {name}")

    # Try to unload from the running profile
    unloaded = False
    loaded = _loaded_extensions()
    if loaded and ext_path in loaded:
        try:
            from qutebrowser.browser.webengine import webenginesettings
            ext_manager = webenginesettings._get_extension_manager(
                webenginesettings.default_profile)
            if ext_manager:
                ext_manager.unloadExtension(loaded[ext_path])
                unloaded = True
                log.misc.debug(f"Unloaded extension: {name}")
        except Exception as e:
            log.misc.warning(f"Failed to unload extension {name}: {e}")

    # Remove from disk
    try:
        shutil.rmtree(ext_path)
    except OSError as e:
        raise cmdutils.CommandError(
            f"Failed to remove extension: {e}")

    if unloaded:
        message.info(f"Extension uninstalled: {name}")
    else:
        message.info(f"Extension removed from disk: {name} "
                     "(restart to fully unload)")
