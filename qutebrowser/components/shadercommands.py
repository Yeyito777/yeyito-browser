# SPDX-FileCopyrightText: Yeyito
#
# SPDX-License-Identifier: GPL-3.0-or-later

"""Element shader on/off commands."""

from qutebrowser.api import cmdutils
from qutebrowser.config import config
from qutebrowser.utils import objreg


# Tracks runtime state; initialized from config on first use
_shader_enabled: bool | None = None


def _get_shader_enabled():
    """Get the current shader state, initializing from config if needed."""
    global _shader_enabled
    if _shader_enabled is None:
        _shader_enabled = config.val.content.element_shader
    return _shader_enabled


def _get_profiles():
    """Get all active QWebEngineProfiles."""
    from qutebrowser.browser.webengine import webenginesettings
    profiles = [webenginesettings.default_profile]
    if webenginesettings.private_profile is not None:
        profiles.append(webenginesettings.private_profile)
    return profiles


def _set_shader_enabled(enabled):
    """Set the ElementShaderEnabled attribute on all profiles."""
    import sys
    from qutebrowser.qt.webenginecore import QWebEngineSettings
    print(f"[SHADER-DEBUG-0] Python: setting ElementShaderEnabled = {enabled}", file=sys.stderr)
    for profile in _get_profiles():
        profile.settings().setAttribute(
            QWebEngineSettings.WebAttribute.ElementShaderEnabled, enabled)
        val = profile.settings().testAttribute(
            QWebEngineSettings.WebAttribute.ElementShaderEnabled)
        print(f"[SHADER-DEBUG-0] Python: testAttribute after set = {val}", file=sys.stderr)


def _update_splitter_handles(enabled):
    """Update the inspector splitter handle color on all tabs."""
    for win_id in objreg.window_registry:
        tabbed_browser = objreg.get('tabbed-browser', scope='window',
                                    window=win_id)
        for tab in tabbed_browser.widgets():
            splitter = tab.data.splitter
            if splitter is not None:
                splitter.update_shader_style(enabled)


def _do_shader_off():
    """Internal: disable the shader."""
    global _shader_enabled
    _shader_enabled = False
    _set_shader_enabled(False)
    _update_splitter_handles(False)


def _do_shader_on():
    """Internal: enable the shader."""
    global _shader_enabled
    _shader_enabled = True
    _set_shader_enabled(True)
    _update_splitter_handles(True)


@cmdutils.register(name='shader-off')
def shader_off() -> None:
    """Turn off the element shader."""
    if not _get_shader_enabled():
        return
    _do_shader_off()


@cmdutils.register(name='shader-on')
def shader_on() -> None:
    """Turn on the element shader."""
    if _get_shader_enabled():
        return
    _do_shader_on()


@cmdutils.register(name='shader-toggle')
def shader_toggle() -> None:
    """Toggle the element shader on or off."""
    if _get_shader_enabled():
        _do_shader_off()
    else:
        _do_shader_on()


@cmdutils.register(name='shader-reload')
def shader_reload() -> None:
    """Reload the element shader (off then on)."""
    _do_shader_off()
    _do_shader_on()
