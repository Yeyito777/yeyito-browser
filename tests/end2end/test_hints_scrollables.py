# SPDX-FileCopyrightText: Florian Bruhin (The Compiler) <mail@qutebrowser.org>
#
# SPDX-License-Identifier: GPL-3.0-or-later


def test_scrollables_hint_uses_nested_page_scroller(quteproc):
    """Prefer the real viewport-sized scroller over a non-scrollable root."""
    quteproc.open_path('data/hints/scrollables_root_container.html')

    quteproc.send_cmd(':hint scrollables focus')
    quteproc.wait_for(message='hints: f', category='hints')

    quteproc.send_cmd(':hint-follow f')
    quteproc.send_cmd(
        ':jseval --world main '
        'console.log("active-scrollable:" + document.activeElement.id)'
    )
    quteproc.wait_for_js('active-scrollable:scroll-root')
