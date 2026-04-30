# Yeyito Chromium-only browser migration TODO

This is the canonical TODO for moving the current qutebrowser/QtWebEngine shim into a Chromium-only browser with the correct architecture.

## Architectural target

```text
Chromium browser process
  ├── YeyitoBrowserShell / window
  ├── InputManager
  ├── CommandDispatcher
  ├── TabModel
  ├── Native chrome views
  │   ├── sidebar
  │   ├── statusbar
  │   ├── commandline
  │   ├── completion
  │   └── prompts
  └── WebContents host / viewport

Renderer processes
  └── web pages only
```

The shell/input/tab model must be independent from tab renderer load or renderer hangs.

Hard requirements:

- Browser chrome/input/tab model must not depend on any tab renderer being responsive.
- Tab sidebar renders from browser-process cached metadata, not renderer/page calls.
- Page renderers can hang without freezing tab switching/sidebar/commandline/completion/prompts/statusbar.
- No Qt dependency for final browser chrome.
- No Python/qutebrowser dependency for core browser UX long-term.
- UI must use Whale-native colors and behavior.
- Caret mode stays deleted.

Whale constants/defaults:

- Backgrounds: `#00050f`, `#000a1a`, `#001020`
- Accent: `#1d9bf0`
- Accent/glow: `#4fd0ff`
- Text: `#ffffff`, `#cce7ff`
- Left tabs width: `175`
- Font: JetBrains Mono
- Smooth scroll factor: `0.3`
- Completion categories: quickmarks, bookmarks, history, filesystem
- Tab row format: `[num] <favicon> <link>`
- Tab link display: omit `https://`/`http://`, local file basename, no wrapping, right-end ellipsis

## Performance/design rules

- No sync IPC to renderer during input handling, sidebar paint, commandline paint, or tab switching.
- No direct JS execution in browser command path unless asynchronous.
- No DB/filesystem blocking on UI thread.
- No WebContents/page query during sidebar paint except browser-process cached metadata.
- Use observers/caches for all tab metadata.

## Milestone A — Chromium shell skeleton

Goal: a standalone Chromium-side binary/window, no Qt, capable of hosting one `WebContents`.

TODO:

- [ ] Decide source root (`qtwebengine/src/3rdparty/chromium/yeyito/` preferred for custom code).
- [ ] Add GN target for a `yeyito_browser` executable or equivalent dev shell.
- [ ] Add entrypoint:
  - [ ] `yeyito/app/yeyito_main.cc`
  - [ ] `yeyito/app/yeyito_content_main_delegate.{h,cc}`
  - [ ] `yeyito/browser/yeyito_browser_main_parts.{h,cc}`
  - [ ] `yeyito/browser/yeyito_content_browser_client.{h,cc}`
  - [ ] `yeyito/browser/yeyito_browser_context.{h,cc}`
- [ ] Open one Chromium-native window.
- [ ] Open one `WebContents` in that window.
- [ ] Add dev build/install target, e.g. `make yeyito-browser-dev`.

Acceptance:

- [ ] `yeyito_browser about:blank` opens a Chromium-native window in `xenv`.
- [ ] No Qt browser chrome involved.
- [ ] It can navigate to a URL.

## Milestone B — Browser shell/window layout

Goal: native browser-process shell with a stable chrome layer and a WebContents viewport.

Classes/files:

- [ ] `YeyitoBrowser`
- [ ] `YeyitoBrowserWindow`
- [ ] `YeyitoRootView`
- [ ] `YeyitoChromeView`
- [ ] `YeyitoWebContentsHostView`

Target layout:

```text
YeyitoRootView
  ├── YeyitoSidebarView        fixed 175 px
  └── MainAreaView
       ├── WebContentsHostView
       ├── StatusOverlayView
       ├── CommandLineView
       ├── CompletionView
       └── PromptView
```

TODO:

- [ ] Implement top-level `views::Widget` window.
- [ ] Implement root layout with fixed-width sidebar sibling to WebContents host.
- [ ] Ensure sidebar has its own opaque layer.
- [ ] Ensure tab/WebContents attach/detach does not recreate or repaint the shell.
- [ ] Avoid renderer calls during shell layout/paint.

Acceptance:

- [ ] Sidebar does not flicker on WebContents swap.
- [ ] Browser chrome remains visible/stable during renderer hang.

## Milestone C — Native tab model

Goal: tabs become browser-process model entries.

Classes/files:

- [ ] `YeyitoTabModel`
- [ ] `YeyitoTab`
- [ ] `YeyitoTabObserver`

Cached tab fields:

- [ ] stable tab id
- [ ] URL
- [ ] title
- [ ] favicon
- [ ] loading/progress
- [ ] pinned
- [ ] audible/muted
- [ ] crashed/discarded
- [ ] `content::WebContents*`

TODO:

