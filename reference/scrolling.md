# Scrolling

Custom scrolling system — command layer, abstract interface, WebEngine implementation with compositor-based smooth scrolling via `SmoothScrollController`, JS element discovery, per-element scrolling.

## Architecture

Three layers: user commands → abstract scroller interface → WebEngine implementation (JS element discovery + C++ compositor gestures).

```
:scroll down
  → scrollcommands.scroll()
  → tab.scroller.down(count)
  → WebEngineScroller._smooth_scroll(0, px)
    ├→ JS scroll.get_scroll_target_center(dx, dy)
    │  ├→ _deepActiveElement() → _findScrollable()
    │  └→ returns [centerX, centerY] of target element, or null
    └→ callback: _do_smooth_scroll(dx, dy, factor, js_result)
       ├→ [x, y] → smoothScrollBy(dx, dy, factor, x, y)  (element scroll)
       └→ null   → smoothScrollBy(dx, dy, factor)          (viewport scroll)
         → SmoothScrollController::scrollBy()
           ├→ sendGestureScrollBegin(deltaXHint, deltaYHint)
           ├→ tick() → sendGestureScrollUpdate(stepX, stepY)  (QTimer at vsync)
           └→ sendGestureScrollEnd()
             → compositor thread applies scroll offset directly
  → QWebEnginePage.scrollPositionChanged signal
  → _update_pos() → emit perc_changed(x%, y%)
```

## Command Layer

`scrollcommands.py` registers four commands, all delegating to `tab.scroller`:

| Command | Method Called | Notes |
|---------|-------------|-------|
| `:scroll <direction>` | `up/down/left/right/top/bottom/page_up/page_down` | `count` multiplier for directional, none for top/bottom |
| `:scroll-px <dx> <dy>` | `delta(dx * count, dy * count)` | Raw pixel scrolling |
| `:scroll-to-perc [perc]` | `to_perc(x, y)` | Defaults to 100% if no arg; emits `before_jump_requested` |
| `:scroll-to-anchor <name>` | `to_anchor(name)` | Emits `before_jump_requested` |

## Abstract Interface

`AbstractScroller` (`browsertab.py:614-689`) defines the contract as a `QObject`.

### Signals

| Signal | Purpose |
|--------|---------|
| `perc_changed(int, int)` | Emitted when scroll position changes (x%, y%) |
| `before_jump_requested()` | Emitted before major jumps — used to set the `'` mark for return |

### Methods

**Position/state:** `pos_px() → QPoint`, `pos_perc() → (int, int)`, `at_top() → bool`, `at_bottom() → bool`

**Scrolling:** `delta(x, y)`, `delta_page(x, y)`, `to_perc(x, y)`, `to_point(point)`, `to_anchor(name)`, `up/down/left/right(count)`, `top()`, `bottom()`, `page_up/page_down(count)`

## WebEngine Implementation

`WebEngineScroller` (`webenginetab.py:496-637`) — the only active implementation (WebKit is legacy).

### Key constant

`_ARROW_SCROLL_PX = 40` — pixels per arrow key press.

### Initialization

Connects `QWebEnginePage.scrollPositionChanged` → `_update_pos` in `_init_widget()` (line 512). Tracks position as both pixels (`_pos_px`) and percentage (`_pos_perc`), plus `_at_bottom` flag.

### Method routing

| Methods | Implementation | Notes |
|---------|---------------|-------|
| `up/down/left/right` | `_smooth_scroll()` | Compositor gesture path via `SmoothScrollController` |
| `delta` | `_smooth_scroll()` | Same compositor path |
| `to_perc` | JS `scroll.to_perc` | Direct JS call |
| `to_point` | JS `window.scroll` | Direct JS call |
| `to_anchor` | `tab.load_url(url#fragment)` | Navigation-based |
| `delta_page` | JS `scroll.delta_page` | Fraction of viewport |
| `top/bottom` | Fake Home/End key | No JS involved |
| `page_up/page_down` | Fake PageUp/PageDown key | No JS involved |

### `_smooth_scroll(dx, dy)`

Core method for directional and delta scrolling (lines 585-597):

1. Get `factor` from `config.val.scrolling.smooth_factor`
2. Run JS `scroll.get_scroll_target_center(dx, dy)` to find the target scrollable element
3. JS returns `[centerX, centerY]` if a focused scrollable element exists, or `null`
4. Callback `_do_smooth_scroll` routes:
   - `[x, y]` → `smoothScrollBy(dx, dy, factor, x, y)` — compositor scrolls at element position
   - `null` → `smoothScrollBy(dx, dy, factor)` — compositor scrolls viewport directly (`target_viewport = true`)

