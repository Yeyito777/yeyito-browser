# SPDX-FileCopyrightText: Florian Bruhin (The Compiler) <mail@qutebrowser.org>
#
# SPDX-License-Identifier: GPL-3.0-or-later

import pytest
from qutebrowser.qt.core import QUrl
pytest.importorskip('qutebrowser.qt.webenginecore')
from qutebrowser.qt.webenginecore import QWebEngineCookieStore, QWebEngineProfile

from qutebrowser.browser.webengine import cookies
from qutebrowser.utils import urlmatch


@pytest.fixture
def filter_request():
    request = QWebEngineCookieStore.FilterRequest()
    request.firstPartyUrl = QUrl('https://example.com')
    return request


@pytest.fixture(autouse=True)
def enable_cookie_logging(monkeypatch):
    monkeypatch.setattr(cookies.objects, 'debug_flags', ['log-cookies'])


@pytest.mark.parametrize('setting, third_party, accepted', [
    ('all', False, True),
    ('never', False, False),
    ('no-3rdparty', False, True),
    ('no-3rdparty', True, False),
])
def test_accept_cookie(config_stub, filter_request, setting, third_party,
                       accepted):
    """Test that _accept_cookie respects content.cookies.accept."""
    config_stub.val.content.cookies.accept = setting
    filter_request.thirdParty = third_party
    assert cookies._accept_cookie(filter_request) == accepted


@pytest.mark.parametrize('setting, pattern_setting, third_party, accepted', [
    ('never', 'all', False, True),
    ('all', 'never', False, False),
    ('no-3rdparty', 'all', True, True),
    ('all', 'no-3rdparty', True, False),
])
def test_accept_cookie_with_pattern(config_stub, filter_request, setting,
                                    pattern_setting, third_party, accepted):
    """Test that _accept_cookie matches firstPartyUrl with the UrlPattern."""
    filter_request.thirdParty = third_party
    config_stub.set_str('content.cookies.accept', setting)
    config_stub.set_str('content.cookies.accept', pattern_setting,
                        pattern=urlmatch.UrlPattern('https://*.example.com'))
    assert cookies._accept_cookie(filter_request) == accepted


@pytest.mark.parametrize('global_value', ['never', 'all'])
def test_invalid_url(config_stub, filter_request, global_value):
    """Make sure we fall back to the global value with invalid URLs.

    This can happen when there's a cookie request from an iframe, e.g. here:
    https://developers.google.com/youtube/youtube_player_demo
    """
    config_stub.val.content.cookies.accept = global_value
    filter_request.firstPartyUrl = QUrl()
    accepted = global_value == 'all'
    assert cookies._accept_cookie(filter_request) == accepted


@pytest.mark.parametrize('enabled', [True, False])
def test_logging(monkeypatch, config_stub, filter_request, caplog, enabled):
    monkeypatch.setattr(cookies.objects, 'debug_flags',
                        ['log-cookies'] if enabled else [])
    config_stub.val.content.cookies.accept = 'all'
    caplog.clear()

    cookies._accept_cookie(filter_request)

    if enabled:
        expected = ("Cookie from origin <unknown> on https://example.com "
                    "(third party: False) -> applying setting all")
        assert caplog.messages == [expected]
    else:
        assert not caplog.messages