- [ ] Add tab create/close/move/clone/pin.
- [ ] Add active index.
- [ ] Add observer events: inserted/removed/moved/active changed/metadata changed.
- [ ] Add per-tab `WebContentsObserver` to update cached metadata asynchronously.
- [ ] Never query renderer synchronously for sidebar/input.
- [ ] Add lazy restore concept: metadata before WebContents creation.

Acceptance:

- [ ] 100 tabs can be listed from model without touching renderers.
- [ ] Renderer hang does not block tab switch.
- [ ] Sidebar updates from cached metadata only.

## Milestone D — WebContents lifecycle/host

Goal: shell owns a viewport slot; tabs own WebContents.

TODO:

- [ ] Create `WebContents` via `content::WebContents::Create`.
- [ ] Attach active tab native view to host.
- [ ] Detach old active tab on switch.
- [ ] Change active index before any WebContents work.
- [ ] Show crashed/hung/unloaded placeholders without blocking shell.
- [ ] Lazy-load restored tabs.

Acceptance:

- [ ] Startup can show sidebar before all tabs load.
- [ ] Switching away from hung tab works immediately.

## Milestone E — Input manager/key dispatcher

Goal: browser-level key handling before active renderer.

Classes/files:

- [ ] `YeyitoInputManager`
- [ ] `YeyitoKeyDispatcher`
- [ ] `YeyitoKeyTrie`
- [ ] `YeyitoCommand`

TODO:

- [ ] Port current `QutebrowserKeyDispatcher` logic to Chromium-native code.
- [ ] Own modes: normal, insert, passthrough, command, hint, prompt, yesno.
- [ ] Own count prefix and keychain.
- [ ] Add command metadata/domain:
  - [ ] BrowserChrome
  - [ ] TabModel
  - [ ] WebContentsAsync
  - [ ] RendererHint
  - [ ] TemporaryFallback
- [ ] Early-handle renderer-independent commands:
  - [ ] tab-next/tab-prev/tab-focus/tab-move
  - [ ] commandline open
  - [ ] search open
  - [ ] completion movement
  - [ ] prompt answers
- [ ] Support counts (`3J`) and keychains (`g0`, `g$`, `go`).
- [ ] Suppress matching key release for consumed browser keys.
- [ ] Insert mode passes text to page.

Acceptance:

- [ ] `Shift+J/K`, `:`, `Esc` work while active renderer is hung.
- [ ] Insert mode does not steal text.
- [ ] Counts/keychains work.

## Milestone F — Sidebar view

Goal: Whale-native browser-process tab sidebar.

Classes/files:

- [ ] `YeyitoSidebarView`
- [ ] `YeyitoTabRowView`

TODO:

- [ ] Implement fixed width 175.
- [ ] Render rows as `[num] <favicon> <link>`.
- [ ] Omit `https://`/`http://`, local file basename.
- [ ] No wrapping; right ellipsis.
- [ ] Active row accent `#1d9bf0`.
- [ ] Observe `YeyitoTabModel`.
- [ ] Repaint only changed rows where practical.
- [ ] Support click to switch tab.
- [ ] Later: drag reorder and wheel behavior.

Acceptance:

- [ ] Sidebar does not flicker on tab switch.
- [ ] Sidebar updates instantly during hung tab.
- [ ] Visual matches qutebrowser/Whale.

## Milestone G — Statusbar/overlays

TODO:

- [ ] `YeyitoStatusView`
- [ ] Render mode/keychain/count.
- [ ] Render URL/link hover.
- [ ] Render scroll position.
- [ ] Use Whale colors.
- [ ] Observe input mode and active tab metadata.

Acceptance:

- [ ] Status updates while renderer is hung.
- [ ] No flicker on tab switch.

## Milestone H — Commandline

TODO:

- [ ] `YeyitoCommandLineView`
- [ ] `YeyitoCommandController`
- [ ] `YeyitoCommandParser`
- [ ] `:` opens commandline.
- [ ] `o`, `go`, etc. prefill commandline.
- [ ] Escape cancels, Return executes.
- [ ] Native command execution for `open`, `tab-focus`, `tab-move`, etc.
- [ ] Expand placeholders `{url}`, `{url:pretty}`, `{url:yank}`, `{clipboard}`, `{primary}`.
- [ ] Maintain command history.

Acceptance:

- [ ] Commandline works while renderer is hung.
- [ ] No Python required for basic tab/open commands.

## Milestone I — Completion

Categories:

- [ ] quickmarks
- [ ] bookmarks
- [ ] history
- [ ] filesystem

TODO:

- [ ] `YeyitoCompletionView`
- [ ] `YeyitoCompletionModel`
- [ ] async providers for all categories.
- [ ] Keyboard: Tab/Shift+Tab/Ctrl+n/Ctrl+p/Up/Down/PgUp/PgDn.
- [ ] Whale styling.

Acceptance:

- [ ] Completion opens instantly.
- [ ] Slow providers never block UI.

## Milestone J — Prompts/yes-no/dialogs

TODO:

