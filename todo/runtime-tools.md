# Runtime Tools — Missing for E2E Testing

## Screenshot tool (`screenshot.sh`)
- [ ] Dedicated per-tab `screenshot.sh` that saves to `screenshot.png` in the tab dir (like `dom.html`)
- [ ] Should screenshot the tab whose directory it lives in, not whichever tab is focused
- [ ] Support `--rect WxH+X+Y` passthrough for sub-region captures
- [ ] Full-window capture mode (`--window`) — grab the entire QMainWindow (tab bar, statusbar, completion widget), not just web content via `tab.grab_pixmap()`

## Input simulation
- [ ] `:fake-click <x> <y>` command — synthesize a QMouseEvent at widget coordinates, dispatched through Qt's event system
- [ ] `:fake-hover <x> <y>` — for testing hover states, element shader highlights, tooltip triggers
- [ ] `:fake-drag <x1> <y1> <x2> <y2>` — for testing text selection, slider interaction
- [ ] Consider an `input.sh` script per tab that wraps these, translating element selectors or CSS coordinates into widget positions via JS bridge
- [ ] `:fake-key` already exists for keyboard — just needs to be scriptable through `command.sh` (verify it works end-to-end)

## Wait-for-condition (`wait.sh`)
- [ ] `wait.sh --js '<expression>'` — poll `console.sh` until the JS expression returns truthy
- [ ] `wait.sh --load` — block until `tab-data.info` shows `load_status: success` or `success_https`
- [ ] `wait.sh --network-idle [--timeout <s>]` — block until no new network requests for N seconds
- [ ] `wait.sh --text '<string>'` — poll DOM (via `snapshot-dom.sh` or JS) until text appears on page
- [ ] Configurable poll interval and timeout with sane defaults

## Visual diffing
- [ ] Baseline screenshot storage (e.g. `{basedir}/runtime/baselines/`)
- [ ] Pixel-diff or perceptual-diff between current screenshot and baseline
- [ ] Tolerance threshold for anti-aliasing / subpixel rendering differences
- [ ] Diff image output showing where pixels changed
