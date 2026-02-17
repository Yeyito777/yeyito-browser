# Scrolling

Custom scrolling system — command layer, abstract interface, WebEngine implementation with JS-first approach + key-press fallback, smooth animation accumulator, per-element scrolling.

## Architecture

Three layers: user commands → abstract scroller interface → WebEngine implementation + JavaScript engine.

```
:scroll down
  → scrollcommands.scroll()
  → tab.scroller.down(count)
  → WebEngineScroller._scroll_with_js_or_key()
    ├→ JS scroll.scroll_focused(px_dx, px_dy, factor)
    │  ├→ _deepActiveElement() → _findScrollable()
    │  ├→ _accumulateScroll() batches deltas
    │  └→ _animateScroll() via requestAnimationFrame
    └→ fallback: fake key press (if JS returns false)
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
| `up/down/left/right` | `_scroll_with_js_or_key()` | JS-first with key fallback |
| `delta` | JS `scroll.scroll_delta` | Always uses smooth factor |
| `to_perc` | JS `scroll.to_perc` | Direct JS call |
| `to_point` | JS `window.scroll` | Direct JS call |
| `to_anchor` | `tab.load_url(url#fragment)` | Navigation-based |
| `delta_page` | JS `scroll.delta_page` | Fraction of viewport |
| `top/bottom` | Fake Home/End key | No JS involved |
| `page_up/page_down` | Fake PageUp/PageDown key | No JS involved |

### `_scroll_with_js_or_key(key, dx, dy, count)`

Core method for directional scrolling (lines 519-531):

1. Calculate pixel distance: `dx * count * 40`
2. Run JS `scroll.scroll_focused(px_dx, px_dy, factor)`
3. JS returns `true` if it found a scrollable element and handled it
4. If `false`, falls back to `_repeated_key_press(key, count)` — sends fake key events to Chromium

### `_update_pos(pos: QPointF)`

Called on every `scrollPositionChanged` signal (lines 534-578):

1. Converts `QPointF` to `QPoint` pixel position
2. Calculates percentage: `round(100 / scrollable_range * pos)`
3. Determines `_at_bottom` via `math.ceil(pos.y()) >= scrollable_y`
4. Emits `perc_changed` only when percentage actually changes (deduplicated)

## JavaScript Engine

`scroll.js` — runs in the page context as `window._qutebrowser.scroll`.

### Public functions

| Function | Called by | Purpose |
|----------|----------|---------|
| `scroll_focused(dx, dy, factor)` | `_scroll_with_js_or_key` | Scroll focused element; returns `bool` |
| `scroll_delta(dx, dy, factor)` | `delta()` | Scroll focused element or fall back to window |
| `to_perc(x, y)` | `to_perc()` | Scroll to percentage of scrollable range |
| `delta_page(x, y)` | `delta_page()` | Scroll by fraction of viewport via `window.scrollBy` |

### `scroll_focused` vs `scroll_delta`

- `scroll_focused` returns `false` if no focused scrollable element exists (Python falls back to key press)
- `scroll_delta` does the same search but falls back to `window` scrolling instead of returning false

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
| `scrolling.smooth_factor` | Float | `0.3` | JS animation speed — fraction of remaining distance per frame. 0.05 = buttery, 1.0 = instant |

Note: `scrolling.smooth` is the browser engine's built-in smooth scrolling. `scrolling.smooth_factor` controls qutebrowser's own JS-based smooth animation (the accumulator described above). They are independent.

## Source Files

| File | Role |
|------|------|
| `qutebrowser/components/scrollcommands.py` | `:scroll`, `:scroll-px`, `:scroll-to-perc`, `:scroll-to-anchor` |
| `qutebrowser/browser/browsertab.py:614-689` | `AbstractScroller` interface |
| `qutebrowser/browser/webengine/webenginetab.py:496-637` | `WebEngineScroller` implementation |
| `qutebrowser/javascript/scroll.js` | JS scrolling engine (element discovery, smooth accumulator) |
| `qutebrowser/config/configdata.yml:2148-2179` | `scrolling.*` settings |
| `qutebrowser/browser/eventfilter.py:157-194` | Wheel event handling (zoom, hint blocking) |