### `_update_pos(pos: QPointF)`

Called on every `scrollPositionChanged` signal (lines 520-564):

1. Converts `QPointF` to `QPoint` pixel position
2. Calculates percentage: `round(100 / scrollable_range * pos)`
3. Determines `_at_bottom` via `math.ceil(pos.y()) >= scrollable_y`
4. Emits `perc_changed` only when percentage actually changes (deduplicated)

## SmoothScrollController (C++)

`smooth_scroll_controller.cpp` — browser-process-side smooth scroll animation that sends compositor gesture events. Runs on a `QTimer` synced to the monitor refresh rate.

### Why compositor scrolling

Compositor gesture events are handled on the compositor thread, which updates scroll offsets directly on composited layers without a round-trip to the renderer main thread. This decouples scroll animation from main-thread load (heavy JS, layout recalc), producing visibly smoother scrolling on complex pages like Discord.

### Lifecycle

1. `scrollBy(dx, dy, factor, posX, posY)` — entry point from `QWebEnginePage::smoothScrollBy`
   - Accumulates deltas into `m_dx/m_dy`
   - On first call (`!m_scrolling`): saves position, sends `GestureScrollBegin`, starts timer
   - On subsequent calls (while animating): just accumulates, timer is already running
2. `tick()` — called every frame by `QTimer`
   - Time-based exponential decay: `effectiveFactor = 1 - pow(1 - factor, dt / 16.0)`
   - Computes fractional step, accumulates sub-pixel remainder
   - Emits whole-pixel `GestureScrollUpdate` when accumulator crosses pixel boundary
   - Stops when remaining delta < 0.01, sends `GestureScrollEnd`
3. `stop()` — cancels animation, sends `GestureScrollEnd`

### Gesture events

All events use `WebGestureDevice::kTouchpad` source and position from `hitTestPosition()`:

| Event | Key fields |
|-------|-----------|
| `GestureScrollBegin` | `delta_x_hint`, `delta_y_hint` (scroll direction), `target_viewport` (true when no element position given) |
| `GestureScrollUpdate` | `delta_x = -stepX`, `delta_y = -stepY` (negated: gesture convention is opposite to scroll direction) |
| `GestureScrollEnd` | Position only |

### Hit test position

`hitTestPosition()` returns:
- `(posX, posY)` if explicit coordinates were given (element scrolling) — the compositor hit-tests here to find the scroll node
- Viewport center if `posX/posY == -1` (viewport scrolling) — combined with `target_viewport = true` to skip hit testing

### Delta hints and scroll node latching

The `GestureScrollBegin` event carries `delta_x_hint` and `delta_y_hint` to tell the compositor which direction the scroll will go. The compositor's `FindNodeToLatch()` uses these to find the first scroll node that can actually consume delta in the intended direction via `CanConsumeDelta()`.

**Critical:** Without direction hints (both zero), `CanConsumeDelta()` returns `true` for any node — causing the compositor to latch to the first `user_scrollable` node from the hit test, regardless of direction. This caused a bug where a horizontally-scrollable `<code>` element (overflow:auto, scrollWidth > clientWidth) captured vertical scroll events, blocking all page scrolling.

### Source files

| File | Role |
|------|------|
| `qtwebengine/src/core/smooth_scroll_controller.cpp` | Animation loop, gesture event emission |
| `qtwebengine/src/core/smooth_scroll_controller.h` | Class declaration |
| `qtwebengine/src/core/render_widget_host_view_qt.cpp` | `smoothScrollBy()` delegates to controller |
| `qtwebengine/src/core/api/qwebenginepage.cpp` | `QWebEnginePage::smoothScrollBy()` public API |
| `qtwebengine/src/core/web_contents_adapter.cpp` | Adapter between Qt API and RWHV |

## JavaScript Engine

`scroll.js` — runs in the page context as `window._qutebrowser.scroll`.

### Public functions

| Function | Called by | Purpose |
|----------|----------|---------|
| `get_scroll_target_center(dx, dy)` | `_smooth_scroll` | Find focused scrollable element center; returns `[x, y]` or `null` |
| `scroll_focused(dx, dy, factor)` | (available for direct JS scrolling) | Scroll focused element via JS `scrollBy`; returns `bool` |
| `scroll_delta(dx, dy, factor)` | `delta()` | Scroll focused element or fall back to window |
| `to_perc(x, y)` | `to_perc()` | Scroll to percentage of scrollable range |
| `delta_page(x, y)` | `delta_page()` | Scroll by fraction of viewport via `window.scrollBy` |