class TestThirdPartyWhitelist:
    """Tests for content.cookies.thirdparty_whitelist setting."""

    @pytest.fixture
    def thirdparty_request(self):
        """Create a filter request for a third-party cookie."""
        request = QWebEngineCookieStore.FilterRequest()
        request.firstPartyUrl = QUrl('https://example.com')
        request.origin = QUrl('https://hcaptcha.com')
        request.thirdParty = True
        return request

    def test_thirdparty_blocked_without_whitelist(self, config_stub,
                                                   thirdparty_request):
        """Third-party cookies are blocked when whitelist is empty."""
        config_stub.val.content.cookies.accept = 'no-3rdparty'
        config_stub.val.content.cookies.thirdparty_whitelist = []
        assert cookies._accept_cookie(thirdparty_request) is False

    def test_thirdparty_allowed_with_whitelist(self, config_stub,
                                                thirdparty_request):
        """Third-party cookies are allowed when origin matches whitelist."""
        config_stub.val.content.cookies.accept = 'no-3rdparty'
        config_stub.val.content.cookies.thirdparty_whitelist = [
            '*://hcaptcha.com/*'
        ]
        assert cookies._accept_cookie(thirdparty_request) is True

    def test_thirdparty_allowed_with_subdomain_pattern(self, config_stub,
                                                        thirdparty_request):
        """Third-party cookies are allowed with subdomain wildcard patterns."""
        thirdparty_request.origin = QUrl('https://accounts.hcaptcha.com')
        config_stub.val.content.cookies.accept = 'no-3rdparty'
        config_stub.val.content.cookies.thirdparty_whitelist = [
            '*://*.hcaptcha.com/*'
        ]
        assert cookies._accept_cookie(thirdparty_request) is True

    def test_thirdparty_blocked_when_not_matching(self, config_stub,
                                                   thirdparty_request):
        """Third-party cookies are blocked when origin doesn't match whitelist."""
        thirdparty_request.origin = QUrl('https://tracker.example.net')
        config_stub.val.content.cookies.accept = 'no-3rdparty'
        config_stub.val.content.cookies.thirdparty_whitelist = [
            '*://*.hcaptcha.com/*'
        ]
        assert cookies._accept_cookie(thirdparty_request) is False

    def test_firstparty_unaffected_by_whitelist(self, config_stub,
                                                 thirdparty_request):
        """First-party cookies work regardless of whitelist."""
        thirdparty_request.thirdParty = False
        config_stub.val.content.cookies.accept = 'no-3rdparty'
        config_stub.val.content.cookies.thirdparty_whitelist = []
        assert cookies._accept_cookie(thirdparty_request) is True

    def test_whitelist_ignored_when_accept_all(self, config_stub,
                                                thirdparty_request):
        """Whitelist is not consulted when accept is 'all'."""
        config_stub.val.content.cookies.accept = 'all'
        config_stub.val.content.cookies.thirdparty_whitelist = []
        assert cookies._accept_cookie(thirdparty_request) is True

    def test_whitelist_ignored_when_accept_never(self, config_stub,
                                                  thirdparty_request):
        """Whitelist cannot override 'never' setting."""
        config_stub.val.content.cookies.accept = 'never'
        config_stub.val.content.cookies.thirdparty_whitelist = [
            '*://hcaptcha.com/*'
        ]
        assert cookies._accept_cookie(thirdparty_request) is False

    def test_invalid_origin_url(self, config_stub, thirdparty_request):
        """Invalid origin URL doesn't crash, cookie is blocked."""
        thirdparty_request.origin = QUrl()  # Invalid URL
        config_stub.val.content.cookies.accept = 'no-3rdparty'
        config_stub.val.content.cookies.thirdparty_whitelist = [
            '*://hcaptcha.com/*'
        ]
        assert cookies._accept_cookie(thirdparty_request) is False

    def test_whitelist_with_no_unknown_3rdparty(self, config_stub,
                                                 thirdparty_request):
        """Whitelist also works with no-unknown-3rdparty setting."""
        config_stub.val.content.cookies.accept = 'no-unknown-3rdparty'
        config_stub.val.content.cookies.thirdparty_whitelist = [
            '*://hcaptcha.com/*'
        ]
        assert cookies._accept_cookie(thirdparty_request) is True


class TestInstall:

    def test_real_profile(self):
        profile = QWebEngineProfile()
        cookies.install_filter(profile)

    def test_fake_profile(self, stubs):
        store = stubs.FakeCookieStore()
        profile = stubs.FakeWebEngineProfile(cookie_store=store)

        cookies.install_filter(profile)

        assert store.cookie_filter is cookies._accept_cookie
