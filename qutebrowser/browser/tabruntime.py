# SPDX-FileCopyrightText: Florian Bruhin (The Compiler) <mail@qutebrowser.org>
#
# SPDX-License-Identifier: GPL-3.0-or-later

"""Tab runtime directory manager — exposes live tab state as plain-text files."""

import json
import shutil
import stat
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

        self._write_open_tab_script()

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
        self._write_snapshot_script(tab_id)
        self._write_console_script(tab_id)
        self._write_network_script(tab_id)
        self._write_command_script(tab_id)
        self._write_screenshot_script(tab_id)
        self._write_click_script(tab_id)
        self._write_wait_script(tab_id)

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

    def _write_console_script(self, tab_id):
        """Write an executable shell script that evals JS in a tab."""
        runtime_dir = standarddir.runtime()
        result_path = self._tabs_dir / tab_id / 'console-result'
        script_path = self._tabs_dir / tab_id / 'console.sh'

        script_path.write_text(
            '#!/bin/sh\n'
            '\n'
            '# Parse arguments\n'
            'TIMEOUT=5\n'
            'JS_EXPR=""\n'
            'while [ $# -gt 0 ]; do\n'
            '    case "$1" in\n'
            '        --timeout)\n'
            '            TIMEOUT="$2"\n'
            '            shift 2\n'
            '            ;;\n'
            '        --timeout=*)\n'
            '            TIMEOUT="${1#--timeout=}"\n'
            '            shift\n'
            '            ;;\n'
            '        *)\n'
            '            JS_EXPR="$1"\n'
            '            shift\n'
            '            ;;\n'
            '    esac\n'
            'done\n'
            '\n'
            'if [ -z "$JS_EXPR" ]; then\n'
            '    echo "Usage: console.sh [--timeout <seconds>] <js-expression>" >&2\n'
            '    exit 1\n'
            'fi\n'
            '\n'
            f'SOCKET="$(ls {runtime_dir}/ipc-* 2>/dev/null | head -1)"\n'
            f'RESULT_FILE="{result_path}"\n'
            f'TAB_ID="{tab_id}"\n'
            'MAX_CHARS=3000\n'
            'INTERVAL=0.5\n'
            'ATTEMPTS=$(awk "BEGIN {printf \\"%d\\", $TIMEOUT / $INTERVAL}")\n'
            '\n'
            'if [ ! -S "$SOCKET" ]; then\n'
            '    echo "Error: IPC socket not found (is qutebrowser running?)" >&2\n'
            '    exit 1\n'
            'fi\n'
            '\n'
            'rm -f "$RESULT_FILE"\n'
            '\n'
            '# Escape backslashes and double quotes for JSON embedding\n'
            'JS_ESC=$(printf \'%s\' "$JS_EXPR" | sed \'s/\\\\/\\\\\\\\/g; s/"/\\\\"/g\')\n'
            'PAYLOAD=$(printf \'{"args":[":js-eval-tab %s %s"],'
            '"target_arg":"tab-silent","protocol_version":1}\''
            ' "$TAB_ID" "$JS_ESC")\n'
            'printf \'%s\\n\' "$PAYLOAD" | socat - UNIX-CONNECT:"$SOCKET"\n'
            '\n'
            'i=0\n'
            'while [ $i -lt $ATTEMPTS ]; do\n'
            '    sleep $INTERVAL\n'
            '    if [ -f "$RESULT_FILE" ]; then\n'
            '        CHAR_COUNT=$(wc -m < "$RESULT_FILE")\n'
            '        if [ "$CHAR_COUNT" -gt "$MAX_CHARS" ]; then\n'
            '            head -c "$MAX_CHARS" "$RESULT_FILE"\n'
            '            printf "\\n...\\n(truncated — full output in console.log)\\n"\n'
            '        else\n'
            '            cat "$RESULT_FILE"\n'
            '        fi\n'
            '        exit 0\n'
            '    fi\n'
            '    i=$((i + 1))\n'
            'done\n'
            '\n'
            'echo "Error: JS eval timed out (${TIMEOUT}s). Use --timeout <seconds> to increase." >&2\n'
            'exit 1\n'
        )
        script_path.chmod(script_path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

    def _write_snapshot_script(self, tab_id):
        """Write an executable shell script that triggers a DOM snapshot."""
        runtime_dir = standarddir.runtime()
        dom_path = self._tabs_dir / tab_id / 'dom.html'
        script_path = self._tabs_dir / tab_id / 'snapshot-dom.sh'

        # Build the JSON payload (mirrors ipc.send_to_running_instance)
        payload = json.dumps({
            'args': [f':dom-snapshot {tab_id}'],
            'target_arg': 'tab-silent',
            'protocol_version': 1,
        }) + '\n'

        script_path.write_text(
            '#!/bin/sh\n'
            f'SOCKET="$(ls {runtime_dir}/ipc-* 2>/dev/null | head -1)"\n'
            f'DOM_FILE="{dom_path}"\n'
            '\n'
            'if [ ! -S "$SOCKET" ]; then\n'
            '    echo "Error: IPC socket not found (is qutebrowser running?)" >&2\n'
            '    exit 1\n'
            'fi\n'
            '\n'
            'rm -f "$DOM_FILE"\n'
            f"printf '%s' '{payload}' | socat - UNIX-CONNECT:\"$SOCKET\"\n"
            '\n'
            'i=0\n'
            'while [ $i -lt 5 ]; do\n'
            '    sleep 1\n'
            '    if [ -f "$DOM_FILE" ]; then\n'
            '        BYTES=$(wc -c < "$DOM_FILE")\n'
            '        CHARS=$(wc -m < "$DOM_FILE")\n'
            '        MB=$(awk "BEGIN {printf \\"%.1f\\", $BYTES / 1048576}")\n'
            '        printf "\\e[96m━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\\e[0m\\n"\n'
            '        printf "\\e[32m  DOM saved to dom.html\\e[0m\\n"\n'
            '        printf "\\e[36m  ${MB}MB \\e[96m│\\e[36m ${CHARS} characters\\e[0m\\n"\n'
            '        if [ "$CHARS" -gt 50000 ]; then\n'
            '            printf "\\e[33m  Consider using tools to parse it to extract\\e[0m\\n"\n'
            '            printf "\\e[33m  the information that you need.\\e[0m\\n"\n'
            '        fi\n'
            '        printf "\\e[96m━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\\e[0m\\n"\n'
            '        exit 0\n'
            '    fi\n'
            '    i=$((i + 1))\n'
            'done\n'
            '\n'
            'echo "Error: DOM snapshot timed out" >&2\n'
            'exit 1\n'
        )
        script_path.chmod(script_path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

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

    def _write_network_script(self, tab_id):
        """Write an executable shell script that queries network data."""
        runtime_dir = standarddir.runtime()
        result_path = self._tabs_dir / tab_id / 'network.json'
        body_path = self._tabs_dir / tab_id / 'network-body'
        script_path = self._tabs_dir / tab_id / 'network.sh'

        script_path.write_text(
            '#!/bin/sh\n'
            '\n'
            '# Parse arguments\n'
            'TIMEOUT=5\n'
            'SUBCMD="list"\n'
            'REQUEST_ID=""\n'
            'FILTER_ERRORS=0\n'
            'FILTER_TYPE=""\n'
            'FILTER_URL=""\n'
            '\n'
            'usage() {\n'
            '    echo "Usage: network.sh [list|detail|body|ws] [options]" >&2\n'
            '    echo "" >&2\n'
            '    echo "Subcommands:" >&2\n'
            '    echo "  list                    List all captured requests (default)" >&2\n'
            '    echo "  detail <request_id>     Full headers/cookies/timing for a request" >&2\n'
            '    echo "  body <request_id>       Response body (raw bytes)" >&2\n'
            '    echo "  ws <request_id>         WebSocket frames" >&2\n'
            '    echo "" >&2\n'
            '    echo "Options:" >&2\n'
            '    echo "  --timeout <seconds>     Query timeout (default: 5)" >&2\n'
            '    echo "  --errors                Filter list: status >= 400 or 0" >&2\n'
            '    echo "  --type <type>           Filter list by resource type (e.g. xhr)" >&2\n'
            '    echo "  --url <pattern>         Filter list by URL pattern (grep)" >&2\n'
            '    exit 1\n'
            '}\n'
            '\n'
            'while [ $# -gt 0 ]; do\n'
            '    case "$1" in\n'
            '        list|detail|body|ws)\n'
            '            SUBCMD="$1"\n'
            '            shift\n'
            '            ;;\n'
            '        --timeout)\n'
            '            TIMEOUT="$2"\n'
            '            shift 2\n'
            '            ;;\n'
            '        --timeout=*)\n'
            '            TIMEOUT="${1#--timeout=}"\n'
            '            shift\n'
            '            ;;\n'
            '        --errors)\n'
            '            FILTER_ERRORS=1\n'
            '            shift\n'
            '            ;;\n'
            '        --type)\n'
            '            FILTER_TYPE="$2"\n'
            '            shift 2\n'
            '            ;;\n'
            '        --type=*)\n'
            '            FILTER_TYPE="${1#--type=}"\n'
            '            shift\n'
            '            ;;\n'
            '        --url)\n'
            '            FILTER_URL="$2"\n'
            '            shift 2\n'
            '            ;;\n'
            '        --url=*)\n'
            '            FILTER_URL="${1#--url=}"\n'
            '            shift\n'
            '            ;;\n'
            '        -h|--help)\n'
            '            usage\n'
            '            ;;\n'
            '        *)\n'
            '            if [ -z "$REQUEST_ID" ]; then\n'
            '                REQUEST_ID="$1"\n'
            '            else\n'
            '                echo "Error: unexpected argument: $1" >&2\n'
            '                usage\n'
            '            fi\n'
            '            shift\n'
            '            ;;\n'
            '    esac\n'
            'done\n'
            '\n'
            '# Validate subcommand args\n'
            'case "$SUBCMD" in\n'
            '    detail|body|ws)\n'
            '        if [ -z "$REQUEST_ID" ]; then\n'
            '            echo "Error: $SUBCMD requires a <request_id> argument" >&2\n'
            '            usage\n'
            '        fi\n'
            '        ;;\n'
            'esac\n'
            '\n'
            f'SOCKET="$(ls {runtime_dir}/ipc-* 2>/dev/null | head -1)"\n'
            f'RESULT_FILE="{result_path}"\n'
            f'BODY_FILE="{body_path}"\n'
            f'TAB_ID="{tab_id}"\n'
            f'TAB_DIR="{self._tabs_dir / tab_id}"\n'
            'INTERVAL=0.5\n'
            'ATTEMPTS=$(awk "BEGIN {printf \\"%d\\", $TIMEOUT / $INTERVAL}")\n'
            '\n'
            'if [ ! -S "$SOCKET" ]; then\n'
            '    echo "Error: IPC socket not found (is qutebrowser running?)" >&2\n'
            '    exit 1\n'
            'fi\n'
            '\n'
            '# Map subcommand to IPC command\n'
            'case "$SUBCMD" in\n'
            '    list)\n'
            '        rm -f "$RESULT_FILE"\n'
            '        POLL_FILE="$RESULT_FILE"\n'
            '        CMD=":network-list $TAB_ID"\n'
            '        ;;\n'
            '    detail)\n'
            '        POLL_FILE="$TAB_DIR/request-${REQUEST_ID}.json"\n'
            '        rm -f "$POLL_FILE"\n'
            '        CMD=":network-detail $TAB_ID $REQUEST_ID"\n'
            '        ;;\n'
            '    body)\n'
            '        rm -f "$BODY_FILE"\n'
            '        POLL_FILE="$BODY_FILE"\n'
            '        CMD=":network-body $TAB_ID $REQUEST_ID"\n'
            '        ;;\n'
            '    ws)\n'
            '        rm -f "$RESULT_FILE"\n'
            '        POLL_FILE="$RESULT_FILE"\n'
            '        CMD=":network-ws-frames $TAB_ID $REQUEST_ID"\n'
            '        ;;\n'
            'esac\n'
            '\n'
            'PAYLOAD=$(printf \'{"args":["%s"],"target_arg":"tab-silent","protocol_version":1}\' "$CMD")\n'
            'printf \'%s\\n\' "$PAYLOAD" | socat - UNIX-CONNECT:"$SOCKET"\n'
            '\n'
            'i=0\n'
            'while [ $i -lt $ATTEMPTS ]; do\n'
            '    sleep $INTERVAL\n'
            '    if [ -f "$POLL_FILE" ]; then\n'
            '        # Apply client-side filters and write back for list subcommand\n'
            '        if [ "$SUBCMD" = "list" ]; then\n'
            '            OUTPUT=$(cat "$POLL_FILE")\n'
            '            if [ "$FILTER_ERRORS" = "1" ] && command -v jq >/dev/null 2>&1; then\n'
            '                OUTPUT=$(echo "$OUTPUT" | jq \'{requests: [.requests[] | select(.status >= 400 or .status == 0)], count: ([.requests[] | select(.status >= 400 or .status == 0)] | length)}\')\n'
            '            fi\n'
            '            if [ -n "$FILTER_TYPE" ] && command -v jq >/dev/null 2>&1; then\n'
            '                OUTPUT=$(echo "$OUTPUT" | jq --arg t "$FILTER_TYPE" \'{requests: [.requests[] | select(.type == $t)], count: ([.requests[] | select(.type == $t)] | length)}\')\n'
            '            fi\n'
            '            if [ -n "$FILTER_URL" ]; then\n'
            '                if command -v jq >/dev/null 2>&1; then\n'
            '                    OUTPUT=$(echo "$OUTPUT" | jq --arg p "$FILTER_URL" \'{requests: [.requests[] | select(.url | test($p))], count: ([.requests[] | select(.url | test($p))] | length)}\')\n'
            '                fi\n'
            '            fi\n'
            '            # Write filtered result back to file\n'
            '            printf \'%s\\n\' "$OUTPUT" > "$POLL_FILE"\n'
            '        fi\n'
            '\n'
            '        BYTES=$(wc -c < "$POLL_FILE")\n'
            '        KB=$(awk "BEGIN {printf \\"%.1f\\", $BYTES / 1024}")\n'
            '\n'
            '        # Pretty-print summary\n'
            '        printf "\\e[96m━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\\e[0m\\n"\n'
            '        case "$SUBCMD" in\n'
            '            list)\n'
            '                printf "\\e[32m  Network data saved to network.json\\e[0m\\n"\n'
            '                if command -v jq >/dev/null 2>&1; then\n'
            '                    COUNT=$(echo "$OUTPUT" | jq -r ".count // 0")\n'
            '                    printf "\\e[36m  ${KB}KB \\e[96m│\\e[36m ${COUNT} requests\\e[0m\\n"\n'
            '                    # Type breakdown\n'
            '                    TYPES=$(echo "$OUTPUT" | jq -r \'[.requests[].type] | group_by(.) | map("\\(length) \\(.[0])") | join(", ")\')\n'
            '                    if [ -n "$TYPES" ]; then\n'
            '                        printf "\\e[36m  %s\\e[0m\\n" "$TYPES"\n'
            '                    fi\n'
            '                    # Error summary\n'
            '                    ERR_COUNT=$(echo "$OUTPUT" | jq \'[.requests[] | select(.status >= 400 or (.status == 0 and (.netError // 0) != 0))] | length\')\n'
            '                    if [ "$ERR_COUNT" -gt 0 ] 2>/dev/null; then\n'
            '                        printf "\\e[33m  %s request(s) with errors\\e[0m\\n" "$ERR_COUNT"\n'
            '                    fi\n'
            '                else\n'
            '                    printf "\\e[36m  ${KB}KB\\e[0m\\n"\n'
            '                fi\n'
            '                ;;\n'
            '            detail)\n'
            '                printf "\\e[32m  Request saved to request-${REQUEST_ID}.json\\e[0m\\n"\n'
            '                if command -v jq >/dev/null 2>&1; then\n'
            '                    DETAIL=$(cat "$POLL_FILE")\n'
            '                    D_URL=$(echo "$DETAIL" | jq -r ".url // \\"-\\"")\n'
            '                    D_STATUS=$(echo "$DETAIL" | jq -r ".status // 0")\n'
            '                    D_METHOD=$(echo "$DETAIL" | jq -r ".method // \\"-\\"")\n'
            '                    D_TYPE=$(echo "$DETAIL" | jq -r ".type // \\"-\\"")\n'
            '                    D_SIZE=$(echo "$DETAIL" | jq -r ".rawBodyBytes // 0")\n'
            '                    D_CACHED=$(echo "$DETAIL" | jq -r ".cached // false")\n'
            '                    D_REMOTE=$(echo "$DETAIL" | jq -r ".remoteEndpoint // \\"-\\"")\n'
            '                    # Truncate URL for display\n'
            '                    D_URL_SHORT=$(printf "%.60s" "$D_URL")\n'
            '                    [ ${#D_URL} -gt 60 ] && D_URL_SHORT="${D_URL_SHORT}..."\n'
            '                    printf "\\e[36m  ${KB}KB \\e[96m│\\e[36m ${D_METHOD} ${D_STATUS} ${D_TYPE}\\e[0m\\n"\n'
            '                    printf "\\e[36m  %s\\e[0m\\n" "$D_URL_SHORT"\n'
            '                    # Body size and response header count\n'
            '                    BODY_LEN=$(echo "$DETAIL" | jq -r \'.body | length // 0\')\n'
            '                    BODY_KB=$(awk "BEGIN {printf \\"%.1f\\", $BODY_LEN / 1024}")\n'
            '                    HDR_COUNT=$(echo "$DETAIL" | jq \'.responseHeaders | length // 0\')\n'
            '                    BODY_ERR=$(echo "$DETAIL" | jq -r \'.bodyError // empty\')\n'
            '                    if [ -n "$BODY_ERR" ]; then\n'
            '                        printf "\\e[33m  body: %s\\e[0m\\n" "$BODY_ERR"\n'
            '                    else\n'
            '                        printf "\\e[36m  ${BODY_KB}KB body \\e[96m│\\e[36m ${HDR_COUNT} response headers"\n'
            '                        if [ "$D_CACHED" = "true" ]; then\n'
            '                            printf " (cached)"\n'
            '                        fi\n'
            '                        printf "\\e[0m\\n"\n'
            '                    fi\n'
            '                    if [ "$D_REMOTE" != "-" ]; then\n'
            '                        printf "\\e[36m  %s\\e[0m\\n" "$D_REMOTE"\n'
            '                    fi\n'
            '                    # Timing summary\n'
            '                    DNS_MS=$(echo "$DETAIL" | jq ".timing.dnsEndMs - .timing.dnsStartMs")\n'
            '                    CONNECT_MS=$(echo "$DETAIL" | jq ".timing.connectEndMs - .timing.connectStartMs")\n'
            '                    SSL_MS=$(echo "$DETAIL" | jq ".timing.sslEndMs - .timing.sslStartMs")\n'
            '                    TTFB_MS=$(echo "$DETAIL" | jq ".timing.receiveHeadersStartMs - .timing.sendEndMs")\n'
            '                    HAS_TIMING=$(echo "$DETAIL" | jq ".timing.sendEndMs > 0")\n'
            '                    if [ "$HAS_TIMING" = "true" ]; then\n'
            '                        DNS_I=$(printf "%.0f" "$DNS_MS")\n'
            '                        CONN_I=$(printf "%.0f" "$CONNECT_MS")\n'
            '                        SSL_I=$(printf "%.0f" "$SSL_MS")\n'
            '                        TTFB_I=$(printf "%.0f" "$TTFB_MS")\n'
            '                        printf "\\e[36m  dns ${DNS_I}ms \\e[96m│\\e[36m tcp ${CONN_I}ms \\e[96m│\\e[36m tls ${SSL_I}ms \\e[96m│\\e[36m ttfb ${TTFB_I}ms\\e[0m\\n"\n'
            '                    fi\n'
            '                else\n'
            '                    printf "\\e[36m  ${KB}KB\\e[0m\\n"\n'
            '                fi\n'
            '                ;;\n'
            '            body)\n'
            '                printf "\\e[32m  Response body saved to network-body\\e[0m\\n"\n'
            '                printf "\\e[36m  ${KB}KB\\e[0m\\n"\n'
            '                ;;\n'
            '            ws)\n'
            '                printf "\\e[32m  WebSocket data saved to network.json\\e[0m\\n"\n'
            '                printf "\\e[36m  ${KB}KB\\e[0m\\n"\n'
            '                ;;\n'
            '        esac\n'
            '        printf "\\e[96m━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\\e[0m\\n"\n'
            '        exit 0\n'
            '    fi\n'
            '    i=$((i + 1))\n'
            'done\n'
            '\n'
            'echo "Error: network query timed out (${TIMEOUT}s). Use --timeout <seconds> to increase." >&2\n'
            'exit 1\n'
        )
        script_path.chmod(
            script_path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

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

    def _write_command_script(self, tab_id):
        """Write an executable shell script that runs qutebrowser commands."""
        runtime_dir = standarddir.runtime()
        result_path = self._tabs_dir / tab_id / 'command-result'
        script_path = self._tabs_dir / tab_id / 'command.sh'

        script_path.write_text(
            '#!/bin/sh\n'
            '\n'
            '# Parse arguments\n'
            'TIMEOUT=10\n'
            'WAIT=0\n'
            'CMD_ARGS=""\n'
            'while [ $# -gt 0 ]; do\n'
            '    case "$1" in\n'
            '        --timeout)\n'
            '            TIMEOUT="$2"\n'
            '            shift 2\n'
            '            ;;\n'
            '        --timeout=*)\n'
            '            TIMEOUT="${1#--timeout=}"\n'
            '            shift\n'
            '            ;;\n'
            '        --wait)\n'
            '            WAIT="$2"\n'
            '            shift 2\n'
            '            ;;\n'
            '        --wait=*)\n'
            '            WAIT="${1#--wait=}"\n'
            '            shift\n'
            '            ;;\n'
            '        -h|--help)\n'
            '            echo "Usage: command.sh [--timeout <seconds>] [--wait <ms>] <command> [args...]" >&2\n'
            '            echo "" >&2\n'
            '            echo "Run a qutebrowser command and capture output." >&2\n'
            '            echo "" >&2\n'
            '            echo "Options:" >&2\n'
            '            echo "  --timeout <seconds>  Poll timeout (default: 10)" >&2\n'
            '            echo "  --wait <ms>          Async capture window (default: 0)" >&2\n'
            '            echo "" >&2\n'
            '            echo "Examples:" >&2\n'
            '            echo "  command.sh config-source" >&2\n'
            '            echo "  command.sh open -t https://example.com" >&2\n'
            '            echo "  command.sh set content.javascript.enabled false" >&2\n'
            '            echo "  command.sh --wait 500 config-source" >&2\n'
            '            exit 0\n'
            '            ;;\n'
            '        *)\n'
            '            if [ -z "$CMD_ARGS" ]; then\n'
            '                CMD_ARGS="$1"\n'
            '            else\n'
            '                CMD_ARGS="$CMD_ARGS $1"\n'
            '            fi\n'
            '            shift\n'
            '            ;;\n'
            '    esac\n'
            'done\n'
            '\n'
            'if [ -z "$CMD_ARGS" ]; then\n'
            '    echo "Usage: command.sh [--timeout <seconds>] [--wait <ms>] <command> [args...]" >&2\n'
            '    exit 1\n'
            'fi\n'
            '\n'
            '# Prepend : if not already present\n'
            'case "$CMD_ARGS" in\n'
            '    :*) ;;\n'
            '    *)  CMD_ARGS=":$CMD_ARGS" ;;\n'
            'esac\n'
            '\n'
            f'SOCKET="$(ls {runtime_dir}/ipc-* 2>/dev/null | head -1)"\n'
            f'RESULT_FILE="{result_path}"\n'
            f'TAB_ID="{tab_id}"\n'
            'INTERVAL=0.5\n'
            'ATTEMPTS=$(awk "BEGIN {printf \\"%d\\", $TIMEOUT / $INTERVAL}")\n'
            '\n'
            'if [ ! -S "$SOCKET" ]; then\n'
            '    echo "Error: IPC socket not found (is qutebrowser running?)" >&2\n'
            '    exit 1\n'
            'fi\n'
            '\n'
            'rm -f "$RESULT_FILE"\n'
            '\n'
            '# Escape backslashes and double quotes for JSON embedding\n'
            'CMD_ESC=$(printf \'%s\' "$CMD_ARGS" | sed \'s/\\\\/\\\\\\\\/g; s/"/\\\\"/g\')\n'
            'PAYLOAD=$(printf \'{"args":[":command-eval %s %s %s"],'
            '"target_arg":"tab-silent","protocol_version":1}\''
            ' "$TAB_ID" "$WAIT" "$CMD_ESC")\n'
            'printf \'%s\\n\' "$PAYLOAD" | socat - UNIX-CONNECT:"$SOCKET"\n'
            '\n'
            'i=0\n'
            'while [ $i -lt $ATTEMPTS ]; do\n'
            '    sleep $INTERVAL\n'
            '    if [ -f "$RESULT_FILE" ]; then\n'
            '        # Parse and format output\n'
            '        if command -v jq >/dev/null 2>&1; then\n'
            '            STATUS=$(jq -r .status "$RESULT_FILE")\n'
            '            COMMAND=$(jq -r .command "$RESULT_FILE")\n'
            '            printf "%s — %s\\n" "$COMMAND" "$STATUS"\n'
            '            jq -r \'.messages[] | "[\\(.level)] \\(.text)"\' "$RESULT_FILE"\n'
            '            if [ "$STATUS" = "error" ]; then\n'
            '                exit 1\n'
            '            fi\n'
            '        else\n'
            '            cat "$RESULT_FILE"\n'
            '        fi\n'
            '        exit 0\n'
            '    fi\n'
            '    i=$((i + 1))\n'
            'done\n'
            '\n'
            'echo "Error: command timed out (${TIMEOUT}s). Use --timeout <seconds> to increase." >&2\n'
            'exit 1\n'
        )
        script_path.chmod(
            script_path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

    def _write_screenshot_script(self, tab_id):
        """Write an executable shell script that captures a tab screenshot."""
        runtime_dir = standarddir.runtime()
        screenshot_path = self._tabs_dir / tab_id / 'screenshot.png'
        script_path = self._tabs_dir / tab_id / 'screenshot.sh'

        script_path.write_text(
            '#!/bin/sh\n'
            '\n'
            '# Parse arguments\n'
            'TIMEOUT=10\n'
            'WINDOW=0\n'
            'while [ $# -gt 0 ]; do\n'
            '    case "$1" in\n'
            '        --window)\n'
            '            WINDOW=1\n'
            '            shift\n'
            '            ;;\n'
            '        --timeout)\n'
            '            TIMEOUT="$2"\n'
            '            shift 2\n'
            '            ;;\n'
            '        --timeout=*)\n'
            '            TIMEOUT="${1#--timeout=}"\n'
            '            shift\n'
            '            ;;\n'
            '        -h|--help)\n'
            '            echo "Usage: screenshot.sh [--window] [--timeout <s>]" >&2\n'
            '            echo "" >&2\n'
            '            echo "Options:" >&2\n'
            '            echo "  --window              Grab the whole window instead of tab content" >&2\n'
            '            echo "  --timeout <seconds>   Poll timeout (default: 10)" >&2\n'
            '            exit 0\n'
            '            ;;\n'
            '        *)\n'
            '            echo "Error: unknown argument: $1" >&2\n'
            '            exit 1\n'
            '            ;;\n'
            '    esac\n'
            'done\n'
            '\n'
            f'SOCKET="$(ls {runtime_dir}/ipc-* 2>/dev/null | head -1)"\n'
            f'SCREENSHOT_FILE="{screenshot_path}"\n'
            f'TAB_ID="{tab_id}"\n'
            'INTERVAL=0.5\n'
            'ATTEMPTS=$(awk "BEGIN {printf \\"%d\\", $TIMEOUT / $INTERVAL}")\n'
            '\n'
            'if [ ! -S "$SOCKET" ]; then\n'
            '    echo "Error: IPC socket not found (is qutebrowser running?)" >&2\n'
            '    exit 1\n'
            'fi\n'
            '\n'
            'rm -f "$SCREENSHOT_FILE"\n'
            '\n'
            '# Build IPC command\n'
            'if [ "$WINDOW" = "1" ]; then\n'
            '    CMD=":tab-screenshot $TAB_ID --window"\n'
            'else\n'
            '    CMD=":tab-screenshot $TAB_ID"\n'
            'fi\n'
            '\n'
            'PAYLOAD=$(printf \'{"args":["%s"],"target_arg":"tab-silent","protocol_version":1}\' "$CMD")\n'
            'printf \'%s\\n\' "$PAYLOAD" | socat - UNIX-CONNECT:"$SOCKET"\n'
            '\n'
            'i=0\n'
            'while [ $i -lt $ATTEMPTS ]; do\n'
            '    sleep $INTERVAL\n'
            '    if [ -f "$SCREENSHOT_FILE" ]; then\n'
            '        BYTES=$(wc -c < "$SCREENSHOT_FILE")\n'
            '        KB=$(awk "BEGIN {printf \\"%.1f\\", $BYTES / 1024}")\n'
            '        RES=$(file "$SCREENSHOT_FILE" | grep -oP \'\\d+ x \\d+\' || echo "unknown")\n'
            '        printf "\\e[96m━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\\e[0m\\n"\n'
            '        printf "\\e[32m  Screenshot saved to screenshot.png\\e[0m\\n"\n'
            '        printf "\\e[36m  ${KB}KB \\e[96m│\\e[36m ${RES}\\e[0m\\n"\n'
            '        printf "\\e[96m━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\\e[0m\\n"\n'
            '        exit 0\n'
            '    fi\n'
            '    i=$((i + 1))\n'
            'done\n'
            '\n'
            'echo "Error: screenshot timed out (${TIMEOUT}s). Use --timeout <seconds> to increase." >&2\n'
            'exit 1\n'
        )
        script_path.chmod(
            script_path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

    def _write_click_script(self, tab_id):
        """Write an executable shell script that clicks elements by CSS selector or coords."""
        console_path = self._tabs_dir / tab_id / 'console.sh'
        command_path = self._tabs_dir / tab_id / 'command.sh'
        script_path = self._tabs_dir / tab_id / 'click.sh'

        SQ = "'"  # shell single-quote — avoids escaping nightmares in Python strings

        script = f"""#!/bin/sh

# click.sh — Click an element by CSS selector or coordinates
#
# Uses qutebrowser :fake-click to send real QMouseEvents through
# the browser engine, triggering framework event handlers (React, etc.)
# that ignore synthetic DOM .click() calls.
#
# Usage:
#   ./click.sh <css-selector>              Click first match
#   ./click.sh --all <css-selector>         Click all matches
#   ./click.sh --coords <x> <y>             Click at viewport coordinates
#   ./click.sh --js <css-selector>           Click via JS .click() (old behavior)
#   ./click.sh --timeout <s> <css-selector>  Custom timeout

TIMEOUT=5
SELECTOR=""
COORDS_X=""
COORDS_Y=""
ALL=0
JS_MODE=0

while [ $# -gt 0 ]; do
    case "$1" in
        --timeout) TIMEOUT="$2"; shift 2 ;;
        --timeout=*) TIMEOUT="${{1#--timeout=}}"; shift ;;
        --coords) COORDS_X="$2"; COORDS_Y="$3"; shift 3 ;;
        --all) ALL=1; shift ;;
        --js) JS_MODE=1; shift ;;
        -h|--help)
            echo "Usage: click.sh [options] <css-selector>" >&2
            echo "" >&2
            echo "Options:" >&2
            echo "  --coords <x> <y>   Click at viewport coordinates (real event)" >&2
            echo "  --all               Click all matching elements" >&2
            echo "  --js                Use JS .click() instead of real events" >&2
            echo "  --timeout <s>       Timeout (default: 5)" >&2
            exit 0
            ;;
        *) SELECTOR="$1"; shift ;;
    esac
done

CONSOLE="{console_path}"
COMMAND="{command_path}"

# --- coords mode: send real click at viewport (x, y) ---
if [ -n "$COORDS_X" ] && [ -n "$COORDS_Y" ]; then
    # Identify element for reporting, then send real click
    JS="(function(){{var el=document.elementFromPoint($COORDS_X,$COORDS_Y);if(!el)return {SQ}Error: no element at ({SQ}+$COORDS_X+{SQ},{SQ}+$COORDS_Y+{SQ}){SQ};return {SQ}Clicked: {SQ}+el.tagName+(el.id?{SQ}#{SQ}+el.id:{SQ}{SQ})+(el.getAttribute({SQ}class{SQ})?{SQ}.{SQ}+el.getAttribute({SQ}class{SQ}).split({SQ} {SQ})[0]:{SQ}{SQ});}})()"
    RESULT=$("$CONSOLE" --timeout "$TIMEOUT" "$JS" 2>&1)
    if echo "$RESULT" | grep -q "^=> Error:"; then
        echo "$RESULT"
        exit 1
    fi
    "$COMMAND" --timeout "$TIMEOUT" fake-click "$COORDS_X" "$COORDS_Y" >/dev/null 2>&1
    echo "$RESULT"
    exit 0
fi

if [ -z "$SELECTOR" ]; then
    echo "Usage: click.sh [--coords X Y | --all] <css-selector>" >&2
    exit 1
fi

# Escape selector for JS string embedding
SEL_ESC=$(printf {SQ}%s{SQ} "$SELECTOR" | sed "s/\\\\\\\\/\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\/g; s/{SQ}/\\\\\\\\{SQ}/g")

if [ "$ALL" = "1" ]; then
    # --all always uses JS .click() (can't send multiple real clicks reliably)
    JS="(function(){{var els=document.querySelectorAll({SQ}${{SEL_ESC}}{SQ});if(!els.length)return {SQ}Error: no elements match selector{SQ};var c=0;els.forEach(function(el){{el.scrollIntoView({{block:{SQ}center{SQ},behavior:{SQ}instant{SQ}}});el.click();c++;}});return {SQ}Clicked {SQ}+c+{SQ} element(s){SQ};}})()"
    exec "$CONSOLE" --timeout "$TIMEOUT" "$JS"
fi

# --- single selector mode ---
if [ "$JS_MODE" = "1" ]; then
    # Legacy JS .click() path
    JS="(function(){{var el=document.querySelector({SQ}${{SEL_ESC}}{SQ});if(!el)return {SQ}Error: no element matches selector{SQ};el.scrollIntoView({{block:{SQ}center{SQ},behavior:{SQ}instant{SQ}}});el.click();var desc=el.tagName;if(el.id)desc+={SQ}#{SQ}+el.id;var txt=(el.textContent||{SQ}{SQ}).trim().substring(0,50);if(txt)desc+={SQ} text={SQ}+txt;return {SQ}Clicked: {SQ}+desc;}})()"
    exec "$CONSOLE" --timeout "$TIMEOUT" "$JS"
fi

# Real click: resolve element to viewport coords, scroll into view, then :fake-click
JS="(function(){{var el=document.querySelector({SQ}${{SEL_ESC}}{SQ});if(!el)return {SQ}Error: no element matches selector{SQ};el.scrollIntoView({{block:{SQ}center{SQ},behavior:{SQ}instant{SQ}}});var r=el.getBoundingClientRect();var cx=Math.round(r.left+r.width/2);var cy=Math.round(r.top+r.height/2);var desc=el.tagName;if(el.id)desc+={SQ}#{SQ}+el.id;var t=(el.textContent||{SQ}{SQ}).trim();if(t)desc+={SQ} «{SQ}+t.substring(0,30)+{SQ}»{SQ};return cx+{SQ},{SQ}+cy+{SQ}|{SQ}+desc;}})()"

RESULT=$("$CONSOLE" --timeout "$TIMEOUT" "$JS" 2>&1)

# Check for error
if echo "$RESULT" | grep -q "^=> Error:"; then
    echo "$RESULT"
    exit 1
fi

# Parse "=> X,Y|description"
PAYLOAD=$(echo "$RESULT" | sed {SQ}s/^=> //{SQ})
COORD_PART=$(echo "$PAYLOAD" | cut -d{SQ}|{SQ} -f1)
DESC_PART=$(echo "$PAYLOAD" | cut -d{SQ}|{SQ} -f2-)
CX=$(echo "$COORD_PART" | cut -d{SQ},{SQ} -f1)
CY=$(echo "$COORD_PART" | cut -d{SQ},{SQ} -f2)

"$COMMAND" --timeout "$TIMEOUT" fake-click "$CX" "$CY" >/dev/null 2>&1
echo "=> Clicked: $DESC_PART"
"""
        script_path.write_text(script)
        script_path.chmod(
            script_path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

    def _write_wait_script(self, tab_id):
        """Write an executable shell script that waits for page load or a selector."""
        tab_data_path = self._tabs_dir / tab_id / 'tab-data.info'
        console_path = self._tabs_dir / tab_id / 'console.sh'
        script_path = self._tabs_dir / tab_id / 'wait.sh'

        SQ = "'"  # shell single-quote

        script = f"""#!/bin/sh

# wait.sh — Wait for page load or an element to appear
#
# Usage:
#   ./wait.sh --load                       Wait for page to finish loading
#   ./wait.sh --selector <css-selector>     Wait for element to appear in DOM
#   ./wait.sh --timeout <s> --load          Custom timeout

TIMEOUT=15
MODE=""
SELECTOR=""

while [ $# -gt 0 ]; do
    case "$1" in
        --timeout) TIMEOUT="$2"; shift 2 ;;
        --timeout=*) TIMEOUT="${{1#--timeout=}}"; shift ;;
        --load) MODE="load"; shift ;;
        --selector) MODE="selector"; SELECTOR="$2"; shift 2 ;;
        -h|--help)
            echo "Usage: wait.sh [options]" >&2
            echo "" >&2
            echo "Modes:" >&2
            echo "  --load                  Wait for page load to complete" >&2
            echo "  --selector <selector>   Wait for CSS selector to exist in DOM" >&2
            echo "" >&2
            echo "Options:" >&2
            echo "  --timeout <seconds>     Timeout (default: 15)" >&2
            exit 0
            ;;
        *)
            echo "Error: unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

if [ -z "$MODE" ]; then
    echo "Usage: wait.sh --load | --selector <selector>" >&2
    exit 1
fi

TAB_DATA="{tab_data_path}"
CONSOLE="{console_path}"
INTERVAL=0.3
ATTEMPTS=$(awk "BEGIN {{printf \\"%d\\", $TIMEOUT / $INTERVAL}}")

if [ "$MODE" = "load" ]; then
    i=0
    while [ $i -lt $ATTEMPTS ]; do
        STATUS=$(grep "^load_status:" "$TAB_DATA" 2>/dev/null | cut -d" " -f2-)
        case "$STATUS" in
            success|success_https)
                echo "loaded ($STATUS)"
                exit 0
                ;;
            error)
                echo "load error" >&2
                exit 1
                ;;
        esac
        sleep $INTERVAL
        i=$((i + 1))
    done
    echo "Error: load timed out (${{TIMEOUT}}s)" >&2
    exit 1
fi

if [ "$MODE" = "selector" ]; then
    SEL_ESC=$(printf {SQ}%s{SQ} "$SELECTOR" | sed "s/\\\\\\\\/\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\/g; s/{SQ}/\\\\\\\\{SQ}/g")
    i=0
    while [ $i -lt $ATTEMPTS ]; do
        RESULT=$($CONSOLE --timeout 3 "document.querySelector({SQ}${{SEL_ESC}}{SQ}) ? {SQ}found{SQ} : {SQ}not-found{SQ}" 2>/dev/null)
        case "$RESULT" in
            *found*)
                if echo "$RESULT" | grep -qv "not-found"; then
                    echo "found"
                    exit 0
                fi
                ;;
        esac
        sleep $INTERVAL
        i=$((i + 1))
    done
    echo "Error: selector timed out (${{TIMEOUT}}s)" >&2
    exit 1
fi
"""
        script_path.write_text(script)
        script_path.chmod(
            script_path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

    def _write_open_tab_script(self):
        """Write an executable shell script at tabs/ level that opens a URL in a new tab."""
        runtime_dir = standarddir.runtime()
        script_path = self._tabs_dir / 'open-tab.sh'
        order_path = self._tabs_dir / 'order'

        SQ = "'"  # shell single-quote

        script = f"""#!/bin/sh

# open-tab.sh — Open a URL in a new tab and return the new tab ID
#
# Usage:
#   ./open-tab.sh <url>
#   ./open-tab.sh --timeout <s> <url>
#
# Outputs the new tab ID on stdout. The tab directory will be at:
#   <tabs-dir>/<tab-id>/

TIMEOUT=10
URL=""

while [ $# -gt 0 ]; do
    case "$1" in
        --timeout) TIMEOUT="$2"; shift 2 ;;
        --timeout=*) TIMEOUT="${{1#--timeout=}}"; shift ;;
        -h|--help)
            echo "Usage: open-tab.sh [--timeout <s>] <url>" >&2
            exit 0
            ;;
        *) URL="$1"; shift ;;
    esac
done

if [ -z "$URL" ]; then
    echo "Usage: open-tab.sh <url>" >&2
    exit 1
fi

SOCKET="$(ls {runtime_dir}/ipc-* 2>/dev/null | head -1)"
ORDER_FILE="{order_path}"
TABS_DIR="{self._tabs_dir}"
INTERVAL=0.3
ATTEMPTS=$(awk "BEGIN {{printf \\"%d\\", $TIMEOUT / $INTERVAL}}")

if [ ! -S "$SOCKET" ]; then
    echo "Error: IPC socket not found (is qutebrowser running?)" >&2
    exit 1
fi

# Snapshot current tab IDs
BEFORE=$(cat "$ORDER_FILE" 2>/dev/null || echo "")

# Send open command
CMD=":open -t $URL"
PAYLOAD=$(printf {SQ}{{"args":["%s"],"target_arg":"tab-silent","protocol_version":1}}{SQ} "$CMD")
printf {SQ}%s\\n{SQ} "$PAYLOAD" | socat - UNIX-CONNECT:"$SOCKET"

# Poll for new tab ID
i=0
while [ $i -lt $ATTEMPTS ]; do
    sleep $INTERVAL
    AFTER=$(cat "$ORDER_FILE" 2>/dev/null || echo "")
    # Find IDs in AFTER that are not in BEFORE
    NEW_ID=$(echo "$AFTER" | while read -r tid; do
        if [ -n "$tid" ] && ! echo "$BEFORE" | grep -qxF "$tid"; then
            echo "$tid"
            break
        fi
    done)
    if [ -n "$NEW_ID" ]; then
        echo "$NEW_ID"
        exit 0
    fi
    i=$((i + 1))
done

echo "Error: timed out waiting for new tab (${{TIMEOUT}}s)" >&2
exit 1
"""
        script_path.write_text(script)
        script_path.chmod(
            script_path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

    def _on_shutdown(self):
        self._tab_data.clear()
        shutil.rmtree(self._tabs_dir, ignore_errors=True)
