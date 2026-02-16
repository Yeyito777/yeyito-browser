# SPDX-FileCopyrightText: Florian Bruhin (The Compiler) <mail@qutebrowser.org>
#
# SPDX-License-Identifier: GPL-3.0-or-later

"""Tab runtime directory manager — exposes live tab state as plain-text files."""

import getpass
import hashlib
import json
import shutil
import stat
import datetime
from pathlib import Path

from qutebrowser.qt.core import QObject, QUrl

from qutebrowser.utils import standarddir, usertypes


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
        self._write_snapshot_script(tab_id)
        self._write_console_script(tab_id)

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
        basedir = str(Path(standarddir.runtime()).parent)
        runtime_dir = standarddir.runtime()
        result_path = self._tabs_dir / tab_id / 'console-result'
        script_path = self._tabs_dir / tab_id / 'console.sh'

        # Compute the IPC socket path (mirrors ipc._get_socketname)
        data_to_hash = f'{getpass.getuser()}-{basedir}'.encode('utf-8')
        md5 = hashlib.md5(data_to_hash).hexdigest()
        socket_path = Path(runtime_dir) / f'ipc-{md5}'

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
            f'SOCKET="{socket_path}"\n'
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
        basedir = str(Path(standarddir.runtime()).parent)
        runtime_dir = standarddir.runtime()
        dom_path = self._tabs_dir / tab_id / 'dom.html'
        script_path = self._tabs_dir / tab_id / 'snapshot-dom.sh'

        # Compute the IPC socket path (mirrors ipc._get_socketname)
        data_to_hash = f'{getpass.getuser()}-{basedir}'.encode('utf-8')
        md5 = hashlib.md5(data_to_hash).hexdigest()
        socket_path = Path(runtime_dir) / f'ipc-{md5}'

        # Build the JSON payload (mirrors ipc.send_to_running_instance)
        payload = json.dumps({
            'args': [f':dom-snapshot {tab_id}'],
            'target_arg': 'tab-silent',
            'protocol_version': 1,
        }) + '\n'

        script_path.write_text(
            '#!/bin/sh\n'
            f'SOCKET="{socket_path}"\n'
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

    def _on_shutdown(self):
        self._tab_data.clear()
        shutil.rmtree(self._tabs_dir, ignore_errors=True)