### `get_scroll_target_center` vs `scroll_focused`

- `get_scroll_target_center` returns the center coordinates of the scrollable element for the compositor to hit-test and target. Used by the compositor path.
- `scroll_focused` directly calls `_accumulateScroll` on the element (JS-based scrolling). Available but not used by the primary scroll path — compositor scrolling is preferred for smoothness.

### Element discovery

1. `_deepActiveElement(root)` — traverses shadow DOM roots to find the deepest `activeElement`
2. `_findScrollable(start, dx, dy)` — walks up from active element checking each ancestor
3. `_isScrollable(elem, dx, dy)` — checks `overflow-x`/`overflow-y` CSS (`auto`/`scroll`/`overlay`) and whether `scrollWidth > clientWidth` or `scrollHeight > clientHeight`

Stops at `document.body` and `document.documentElement` — these are not considered scrollable targets (window scrolling handles them).

### Smooth animation accumulator

Batches rapid scroll calls into one continuous animation (lines 107-150):

**State:** `_scrollState = {target, dx, dy}`, `_scrollRaf` (animation frame ID), `_scrollFactor` (default 0.3)

**`_accumulateScroll(target, dx, dy, factor)`:**
- Same target as current animation → adds deltas to existing state
- Different target → cancels current animation, starts new one

**`_animateScroll()`** (RAF loop):
1. Step = `trunc(remaining * factor)` — fraction of remaining distance
2. Minimum step of ±1px while ≥1px remains (avoids large final snaps)
3. `scrollBy({behavior: "instant"})` — bypasses CSS smooth scrolling
4. Subtracts step from remaining, requests next frame
5. Stops when both `stepX` and `stepY` are 0

**Why `Math.trunc`:** Sub-pixel `scrollBy` values cause floor-based pixel snapping that makes up/down behave asymmetrically. Truncating to whole pixels avoids this.

**Note:** The JS accumulator is used by `scroll_focused` and `scroll_delta`. The primary directional scroll path (`up/down/left/right`, `delta`) uses the compositor `SmoothScrollController` instead, which has its own equivalent animation loop with better timing characteristics (browser-process QTimer vs renderer-thread RAF).

## Wheel Events

`eventfilter.py:157-194` — `_handle_wheel`:

| Condition | Behavior |
|-----------|----------|
| `_ignore_wheel_event` flag | Filter (consume) event — used after programmatic zoom |
| Hint mode active | Filter — block scrolling during hinting |
| Ctrl held (not passthrough mode) | Zoom: `factor + angleDelta.y / zoom.mouse_divider` |
| Otherwise | Pass through to WebEngine (native scroll) |

## Configuration

`configdata.yml:2148-2179`:

| Setting | Type | Default | Purpose |
|---------|------|---------|---------|
| `scrolling.bar` | String | `overlay` | Scrollbar visibility: `always` / `never` / `when-searching` / `overlay` |
| `scrolling.smooth` | Bool | `false` | Chromium's native smooth scrolling (per-pattern) |
| `scrolling.smooth_factor` | Float | `0.3` | Smooth scroll speed — fraction of remaining distance per frame. 0.05 = buttery, 1.0 = instant. Controls both the JS accumulator and the C++ `SmoothScrollController` |

Note: `scrolling.smooth` is the browser engine's built-in smooth scrolling. `scrolling.smooth_factor` controls qutebrowser's own smooth animation (both JS accumulator and C++ compositor controller). They are independent.

## Source Files

| File | Role |
|------|------|
| `qutebrowser/components/scrollcommands.py` | `:scroll`, `:scroll-px`, `:scroll-to-perc`, `:scroll-to-anchor` |
| `qutebrowser/browser/browsertab.py:614-689` | `AbstractScroller` interface |
| `qutebrowser/browser/webengine/webenginetab.py:496-637` | `WebEngineScroller` implementation |
| `qutebrowser/javascript/scroll.js` | JS scrolling engine (element discovery, target center, smooth accumulator) |
| `qutebrowser/config/configdata.yml:2148-2179` | `scrolling.*` settings |
| `qutebrowser/browser/eventfilter.py:157-194` | Wheel event handling (zoom, hint blocking) |
| `qtwebengine/src/core/smooth_scroll_controller.cpp` | C++ compositor smooth scroll controller |
| `qtwebengine/src/core/smooth_scroll_controller.h` | Controller class declaration |
| `qtwebengine/src/core/render_widget_host_view_qt.cpp` | `smoothScrollBy()` entry point |
| `qtwebengine/src/core/api/qwebenginepage.cpp` | `QWebEnginePage::smoothScrollBy()` public API |
