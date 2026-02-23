# Tab Screenshot — Technical Reference

Full-resolution tab screenshots captured via a custom C++ pipeline in QtWebEngine. Any tab (active or background) is captured at the primary monitor's native resolution (e.g. 1920x1080) regardless of the actual window size.

For usage and shell script interface, see [tab-runtime.md](tab-runtime.md#tab-screenshots).

## Architecture Overview

```
Python (tabruntime.py)
  └─ QWebEnginePage.captureScreenshot(QSize, callback)     [public API]
       └─ QWebEnginePagePrivate::captureScreenshot()        [private impl]
            └─ WebContentsAdapter::captureScreenshot()      [C++ core]
                 ├─ RWHV viewport override (GetViewBounds)
                 ├─ IncrementCapturerCount (prevent discard)
                 ├─ ForceRedrawForTesting
                 ├─ InsertVisualStateCallback
                 ├─ CopyFromSurface → PNG encode
                 └─ finishScreenshotRequest (cleanup)
```

## C++ Capture Pipeline

### 1. Viewport Override (`render_widget_host_view_qt.h/.cpp`)

`m_captureSizeOverride` (type `gfx::Rect`) is set before capture. When non-empty, `GetViewBounds()` returns this override instead of the actual widget size:

```cpp
gfx::Rect RenderWidgetHostViewQt::GetViewBounds()
{
    if (!m_captureSizeOverride.IsEmpty())
        return m_captureSizeOverride;
    return toGfx(delegateClient()->viewRectInDips());
}
```

`synchronizeVisualProperties()` calls `GetRequestedRendererSize()` which calls `GetViewBounds()`, so the renderer re-lays out content at the override size. This produces true full-resolution rendering, not upscaling.

`WebContentsAdapter` is declared as `friend` to access `m_captureSizeOverride` and `synchronizeVisualProperties()` directly.

### 2. Capture Sequence (`web_contents_adapter.cpp`)

`captureScreenshot(QSize outputSize, callback)`:

1. **Set viewport override** → `synchronizeVisualProperties()` propagates to renderer
2. **Increment capturer count** → `IncrementCapturerCount(stay_hidden=true)` prevents tab discard without triggering JS visibility change. Wrapped in `ScopedClosureRunner` (type-erased as `shared_ptr<void>` in the header) for automatic cleanup.
3. **Force redraw** → `ForceRedrawForTesting()` makes renderer produce a frame at the new viewport size
4. **Wait for frame** → `InsertVisualStateCallback()` fires when renderer submits a CompositorFrame
5. **Compositor delay** → 80ms `QTimer::singleShot` after the callback, allowing the compositor to process the submitted frame
6. **Copy from surface** → `CopyFromSurface(gfx::Rect(), outputSize, callback)` reads from the viz Surface via DelegatedFrameHost
7. **PNG encode** → `toQImage(SkBitmap)` → `QImage::save(&QBuffer, "PNG")`
8. **Safety timeout** → 15-second `QTimer::singleShot` calls `finishScreenshotRequest()` if capture hasn't completed

`finishScreenshotRequest(reqId, result)`:

1. Clears `m_captureSizeOverride` → `synchronizeVisualProperties()` restores original widget size
2. Moves callback out of map, erases entry (ScopedClosureRunner destructs → DecrementCapturerCount)
3. Fires callback with PNG data (or empty QByteArray on failure)

### 3. Request Tracking

```cpp
struct ScreenshotRequest {
    std::function<void(const QByteArray &)> callback;
    std::shared_ptr<void> capturer;  // type-erased ScopedClosureRunner
};
std::map<quint64, ScreenshotRequest> m_screenshotCallbacks;
```

Uses `m_nextRequestId++` (shared with print callbacks) for unique IDs. Weak pointers (`QWeakPointer<WebContentsAdapter>`) prevent use-after-free in async callbacks.

## Focus Suppression (Zero Input Disruption)

Background tabs must be temporarily made the current widget in QStackedLayout because Qt's render loop only sends BeginFrame to the visible widget. This would normally steal keyboard focus. Three layers of suppression prevent this:

### Layer 1: Delegate Client (`render_widget_host_view_qt_delegate_client.h/.cpp`)

```cpp
static int s_suppressFocusCount;  // when > 0, suppress focus events
```

In `handleFocusEvent()`: when suppression is active, silently accepts focus events without calling `host()->GotFocus()` or `host()->LostFocus()`. This keeps Chromium's renderer unaware of the tab switch.

### Layer 2: Delegate Item (`render_widget_host_view_qt_delegate_item.cpp`)

When `s_suppressFocusCount > 0`:
- `focusInEvent()` → `event->ignore()` + return (prevents QQuickItem focus acquisition)
- `focusOutEvent()` → `event->ignore()` + return (prevents active tab from losing QQuickItem focus)
- `keyPressEvent()` / `keyReleaseEvent()` → `event->accept()` + return (eats keyboard events)
- `event()` for `ShortcutOverride` → returns false (blocks shortcut processing)

### Layer 3: Python Re-show (`tabruntime.py`)

After `setCurrentIndex(target)` hides the original tab, the Python code immediately re-shows it:

```python
original_view.show()   # both tabs now visible, both get BeginFrame
original_view.raise_() # original on top — no visual change
focused_widget.setFocus()  # works because widget is visible again
```

This is the key insight: both tabs are visible and rendering simultaneously. The original stays on top with focus, so keyboard events go to the correct widget. The target renders underneath for capture.

### Public API

```cpp
// qwebenginepage.h — static methods
static void QWebEnginePage::suppressFocusNotifications();
static void QWebEnginePage::restoreFocusNotifications();
```

Exposed via SIP bindings in `qwebenginepage.sip`.

## Python Orchestration (`tabruntime.py`)

`screenshot_tab(tab_id_str, window_mode=False)`:

**Window mode**: `main_window.grab()` → save as PNG. Simple QWidget capture.

**Tab content mode** (background tabs):
1. `suppressFocusNotifications()` — activate C++ suppression
2. Save `QApplication.focusWidget()`
3. `tab_widget.blockSignals(True)` — prevent qutebrowser's `_on_current_changed` handler
4. `tab_widget.setCurrentIndex(target)` — target gets BeginFrame
5. `original_view.show()` + `raise_()` — re-show original on top
6. `focused_widget.setFocus()` — restore keyboard focus
7. `captureScreenshot(screen_size, callback)` — async C++ pipeline
8. Callback: `setCurrentIndex(original)`, `blockSignals(False)`, `restoreFocusNotifications()`, save PNG

**Tab content mode** (active tab): steps 1-6 skipped, just calls `captureScreenshot()` directly.

## Modified Files

### QtWebEngine (C++)

| File | Changes |
|------|---------|
| `src/core/render_widget_host_view_qt.h` | `friend class WebContentsAdapter`, `gfx::Rect m_captureSizeOverride` |
| `src/core/render_widget_host_view_qt.cpp` | `GetViewBounds()` override check |
| `src/core/render_widget_host_view_qt_delegate_client.h` | `static int s_suppressFocusCount` |
| `src/core/render_widget_host_view_qt_delegate_client.cpp` | Static definition, `handleFocusEvent()` guard |
| `src/core/render_widget_host_view_qt_delegate_item.cpp` | Focus/key/shortcut suppression |
| `src/core/web_contents_adapter.h` | `captureScreenshot`, `finishScreenshotRequest`, `ScreenshotRequest` struct |
| `src/core/web_contents_adapter.cpp` | Capture pipeline implementation |
| `src/core/api/qwebenginepage.h` | Public `captureScreenshot`, static focus methods |
| `src/core/api/qwebenginepage.cpp` | Implementation |
| `src/core/api/qwebenginepage_p.h` | Private `captureScreenshot` |

### PyQt6-WebEngine (SIP)

| File | Changes |
|------|---------|
| `sip/QtWebEngineCore/qwebenginepage.sip` | `captureScreenshot`, `suppressFocusNotifications`, `restoreFocusNotifications` |

### Qutebrowser (Python)

| File | Changes |
|------|---------|
| `qutebrowser/browser/tabruntime.py` | `screenshot_tab()`, `_write_screenshot_script()` |
| `qutebrowser/browser/commands.py` | `:tab-screenshot` command |

## Key References in Chromium/Qt Source

| What | Where |
|------|-------|
| `CopyFromSurface` (DFH path) | `render_widget_host_view_qt.cpp` (original, unchanged) |
| `synchronizeVisualProperties` | `render_widget_host_view_qt.cpp` — calls `GetRequestedRendererSize()` → `GetViewBounds()` |
| `toQImage(SkBitmap)` | `type_conversion.h` |
| `IncrementCapturerCount` | `chromium/content/public/browser/web_contents.h` |
| `ForceRedrawForTesting` | `chromium/content/browser/renderer_host/render_widget_host_impl.h` |
| `InsertVisualStateCallback` | `chromium/content/browser/renderer_host/render_widget_host_impl.h` |
| `printToPdf` (pattern reference) | `web_contents_adapter.cpp` — similar callback map pattern |
