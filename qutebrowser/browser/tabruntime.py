# SPDX-FileCopyrightText: Florian Bruhin (The Compiler) <mail@qutebrowser.org>
#
# SPDX-License-Identifier: GPL-3.0-or-later

"""Tab runtime directory manager — exposes live tab state as plain-text files."""

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

    def _write_snapshot_script(self, tab_id):
        """Write an executable shell script that triggers a DOM snapshot."""
        basedir = Path(standarddir.runtime()).parent
        dom_path = self._tabs_dir / tab_id / 'dom.html'
        script_path = self._tabs_dir / tab_id / 'snapshot-dom.sh'
        script_path.write_text(
            '#!/bin/sh\n'
            f'BASEDIR="{basedir}"\n'
            f'TAB_ID="{tab_id}"\n'
            f'DOM_FILE="{dom_path}"\n'
            '\n'
            'rm -f "$DOM_FILE"\n'
            'qutebrowser --basedir "$BASEDIR" --target tab-silent ":dom-snapshot $TAB_ID"\n'
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
