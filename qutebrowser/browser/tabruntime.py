# SPDX-FileCopyrightText: Florian Bruhin (The Compiler) <mail@qutebrowser.org>
#
# SPDX-License-Identifier: GPL-3.0-or-later

"""Tab runtime directory manager — exposes live tab state as plain-text files."""

import json
import shutil
import datetime
from pathlib import Path

from qutebrowser.qt.core import QObject, QTimer
from qutebrowser.qt import sip

from qutebrowser.utils import standarddir, usertypes, message


class TabRuntimeManager(QObject):

    """Manages runtime/tabs/ directory with live tab state as files.

    Each open tab gets a directory tabs/{tab_id}/ with a tab-data.info file
    containing key: value lines. An 'order' file lists tab_ids in tab-bar order.
    """

    def __init__(self, tabbed_browser, parent=None):
        super().__init__(parent)
        self._tabbed_browser = tabbed_browser
        self._tabs_dir = Path(standarddir.runtime()) / 'tabs'
        self._tab_data = {}  # tab_id str -> dict

        # Wipe and recreate (handles crash leftovers)
        shutil.rmtree(self._tabs_dir, ignore_errors=True)
        self._tabs_dir.mkdir(parents=True, exist_ok=True)

        tabbed_browser.new_tab.connect(self._on_new_tab)
        tabbed_browser.shutting_down.connect(self._on_shutdown)
        tabbed_browser.widget.tab_bar().tabMoved.connect(self._update_indices)

    def _on_new_tab(self, tab, idx):
        tab_id = str(tab.tab_id)

        self._tab_data[tab_id] = {
            'url': tab.url().toDisplayString(),
            'title': tab.title(),
            'index': str(idx),
            'pinned': str(tab.data.pinned).lower(),
            'load_status': tab.load_status().name,
            'private': str(bool(tab.is_private)).lower(),
            'audio': self._audio_state(tab),
            'window': str(tab.win_id),
            'created_at': datetime.datetime.now().isoformat(),
        }
        (self._tabs_dir / tab_id).mkdir(exist_ok=True)
        self._write_tab(tab_id)

        # Per-tab signal connections
        tab.url_changed.connect(
            lambda url, tid=tab_id: self._update_field(
                tid, 'url', url.toDisplayString()))
        tab.title_changed.connect(
            lambda title, tid=tab_id: self._update_field(
                tid, 'title', title))
        tab.load_status_changed.connect(
            lambda status, tid=tab_id: self._update_field(
                tid, 'load_status', status.name))
        tab.pinned_changed.connect(
            lambda pinned, tid=tab_id: self._update_field(
                tid, 'pinned', str(pinned).lower()))
        tab.audio.muted_changed.connect(
            lambda _, t=tab, tid=tab_id: self._update_field(
                tid, 'audio', self._audio_state(t)))
        tab.audio.recently_audible_changed.connect(
            lambda _, t=tab, tid=tab_id: self._update_field(
                tid, 'audio', self._audio_state(t)))
        tab.shutting_down.connect(
            lambda tid=tab_id: self._on_tab_removed(tid))
        tab.console_message.connect(
            lambda level, source, line, msg, tid=tab_id:
                self._append_console_log(tid, level, source, line, msg))
        tab.load_started.connect(
            lambda tid=tab_id: self._truncate_console_log(tid))

        # Auto-resume YouTube playback after session restore.
        # The greasemonkey script emits console.log('[yt-resume-ready]') when
        # seek has settled and the video was playing before shutdown.  We watch
        # for that signal and immediately grant user activation + play().
        tab.console_message.connect(
            lambda level, source, line, msg, t=tab:
                self._on_youtube_resume_signal(t, msg))

        self._update_indices()

    def _on_tab_removed(self, tab_id):
        self._tab_data.pop(tab_id, None)
        shutil.rmtree(self._tabs_dir / tab_id, ignore_errors=True)
        self._update_indices()

    def _update_indices(self):
        tab_ids = []
        for i, tab in enumerate(self._tabbed_browser.widgets()):
            tid = str(tab.tab_id)
            if tid in self._tab_data:
                self._tab_data[tid]['index'] = str(i)
                self._write_tab(tid)
                tab_ids.append(tid)
        order_file = self._tabs_dir / 'order'
        try:
            order_file.write_text(
                '\n'.join(tab_ids) + '\n' if tab_ids else '')
        except FileNotFoundError:
            pass

    def _update_field(self, tab_id, key, value):
        if tab_id in self._tab_data:
            self._tab_data[tab_id][key] = value
            self._write_tab(tab_id)

    def _write_tab(self, tab_id):
        data = self._tab_data.get(tab_id)
        if not data:
            return
        filepath = self._tabs_dir / tab_id / 'tab-data.info'
        try:
            filepath.write_text(
                '\n'.join(f'{k}: {v}' for k, v in data.items()) + '\n')
        except FileNotFoundError:
            pass

    @staticmethod
    def _audio_state(tab):
        if tab.audio.is_muted():
            return 'muted'
        if tab.audio.is_recently_audible():
            return 'unmuted'
        return 'none'

    def _append_console_log(self, tab_id, level, source, line, msg):
        """Append a console message to the tab's console.log file."""
        if tab_id not in self._tab_data:
            return
        log_path = self._tabs_dir / tab_id / 'console.log'
        timestamp = datetime.datetime.now().isoformat()
        entry = f'[{timestamp}] {level.name.upper()} {source}:{line} — {msg}\n'
        try:
            with open(log_path, 'a') as f:
                f.write(entry)
        except FileNotFoundError:
            pass

    def _truncate_console_log(self, tab_id):
        """Clear the console.log file on navigation."""
        if tab_id not in self._tab_data:
            return
        log_path = self._tabs_dir / tab_id / 'console.log'
        try:
            log_path.write_text('')
        except FileNotFoundError:
            pass

    def js_eval_tab(self, tab_id_str, js_code):
        """Evaluate JS in a tab and write result to console-result.

        Uses three sequential run_js_async calls (processed in order by
        QtWebEngine) to avoid eval() which is blocked by Trusted Types CSP:
        1. Install console interceptors on window.__qb_console
        2. Run the user's code directly (return value via callback)
        3. Collect captured output, restore originals, write result file
        """
        tab = None
        for t in self._tabbed_browser.widgets():
            if str(t.tab_id) == tab_id_str:
                tab = t
                break
        if tab is None:
            return False

        result_path = self._tabs_dir / tab_id_str / 'console-result'

        setup_js = (
            '(function(){'
            'window.__qb_c={out:[],'
            'oL:console.log,oW:console.warn,oE:console.error};'
            'console.log=function(){'
            'window.__qb_c.out.push(Array.from(arguments).join(" "));'
            'window.__qb_c.oL.apply(console,arguments);};'
            'console.warn=function(){'
            'window.__qb_c.out.push("[warn] "+Array.from(arguments).join(" "));'
            'window.__qb_c.oW.apply(console,arguments);};'
            'console.error=function(){'
            'window.__qb_c.out.push("[error] "+Array.from(arguments).join(" "));'
            'window.__qb_c.oE.apply(console,arguments);};'
            '})()'
        )

        collect_js = (
            '(function(){'
            'var c=window.__qb_c;if(!c)return"";'
            'console.log=c.oL;console.warn=c.oW;console.error=c.oE;'
            'var out=c.out.join("\\n");delete window.__qb_c;return out;'
            '})()'
        )

        def _on_user_result(result):
            def _on_collect(output):
                try:
                    parts = []
                    if output:
                        parts.append(str(output))
                    if result is not None:
                        parts.append(f'=> {result}')
                    text = '\n'.join(parts) if parts else 'undefined'
                    result_path.write_text(text + '\n')
                except FileNotFoundError:
                    pass

            tab.run_js_async(
                collect_js, callback=_on_collect,
                world=usertypes.JsWorld.main)

        # 1. Install interceptors
        tab.run_js_async(setup_js, world=usertypes.JsWorld.main)
        # 2. Run user code directly (no eval — not subject to CSP)
        tab.run_js_async(
            js_code, callback=_on_user_result,
            world=usertypes.JsWorld.main)
        return True

    def snapshot_dom(self, tab_id_str):
        """Capture a tab's DOM and write it to dom.html in its runtime dir."""
        tab = None
        for t in self._tabbed_browser.widgets():
            if str(t.tab_id) == tab_id_str:
                tab = t
                break
        if tab is None:
            return False

        dom_path = self._tabs_dir / tab_id_str / 'dom.html'

        def _on_result(html):
            if html is not None:
                try:
                    dom_path.write_text(html)
                except FileNotFoundError:
                    pass

        tab.run_js_async(
            'document.documentElement.outerHTML', callback=_on_result)
        return True

    def _on_youtube_resume_signal(self, tab, msg):
        """Grant user activation and play when greasemonkey signals seek done.

        The greasemonkey script emits console.log('[yt-resume-ready]') after
        the seek has settled and the pre-restore state was playing.  This fires
        immediately with no fixed delay.
        """
        if '[yt-resume-ready]' not in msg:
            return
        if getattr(tab, '_yt_resumed', False):
            return
        if sip.isdeleted(tab._widget):
            return
        tab._yt_resumed = True
        tab._widget.page().notifyUserActivation()
        tab.run_js_async(
            "document.querySelector('video')?.play()",
            world=usertypes.JsWorld.main)

    def _find_tab(self, tab_id_str):
        """Find a tab widget by its string ID."""
        for t in self._tabbed_browser.widgets():
            if str(t.tab_id) == tab_id_str:
                return t
        return None

    def screenshot_tab(self, tab_id_str, window_mode=False):
        """Capture a screenshot of any tab at full screen resolution.

        For background tabs, the target must become the current widget in
        QStackedLayout so Qt's render loop sends BeginFrame to its compositor.
        To avoid disrupting the user:

        1. C++ focus suppression (s_suppressFocusCount) blocks Chromium-level
           focus notifications AND keyboard/shortcut forwarding in the
           QQuickItem delegate, so the tab switch is invisible to renderers.
        2. The original tab is immediately re-shown and raised on top, keeping
           Qt focus and keyboard input on the correct widget.
        """
        tab = self._find_tab(tab_id_str)
        if tab is None:
            return False

        screenshot_path = self._tabs_dir / tab_id_str / 'screenshot.png'

        if window_mode:
            main_window = self._tabbed_browser.window()
            pic = main_window.grab()
            if pic is not None and not pic.isNull():
                pic.save(str(screenshot_path))
            return True

        from qutebrowser.qt.widgets import QApplication
        from qutebrowser.qt.core import QSize
        from qutebrowser.qt.webenginecore import QWebEnginePage

        # Full-screen resolution from primary monitor
        main_window = self._tabbed_browser.window()
        screen = main_window.screen() or QApplication.primaryScreen()
        size = screen.size()

        tab_widget = self._tabbed_browser.widget
        original_idx = tab_widget.currentIndex()
        target_idx = tab_widget.indexOf(tab)
        if target_idx < 0:
            return False

        is_background = (target_idx != original_idx)
        original_view = tab_widget.widget(original_idx) if is_background else None

        if is_background:
            # 1. Suppress focus/keyboard at C++ level (delegate item + client)
            QWebEnginePage.suppressFocusNotifications()

            # 2. Save the widget that currently has keyboard focus
            focused_widget = QApplication.focusWidget()

            # 3. Block qutebrowser's tab-change handler
            tab_widget.blockSignals(True)

            # 4. Switch stacked layout to target — it now gets BeginFrame
            #    (original is hidden by QStackedLayout, target is shown)
            tab_widget.setCurrentIndex(target_idx)

            # 5. Re-show the original tab on top — BOTH tabs are now visible
            #    and rendering, but the original covers the target visually.
            #    Since original is visible again, setFocus() works.
            original_view.show()
            original_view.raise_()

            # 6. Restore Qt focus to the original tab's focused widget
            if focused_widget is not None:
                focused_widget.setFocus()

        def _on_result(data):
            if is_background:
                # Restore stacked layout: original becomes current again,
                # target gets hidden.  Original was already visible + raised.
                tab_widget.setCurrentIndex(original_idx)
                tab_widget.blockSignals(False)
                QWebEnginePage.restoreFocusNotifications()

            if data and len(data) > 0:
                try:
                    screenshot_path.write_bytes(bytes(data))
                except FileNotFoundError:
                    pass

        tab._widget.page().captureScreenshot(
            QSize(size.width(), size.height()), _on_result)
        return True

    def network_list(self, tab_id_str):
        """Query all captured network requests for a tab."""
        tab = self._find_tab(tab_id_str)
        if tab is None:
            return False
        result_path = self._tabs_dir / tab_id_str / 'network.json'

        def _on_result(data):
            try:
                result_path.write_text(
                    data if data else '{"error":"not available"}\n')
            except FileNotFoundError:
                pass

        tab.network_query('list', {}, _on_result)
        return True

    def network_detail(self, tab_id_str, request_id):
        """Get detail + response body/headers for a network request.

        Chains: C++ detail → JS fetch (via console.log callback) → write file.
        Uses console.log with a sentinel prefix because QtWebEngine's
        runJavaScript callback doesn't resolve Promises.
        """
        tab = self._find_tab(tab_id_str)
        if tab is None:
            return False
        result_path = self._tabs_dir / tab_id_str / f'request-{request_id}.json'
        sentinel = f'__qb_nr_{request_id}'

        def _write(obj):
            try:
                result_path.write_text(json.dumps(obj, indent=2) + '\n')
            except FileNotFoundError:
                pass

        def _on_detail(data):
            if not data:
                _write({'error': 'not available'})
                return
            try:
                detail = json.loads(data)
            except json.JSONDecodeError:
                _write({'error': 'invalid response'})
                return
            if 'error' in detail:
                _write(detail)
                return

            url = detail.get('url', '')
            if not url:
                _write(detail)
                return

            # Listen for the fetch result via console.log sentinel
            def _on_console(level, source, line, msg):
                if not msg.startswith(sentinel):
                    return
                try:
                    tab.console_message.disconnect(_on_console)
                except TypeError:
                    pass
                payload = msg[len(sentinel):]
                if payload:
                    try:
                        detail.update(json.loads(payload))
                    except (json.JSONDecodeError, TypeError):
                        pass
                _write(detail)

            tab.console_message.connect(_on_console)

            # Re-fetch from HTTP cache to get body + response headers
            url_json = json.dumps(url)
            sentinel_json = json.dumps(sentinel)
            fetch_js = (
                f'fetch({url_json}, {{cache: "force-cache"}})'
                '.then(async function(r) {'
                '  var h = {};'
                '  r.headers.forEach(function(v, k) { h[k] = v; });'
                f'  console.log({sentinel_json} + JSON.stringify({{responseHeaders: h, body: await r.text()}}));'
                '}).catch(function(e) {'
                f'  console.log({sentinel_json} + JSON.stringify({{bodyError: e.message}}));'
                '})'
            )
            tab.run_js_async(fetch_js, world=usertypes.JsWorld.main)

        tab.network_query('detail', {'request_id': request_id}, _on_detail)
        return True

    def network_body(self, tab_id_str, request_id):
        """Get response body for a network request."""
        tab = self._find_tab(tab_id_str)
        if tab is None:
            return False
        body_path = self._tabs_dir / tab_id_str / 'network-body'

        def _on_result(data):
            try:
                if isinstance(data, bytes):
                    body_path.write_bytes(data)
                else:
                    body_path.write_text(data if data else '')
            except FileNotFoundError:
                pass

        tab.network_query('body', {'request_id': request_id}, _on_result)
        return True

    def network_ws_frames(self, tab_id_str, request_id):
        """Get WebSocket frames for a connection."""
        tab = self._find_tab(tab_id_str)
        if tab is None:
            return False
        result_path = self._tabs_dir / tab_id_str / 'network.json'

        def _on_result(data):
            try:
                result_path.write_text(
                    data if data else '{"error":"not available"}\n')
            except FileNotFoundError:
                pass

        tab.network_query('ws_frames', {'request_id': request_id}, _on_result)
        return True

    def command_eval(self, tab_id_str, command, wait_ms=0):
        """Run a qutebrowser command and capture messages to command-result.

        Hooks message.global_bridge.show_message to capture all info/warning/
        error messages produced during execution, then writes a JSON result
        file for the shell script to pick up.
        """
        from qutebrowser.commands.runners import CommandRunner

        if tab_id_str not in self._tab_data:
            return False

        # Determine win_id from the tab
        tab = self._find_tab(tab_id_str)
        if tab is None:
            return False

        result_path = self._tabs_dir / tab_id_str / 'command-result'
        captured = []

        def _on_message(info):
            captured.append({
                'level': info.level.name,
                'text': info.text,
            })

        def _finish():
            try:
                message.global_bridge.show_message.disconnect(_on_message)
            except TypeError:
                pass
            has_error = any(m['level'] == 'error' for m in captured)
            status = 'error' if has_error else 'ok'
            result = {
                'status': status,
                'command': ':' + command.lstrip(':'),
                'messages': captured,
            }
            try:
                result_path.write_text(json.dumps(result, indent=2) + '\n')
            except FileNotFoundError:
                pass

        message.global_bridge.show_message.connect(_on_message)

        runner = CommandRunner(tab.win_id)
        try:
            runner.run(command, safely=True)
        except Exception as e:
            captured.append({
                'level': 'error',
                'text': str(e),
            })

        if wait_ms > 0:
            QTimer.singleShot(wait_ms, _finish)
        else:
            _finish()

        return True

    def _on_shutdown(self):
        self._tab_data.clear()
        shutil.rmtree(self._tabs_dir, ignore_errors=True)
