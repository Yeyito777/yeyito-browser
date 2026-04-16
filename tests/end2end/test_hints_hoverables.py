# SPDX-FileCopyrightText: Florian Bruhin (The Compiler) <mail@qutebrowser.org>
#
# SPDX-License-Identifier: GPL-3.0-or-later

import pytest


@pytest.mark.usefixtures('quteproc')
def test_hoverables_hint_detects_js_mouse_listeners(quteproc, request):
    if not request.config.webengine:
        pytest.skip('QtWebKit has no main-world hover tracker helper')

    quteproc.open_path('data/hints/hoverables_js_listener.html')

    quteproc.send_cmd(':hint hoverables delete')
    quteproc.wait_for(message='hints: a', category='hints')
    quteproc.send_cmd(':hint-follow a')

    quteproc.send_cmd(
        ':jseval --world main '
        'console.log("hoverables-js-left:" + document.querySelectorAll("#hover-me").length)'
    )
    quteproc.wait_for_js('hoverables-js-left:0')


@pytest.mark.usefixtures('quteproc')
def test_hoverables_hint_detects_react_style_hover_props(quteproc, request):
    if not request.config.webengine:
        pytest.skip('QtWebKit has no main-world hover tracker helper')

    quteproc.open_path('data/hints/hoverables_react_props.html')

    quteproc.send_cmd(':hint hoverables delete')
    quteproc.wait_for(message='hints: a', category='hints')
    quteproc.send_cmd(':hint-follow a')

    quteproc.send_cmd(
        ':jseval --world main '
        'console.log("hoverables-react-left:" + document.querySelectorAll("#react-like").length)'
    )
    quteproc.wait_for_js('hoverables-react-left:0')
