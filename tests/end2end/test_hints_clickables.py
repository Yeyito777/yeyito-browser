# SPDX-FileCopyrightText: Florian Bruhin (The Compiler) <mail@qutebrowser.org>
#
# SPDX-License-Identifier: GPL-3.0-or-later

import pytest


@pytest.mark.usefixtures('quteproc')
def test_all_hints_detect_js_click_listeners(quteproc, request):
    if not request.config.webengine:
        pytest.skip('QtWebKit has no main-world click tracker helper')

    quteproc.open_path('data/hints/clickables_js_listener.html')

    quteproc.send_cmd(':hint all delete')
    quteproc.wait_for(message='hints: a', category='hints')
    quteproc.send_cmd(':hint-follow a')

    quteproc.send_cmd(
        ':jseval --world main '
        'console.log('
        '"clickables-js-left:" + '
        'document.querySelectorAll("#click-me").length'
        ')'
    )
    quteproc.wait_for_js('clickables-js-left:0')


@pytest.mark.usefixtures('quteproc')
def test_all_hints_detect_react_style_click_props(quteproc, request):
    if not request.config.webengine:
        pytest.skip('QtWebKit has no main-world click tracker helper')

    quteproc.open_path('data/hints/clickables_react_props.html')

    quteproc.send_cmd(':hint all delete')
    quteproc.wait_for(message='hints: a', category='hints')
    quteproc.send_cmd(':hint-follow a')

    quteproc.send_cmd(
        ':jseval --world main '
        'console.log('
        '"clickables-react-left:" + '
        'document.querySelectorAll("#react-like").length'
        ')'
    )
    quteproc.wait_for_js('clickables-react-left:0')


@pytest.mark.usefixtures('quteproc')
def test_all_hints_ignore_nested_clicktrack_inside_base_clickable(quteproc, request):
    if not request.config.webengine:
        pytest.skip('QtWebKit has no main-world click tracker helper')

    quteproc.open_path('data/hints/clickables_nested_inside_button.html')

    quteproc.send_cmd(':hint all delete')
    quteproc.wait_for(message='hints: a, s', category='hints')


@pytest.mark.usefixtures('quteproc')
def test_all_hints_prune_duplicate_selector_ancestors(quteproc):
    quteproc.open_path('data/hints/clickables_selector_duplicate_ancestors.html')

    quteproc.send_cmd(':hint all delete')
    quteproc.wait_for(message='hints: a', category='hints')
    quteproc.send_cmd(':hint-follow a')

    quteproc.send_cmd(
        ':jseval --world main '
        'console.log('
        '"selector-dup-left:" + '
        'document.querySelectorAll("#tree").length + "," + '
        'document.querySelectorAll("#row").length + "," + '
        'document.querySelectorAll("#icon").length'
        ')'
    )
    quteproc.wait_for_js('selector-dup-left:1,0,0')
