# SPDX-FileCopyrightText: Yeyito
#
# SPDX-License-Identifier: GPL-3.0-or-later

"""Element shader on/off commands."""

from qutebrowser.api import cmdutils
from qutebrowser.utils import objreg


# Shader is on by default (C++ shader runs unconditionally unless disabled)
_shader_enabled = True

_SHADER_SCRIPT_NAME = '_qute_shader_off'

# JS for profile-level script (DocumentCreation): sets attribute before styles resolve
_SHADER_OFF_PROFILE_JS = (
    "(function() {"
    " if (document.documentElement)"
    "   document.documentElement.setAttribute('data-no-shader', '');"
    "})();"
)

# JS to run on existing tabs to disable shader + force full style recalculation
_SHADER_OFF_JS = """(function() {
    var root = document.documentElement;
    if (!root) return;
    root.setAttribute('data-no-shader', '');
    var s = document.getElementById('__shader_trigger');
    if (!s) {
        s = document.createElement('style');
        s.id = '__shader_trigger';
        (document.head || root).appendChild(s);
    }
    s.textContent = ':root { --__shader_state: off; }';
})();"""

# JS to run on existing tabs to enable shader + force full style recalculation
_SHADER_ON_JS = """(function() {
    var root = document.documentElement;
    if (!root) return;
    root.removeAttribute('data-no-shader');
    var s = document.getElementById('__shader_trigger');
    if (!s) {
        s = document.createElement('style');
        s.id = '__shader_trigger';
        (document.head || root).appendChild(s);
    }
    s.textContent = ':root { --__shader_state: on; }';
})();"""


def _get_profiles():
    """Get all active QWebEngineProfiles."""
    from qutebrowser.browser.webengine import webenginesettings
    profiles = [webenginesettings.default_profile]
    if webenginesettings.private_profile is not None:
        profiles.append(webenginesettings.private_profile)
    return profiles


def _install_shader_off_script():
    """Install a profile-level script that disables the shader on new pages."""
    from qutebrowser.qt.webenginecore import QWebEngineScript
    script = QWebEngineScript()
    script.setName(_SHADER_SCRIPT_NAME)
    script.setSourceCode(_SHADER_OFF_PROFILE_JS)
    script.setInjectionPoint(QWebEngineScript.InjectionPoint.DocumentCreation)
    script.setWorldId(QWebEngineScript.ScriptWorldId.ApplicationWorld)
    script.setRunsOnSubFrames(True)
    for profile in _get_profiles():
        profile.scripts().insert(script)


def _remove_shader_off_script():
    """Remove the shader-off script from all profiles."""
    for profile in _get_profiles():
        scripts = profile.scripts()
        for script in scripts.find(_SHADER_SCRIPT_NAME):
            scripts.remove(script)


def _run_js_all_tabs(js_code):
    """Run JavaScript on all open tabs."""
    for win_id in objreg.window_registry:
        tabbed_browser = objreg.get('tabbed-browser', scope='window',
                                    window=win_id)
        for tab in tabbed_browser.widgets():
            tab.run_js_async(js_code)


def _do_shader_off():
    """Internal: disable the shader."""
    global _shader_enabled
    _shader_enabled = False
    _install_shader_off_script()
    _run_js_all_tabs(_SHADER_OFF_JS)


def _do_shader_on():
    """Internal: enable the shader."""
    global _shader_enabled
    _shader_enabled = True
    _remove_shader_off_script()
    _run_js_all_tabs(_SHADER_ON_JS)


@cmdutils.register(name='shader-off')
def shader_off() -> None:
    """Turn off the element shader."""
    if not _shader_enabled:
        return
    _do_shader_off()


@cmdutils.register(name='shader-on')
def shader_on() -> None:
    """Turn on the element shader."""
    if _shader_enabled:
        return
    _do_shader_on()


@cmdutils.register(name='shader-reload')
def shader_reload() -> None:
    """Reload the element shader (off then on)."""
    _do_shader_off()
    _do_shader_on()
