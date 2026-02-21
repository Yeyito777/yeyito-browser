# Chrome Extension Support

## Tier 1 — Extension popup UI (Python)
- [ ] Create a QWebEngineView popup widget for extension browser action popups
- [ ] Wire popup trigger to a keybinding or statusbar button
- [ ] Handle `browserAction.setPopup()` / `action.setPopup()` from extensions
- [ ] Support popup resizing based on extension content

## Tier 2 — chrome.tabs API (C++)
- [ ] Implement `tabs.query()` in QtWebEngine's tabs_api.cc
- [ ] Implement `tabs.sendMessage()` for content script ↔ background communication
- [ ] Implement `tabs.get()` for single tab lookup
- [ ] Implement `tabs.create()` and `tabs.remove()`
- [ ] Implement `tabs.executeScript()` for programmatic script injection
- [ ] Wire tab events: `onActivated`, `onUpdated`, `onCreated`, `onRemoved`
- [ ] Bridge qutebrowser tab state (TabRuntimeManager) into Chromium extension layer

## Tier 3 — webRequest / declarativeNetRequest (C++)
- [ ] Verify whether webRequest events already fire in QtWebEngine's network stack
- [ ] If not, wire URL loading events to extension webRequest handlers
- [ ] Test declarativeNetRequest static rule matching (may already work)
- [ ] Test with uBlock Origin or a simple request-blocking extension

## Tier 4 — Context menus (C++/Python)
- [ ] Bridge `chrome.contextMenus` API to qutebrowser's context menu
- [ ] Render extension-added menu items in right-click menu
- [ ] Handle menu item click callbacks back to extensions

## Tier 5 — Options pages and management (Python)
- [ ] Implement `runtime.openOptionsPage()` — read `options_page` from manifest, open in tab
- [ ] Add `:extension-options <name>` command
- [ ] Add enable/disable individual extensions without restart

## Tier 6 — Additional APIs
- [ ] `chrome.cookies` — cookie read/write for extensions
- [ ] `chrome.webNavigation` — navigation event dispatch to extensions
- [ ] `chrome.notifications` — bridge to desktop notifications
- [ ] `chrome.omnibox` — extension search suggestions in qutebrowser's command line