- [ ] `YeyitoPromptView`
- [ ] JS alert/confirm/prompt.
- [ ] download prompt.
- [ ] file chooser prompt.
- [ ] auth/cert prompt.
- [ ] yes-no mode.

Acceptance:

- [ ] Prompt input independent from page renderer.
- [ ] Prompt mode blocks normal-mode tab keys.

## Milestone K — Native tab/window/session commands

TODO:

- [ ] tab-next/tab-prev/tab-focus/tab-move/tab-clone/tab-only/tab-pin/tab-close/undo.
- [ ] open/open -t/open -b/open -w/open -p.
- [ ] new window/close window/session save/restore.
- [ ] closed-tab undo stack.
- [ ] metadata-first startup restore.

Acceptance:

- [ ] Startup with many tabs shows sidebar immediately.
- [ ] Tab switch instant during loads/hangs.

## Milestone L — Profile/preferences/config migration

TODO:

- [ ] Native prefs registry.
- [ ] Native defaults for Whale theme and qutebrowser-yeyito config.
- [ ] Smooth factor `0.3`.
- [ ] Completion category prefs.
- [ ] Devtools autofocus.
- [ ] Webpage bg `#00050f`.
- [ ] One-time import or compatibility reader for current config.
- [ ] Remove moved features from qutebrowser `config.py`.

Acceptance:

- [ ] Fresh native profile uses Whale defaults.
- [ ] Existing profile migrates.

## Milestone M — History/bookmarks/quickmarks

TODO:

- [ ] Chromium HistoryService for history.
- [ ] Chromium BookmarkModel for bookmarks.
- [ ] Custom quickmark store/importer.
- [ ] Commands for quickmarks/bookmarks.
- [ ] Completion integration.

## Milestone N — Downloads

TODO:

- [ ] Native download manager UI/status/prompt.
- [ ] Commands for cancel/open/yank.
- [ ] Hint download action.
- [ ] PDF handling decision.

## Milestone O — Search/find

TODO:

- [ ] `YeyitoFindController`.
- [ ] Use `WebContents::Find`.
- [ ] Views find UI.
- [ ] `/`, `?`, `n`, `N`, match count.

Acceptance:

- [ ] Find UI independent from renderer chrome lifecycle.

## Milestone P — Hints

Architecture:

```text
Browser process:
  - parses hint command/options
  - owns hint mode/input state

Renderer/Blink:
  - enumerates page elements
  - paints/positions page-local labels or sends rects back
  - activates selected target
```

TODO:

- [ ] Explicit `YeyitoHintCommand` transport instead of physical-key inference.
- [ ] Browser→renderer selector/action/flags.
- [ ] Browser owns typed hint string/mode.
- [ ] Implement common variants:
  - [ ] hint all current/tab/right-click/hover
  - [ ] hint scrollables
  - [ ] hint inputs / --first
  - [ ] hint links yank/download/fill
- [ ] Whale hint styling: bg `#1d9bf0`, fg `#00050f`, border `1px solid #1d9bf0`, radius `0`.

Acceptance:

- [ ] Browser can exit hint mode even if renderer hangs.

## Milestone Q — Smooth scrolling

TODO:

- [ ] Native prefs.
- [ ] Preserve smooth factor `0.3`.
- [ ] Async page scroll commands.

## Milestone R — Clipboard/yank

TODO:

- [ ] Clipboard/primary abstraction.
- [ ] yank URL/title/selection.
- [ ] hint yank.
- [ ] command placeholders.

## Milestone S — Devtools

TODO:

- [ ] Native devtools command/window/panel.
- [ ] Autofocus based on profile pref.

## Milestone T — URL/navigation

TODO:

- [ ] qutebrowser-compatible URL/search parser.
- [ ] search engines.
- [ ] open dispositions and tab insertion policies.

## Milestone U — Security boundaries

TODO:

- [ ] Browser chrome native/privileged, not page DOM.
- [ ] Web content cannot spoof chrome or invoke privileged commands.
- [ ] Validate renderer-to-browser hint messages.

## Milestone V — Tests/perf harness

TODO:

- [ ] xenv smoke scripts:
  - [ ] shell tabs
  - [ ] hung renderer
  - [ ] commandline
  - [ ] completion
- [ ] Browser tests:
  - [ ] tab model
  - [ ] input manager
  - [ ] command parser
  - [ ] session
- [ ] Screenshot checks for Whale sidebar.
- [ ] Perf trace tests: tab switch latency with hung renderer; startup with many tabs.

## Milestone W — Delete Qt/qutebrowser shim

TODO:

- [ ] Mark current Qt sidebar/find/commandline as temporary.
- [ ] Disable/delete Qt sidebar after native sidebar exists.
- [ ] Disable/delete Qt commandline after native commandline exists.
- [ ] Disable/delete Qt find after native find exists.
- [ ] Remove Python tab widget/session dependency after native tab/session exists.
- [ ] Remove QtWebEngineWidgets browser shell.
- [ ] Ship Chromium-only launcher.
