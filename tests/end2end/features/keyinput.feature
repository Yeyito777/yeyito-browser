Feature: Keyboard input

    Tests for :clear-keychain and other keyboard input related things.

    # :clear-keychain

    Scenario: Clearing the keychain
        When I run :bind ,foo message-error test12
        And I run :bind ,bar message-info test12-2
        And I press the keys ",fo"
        And I run :clear-keychain
        And I press the keys ",bar"
        And I run :unbind ,foo
        And I run :unbind ,bar
        Then the message "test12-2" should be shown

    # input.forward_unbound_keys

    Scenario: Forwarding no keys
        When I open data/keyinput/log.html
        And I set input.forward_unbound_keys to none
        And I press the key "<F1>"
        # <F1>
        Then the javascript message "key press: 112" should not be logged
        And the javascript message "key release: 112" should not be logged

    # :fake-key

    Scenario: :fake-key with an unparsable key
        When I run :fake-key <blub>
        Then the error "Could not parse '<blub>': Got invalid key!" should be shown

    Scenario: :fake-key sending key to the website
        When I open data/keyinput/log.html
        And I wait 0.01s
        And I run :fake-key x
        Then the javascript message "key press: 88" should be logged
        And the javascript message "key release: 88" should be logged

    @no_xvfb @posix @qtwebengine_skip
    Scenario: :fake-key sending key to the website with other window focused
        When I open data/keyinput/log.html
        And I run :devtools
        And I wait for "Focus object changed: <Py*.QtWebKitWidgets.QWebView object at *>" in the log
        And I run :fake-key x
        And I run :devtools
        And I wait for "Focus object changed: <qutebrowser.browser.webkit.webview.WebView *>" in the log
        Then the error "No focused webview!" should be shown

    Scenario: :fake-key sending special key to the website
        When I open data/keyinput/log.html
        And I wait 0.01s
        And I run :fake-key <Escape>
        Then the javascript message "key press: 27" should be logged
        And the javascript message "key release: 27" should be logged

    Scenario: :fake-key sending keychain to the website
        When I open data/keyinput/log.html
        And I wait 0.01s
        And I run :fake-key x<greater>y<less>" "
        Then the javascript message "key press: 88" should be logged
        And the javascript message "key release: 88" should be logged
        And the javascript message "key press: 190" should be logged
        And the javascript message "key release: 190" should be logged
        And the javascript message "key press: 89" should be logged
        And the javascript message "key release: 89" should be logged
        And the javascript message "key press: 188" should be logged
        And the javascript message "key release: 188" should be logged
        And the javascript message "key press: 32" should be logged
        And the javascript message "key release: 32" should be logged

    Scenario: :fake-key sending keypress to qutebrowser
        When I run :fake-key -g x
        And I wait for "got keypress in mode KeyMode.normal - delegating to <qutebrowser.keyinput.modeparsers.NormalKeyParser>" in the log
        Then no crash should happen

    # test all tabs.mode_on_change modes

    Scenario: mode on change normal
        Given I set tabs.mode_on_change to normal
        And I clean up open tabs
        When I open data/hello.txt
        And I run :mode-enter insert
        And I wait for "Entering mode KeyMode.insert (reason: command)" in the log
        And I open data/hello2.txt in a new background tab
        And I run :tab-focus 2
        Then "Leaving mode KeyMode.insert (reason: tab changed)" should be logged
        And "Mode before tab change: insert (mode_on_change = normal)" should be logged
        And "Mode after tab change: normal (mode_on_change = normal)" should be logged

    Scenario: mode on change persist
        Given I set tabs.mode_on_change to persist
        And I clean up open tabs
        When I open data/hello.txt
        And I run :mode-enter insert
        And I wait for "Entering mode KeyMode.insert (reason: command)" in the log
        And I open data/hello2.txt in a new background tab
        And I run :tab-focus 2
        Then "Leaving mode KeyMode.insert (reason: tab changed)" should not be logged
        And "Mode before tab change: insert (mode_on_change = persist)" should be logged
        And "Mode after tab change: insert (mode_on_change = persist)" should be logged

    Scenario: mode on change restore
        Given I set tabs.mode_on_change to restore
        And I clean up open tabs
        When I open data/hello.txt
        And I run :mode-enter insert
        And I wait for "Entering mode KeyMode.insert (reason: command)" in the log
        And I open data/hello2.txt in a new background tab
        And I run :tab-focus 2
        And I wait for "Mode before tab change: insert (mode_on_change = restore)" in the log
        And I wait for "Mode after tab change: normal (mode_on_change = restore)" in the log
        And I run :mode-enter passthrough
        And I wait for "Entering mode KeyMode.passthrough (reason: command)" in the log
        And I run :tab-focus 1
        Then "Mode before tab change: passthrough (mode_on_change = restore)" should be logged
        And "Entering mode KeyMode.insert (reason: restore)" should be logged
        And "Mode after tab change: insert (mode_on_change = restore)" should be logged
