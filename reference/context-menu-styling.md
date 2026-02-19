# Context Menu Styling

Right-click context menus (page body menu, image menu, link menu, etc.) are `QMenu` instances created by QtWebEngine's C++ side. This document covers how they're instantiated, how styling is applied, the custom alternating-background mechanism, and vim-style keyboard navigation.

## How context menus are created

### C++ layer

`QWebEngineView::contextMenuEvent()` in `qtwebengine/src/webenginewidgets/api/qwebengineview.cpp` calls `createStandardContextMenu()`, which:

1. Allocates a `QMenu(this)` parented to the view
2. Populates it via `QContextMenuBuilder::initMenu()` using the current `QWebEngineContextMenuRequest` (what was right-clicked: link, image, selected text, page body, etc.)
3. Sets `Qt::WA_DeleteOnClose` so the menu self-destructs when dismissed
4. Returns the `QMenu*` (exposed to Python via SIP with `/Factory/` ownership)

The default `contextMenuEvent()` simply calls `menu->popup(event->globalPos())`.

### Python layer

`WebEngineView.contextMenuEvent()` in `qutebrowser/browser/webengine/webview.py` overrides the Qt virtual. It always creates a `ContextMenu` (custom `QMenu` subclass) to provide vim-style keyboard navigation and smart default selection. The standard menu's actions are transferred to the custom menu.

## QSS-based styling (border, background, colors)

The `MainWindow.STYLESHEET` Jinja2 template in `qutebrowser/mainwindow/mainwindow.py` applies application-wide QSS rules for `QMenu`:

```css
QMenu {
    font: ...;                /* from fonts.contextmenu */
    background-color: ...;    /* from colors.contextmenu.menu.bg */
    color: ...;               /* from colors.contextmenu.menu.fg */
    border: 1px solid ...;    /* from colors.contextmenu.menu.border */
}

QMenu::item:selected {
    background-color: ...;    /* from colors.contextmenu.selected.bg */
    color: ...;               /* from colors.contextmenu.selected.fg */
}

QMenu::item:disabled {
    background-color: ...;    /* from colors.contextmenu.disabled.bg */
    color: ...;               /* from colors.contextmenu.disabled.fg */
}
```

These are registered on the `MainWindow` via `stylesheet.set_register(self)` and cascade to all child `QMenu` widgets. The stylesheet is re-rendered automatically when any referenced config value changes (handled by `qutebrowser/config/stylesheet.py`).

## Config options

Defined in `qutebrowser/config/configdata.yml` under the `colors.contextmenu.*` namespace:

| Option | Type | Description |
|--------|------|-------------|
| `colors.contextmenu.menu.bg` | `QssColor` (nullable) | Menu background color |
| `colors.contextmenu.menu.fg` | `QssColor` (nullable) | Menu text color |
| `colors.contextmenu.menu.border` | `QssColor` (nullable) | Border color (renders as `1px solid <color>`) |
| `colors.contextmenu.selected.bg` | `QssColor` (nullable) | Hovered/selected item background |
| `colors.contextmenu.selected.fg` | `QssColor` (nullable) | Hovered/selected item text color |
| `colors.contextmenu.disabled.bg` | `QssColor` (nullable) | Disabled item background |
| `colors.contextmenu.disabled.fg` | `QssColor` (nullable) | Disabled item text color |
| `colors.contextmenu.alternate.bg` | `QssColor` (nullable) | Alternating item background (even rows) |

All default to `null` (Qt default styling). The `menu.border` and `alternate.bg` options were added as custom extensions.

## Alternating row backgrounds

QSS has no `nth-child` or `alternate-background-color` support for `QMenu` items (those only work on `QAbstractItemView` subclasses). Alternating backgrounds are implemented via the `ContextMenu` subclass's overridden `paintEvent`.

### `ContextMenu` class

Defined in `qutebrowser/browser/webengine/webview.py`:

```python
class ContextMenu(QMenu):
    def __init__(self, *, even_color=None, odd_color=None, parent=None)
    def paintEvent(self, event)
    def keyPressEvent(self, event)
    def select_default_for_context(self, request)
```

**Paint order** (when alternating colors are configured):

1. `paintEvent()` opens a `QPainter` on the widget
2. Iterates `self.actions()`, skipping separators, tracking a `visual_idx`
3. For each non-separator action, calls `self.actionGeometry(action)` to get its pixel rect
4. Fills the rect with `even_color` (idx % 2 == 0) or `odd_color`
5. Closes the painter
6. Calls `super().paintEvent(event)` — the normal QMenu paint draws text, icons, separators, and selected/disabled highlights **on top** of the pre-painted backgrounds

When `even_color` and `odd_color` are `None`, `paintEvent()` skips straight to `super().paintEvent(event)`.

### Why per-instance QSS is needed

