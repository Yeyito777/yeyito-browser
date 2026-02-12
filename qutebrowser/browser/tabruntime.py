# SPDX-FileCopyrightText: Florian Bruhin (The Compiler) <mail@qutebrowser.org>
#
# SPDX-License-Identifier: GPL-3.0-or-later

"""Tab runtime directory manager — exposes live tab state as plain-text files."""

import shutil
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
        order_file.write_text('\n'.join(tab_ids) + '\n' if tab_ids else '')

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

    def _on_shutdown(self):
        self._tab_data.clear()
        shutil.rmtree(self._tabs_dir, ignore_errors=True)
