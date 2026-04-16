# SPDX-FileCopyrightText: Florian Bruhin (The Compiler) <mail@qutebrowser.org>
#
# SPDX-License-Identifier: GPL-3.0-or-later

"""Helpers for qutebrowser's internal remote debugging bridge.

This module is intentionally stdlib-only so it can be imported from earlyinit
before any Qt modules are loaded.
"""

from __future__ import annotations

import hashlib
import os
from typing import Optional, Tuple


REMOTE_DEBUGGING_ENV = 'QTWEBENGINE_REMOTE_DEBUGGING'
_CHROMIUM_FLAGS_ENV = 'QTWEBENGINE_CHROMIUM_FLAGS'
_DEFAULT_HOST = '127.0.0.1'
# Use a high, deterministic localhost port derived from the basedir.
# This avoids hard-coding a single global port while still keeping the address
# stable across restarts of the same profile.
_PORT_BASE = 38000
_PORT_SPAN = 20000


def auto_remote_debugging_value(basedir: str) -> str:
    """Return a deterministic host:port for the given qutebrowser basedir."""
    normalized = os.path.abspath(basedir)
    digest = hashlib.sha256(normalized.encode('utf-8')).digest()
    port = _PORT_BASE + (int.from_bytes(digest[:4], 'big') % _PORT_SPAN)
    return f'{_DEFAULT_HOST}:{port}'


def parse_remote_debugging_value(value: Optional[str]) -> Optional[Tuple[str, int]]:
    """Parse QTWEBENGINE_REMOTE_DEBUGGING style values.

    Valid inputs are either ``"12345"`` or ``"127.0.0.1:12345"``.
    Returns ``(host, port)`` or ``None`` if the value is missing/invalid.
    """
    if not value:
        return None

    value = value.strip()
    if not value:
        return None

    if ':' in value:
        host, port_str = value.rsplit(':', 1)
        host = host or _DEFAULT_HOST
    else:
        host = _DEFAULT_HOST
        port_str = value

    try:
        port = int(port_str)
    except ValueError:
        return None

    if not (0 < port < 65535):
        return None

    return host, port


def has_remote_debugging_override() -> bool:
    """Return True when the user already configured remote debugging."""
    if os.environ.get(REMOTE_DEBUGGING_ENV):
        return True

    chromium_flags = os.environ.get(_CHROMIUM_FLAGS_ENV, '')
    return '--remote-debugging-port=' in chromium_flags


def ensure_remote_debugging_env(*, basedir: Optional[str]) -> Optional[str]:
    """Enable localhost-only remote debugging unless the user already did.

    Returns the resulting env value, or ``None`` if no value was set.
    """
    if has_remote_debugging_override() or not basedir:
        return os.environ.get(REMOTE_DEBUGGING_ENV)

    value = auto_remote_debugging_value(basedir)
    os.environ.setdefault(REMOTE_DEBUGGING_ENV, value)
    return os.environ.get(REMOTE_DEBUGGING_ENV)


def current_remote_debugging_address() -> Optional[Tuple[str, int]]:
    """Return the currently configured localhost remote debugging address."""
    return parse_remote_debugging_value(os.environ.get(REMOTE_DEBUGGING_ENV))