The `MainWindow` QSS sets `QMenu { background-color: <color>; }`. When `super().paintEvent()` runs, it fills the entire menu widget with that color, overwriting the alternating backgrounds painted in step 1–4. To prevent this, the `ContextMenu` instance gets a per-instance stylesheet when alternating is enabled:

```python
menu.setStyleSheet("QMenu { background-color: transparent; }")
```

This overrides only the `background-color` property for this specific menu. Other inherited properties (border, font, color, `::item:selected`, `::item:disabled`) cascade normally from the `MainWindow` QSS.

### Action lifecycle

`WebEngineView.contextMenuEvent()` calls `self.createStandardContextMenu()` to get the standard `QMenu` with all the browser-generated actions (back, forward, reload, copy image, etc.). It then creates a `ContextMenu` and transfers the actions:

```python
standard_menu = self.createStandardContextMenu()
menu = ContextMenu(even_color=alt_bg, odd_color=..., parent=self)
for action in standard_menu.actions():
    menu.addAction(action)
menu._source_menu = standard_menu  # prevent GC
```

`QMenu.addAction()` adds the `QAction` to the new menu's action list without re-parenting it — the actions remain children of `standard_menu`. The `_source_menu` reference prevents `standard_menu` from being garbage-collected (which would destroy the child actions) while the custom menu is alive. `WA_DeleteOnClose` is set on the custom menu so both menus are cleaned up when the user dismisses the context menu.

## Keyboard navigation

### Vim-style bindings

`ContextMenu.keyPressEvent()` maps vim keys to Qt's built-in `QMenu` keyboard handling:

| Key | Effect |
|-----|--------|
| `j` | Move to next item (synthesizes `Key_Down`) |
| `k` | Move to previous item (synthesizes `Key_Up`) |
| `Enter` | Trigger the active item (native `QMenu` behavior) |
| `Esc` | Close the menu (native `QMenu` behavior) |

The `j` and `k` keys create synthetic `QKeyEvent` objects with `Key_Down`/`Key_Up` and pass them to `super().keyPressEvent()`, so Qt's native menu navigation logic handles wrapping, skipping separators and disabled items, etc.

### Event filter bypass

Qutebrowser installs an application-level event filter (`qutebrowser/keyinput/eventfilter.py`) on `QApplication` that intercepts all `KeyPress`, `KeyRelease`, and `ShortcutOverride` events and routes them through the mode manager (`qutebrowser/keyinput/modeman.py`). Without special handling, the mode manager would consume key events (j/k/Esc/Enter) before they reach the popup `QMenu`.

The fix is in `EventFilter._handle_key_event()`:

```python
if objects.qapp.activePopupWidget() is not None:
    return False  # let the popup handle its own keyboard events
```

`QApplication.activePopupWidget()` returns the currently active popup widget (e.g., a `QMenu`). When a popup is active, the event filter returns `False` immediately, bypassing the mode manager and letting Qt deliver the event to the popup's `keyPressEvent()`.

### Smart default selection

`ContextMenu.select_default_for_context(request)` uses `QWebEngineContextMenuRequest` (from `self.lastContextMenuRequest()`) to determine what was right-clicked and pre-selects a sensible action via `QMenu.setActiveAction()`:

| Context | Default action |
|---------|---------------|
| Image | "Copy image" |
| Video | "Save media" |
| Audio | "Copy media address" |
| Link | "Open link in new tab" |
| Editable field | "Paste" |
| Selected text | "Copy" |
| Plain page | "View page source" |

Context detection priority: media type → link URL → editable → selected text → page. If the target action text isn't found in the menu, falls back to the first enabled non-separator action.

The context type is determined from the request object:
- `request.mediaType()` — checks against `MediaTypeImage`, `MediaTypeVideo`, `MediaTypeAudio`
- `request.linkUrl().isValid()` — link context
- `request.isContentEditable()` — editable field
- `request.selectedText()` — text selection

## Files involved

| File | Role |
|------|------|
| `qutebrowser/config/configdata.yml` | Config option definitions (`colors.contextmenu.*`) |
| `qutebrowser/mainwindow/mainwindow.py` | QSS template (`MainWindow.STYLESHEET`) with `QMenu` rules |
| `qutebrowser/config/stylesheet.py` | Jinja2 rendering and live-reload of QSS |
| `qutebrowser/browser/webengine/webview.py` | `ContextMenu` class and `WebEngineView.contextMenuEvent()` |
| `qutebrowser/keyinput/eventfilter.py` | App-level event filter with popup bypass in `_handle_key_event()` |
| `qtwebengine/src/webenginewidgets/api/qwebengineview.cpp` | C++ `createStandardContextMenu()` and default `contextMenuEvent()` |
| `pyqt6-webengine/sip/QtWebEngineCore/qwebenginecontextmenurequest.sip` | SIP bindings for `QWebEngineContextMenuRequest` |
