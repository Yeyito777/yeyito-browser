# Context Menu Styling

Right-click context menus (page body menu, image menu, link menu, etc.) are `QMenu` instances created by QtWebEngine's C++ side. This document covers how they're instantiated, how styling is applied, and the custom alternating-background mechanism.

## How context menus are created

### C++ layer

`QWebEngineView::contextMenuEvent()` in `qtwebengine/src/webenginewidgets/api/qwebengineview.cpp` calls `createStandardContextMenu()`, which:

1. Allocates a `QMenu(this)` parented to the view
2. Populates it via `QContextMenuBuilder::initMenu()` using the current `QWebEngineContextMenuRequest` (what was right-clicked: link, image, selected text, page body, etc.)
3. Sets `Qt::WA_DeleteOnClose` so the menu self-destructs when dismissed
4. Returns the `QMenu*` (exposed to Python via SIP with `/Factory/` ownership)

The default `contextMenuEvent()` simply calls `menu->popup(event->globalPos())`.

### Python layer

`WebEngineView.contextMenuEvent()` in `qutebrowser/browser/webengine/webview.py` overrides the Qt virtual. When `colors.contextmenu.alternate.bg` is **not** set, it delegates to `super().contextMenuEvent(ev)` which hits the C++ path above. When the alternating config is set, it intercepts the menu creation (see below).

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

QSS has no `nth-child` or `alternate-background-color` support for `QMenu` items (those only work on `QAbstractItemView` subclasses). Alternating backgrounds are implemented via a custom `QMenu` subclass with an overridden `paintEvent`.

### `AlternatingContextMenu` class

Defined in `qutebrowser/browser/webengine/webview.py`:

```python
class AlternatingContextMenu(QMenu):
    def __init__(self, even_color, odd_color, parent=None)
    def paintEvent(self, event)
```

**Paint order:**

1. `paintEvent()` opens a `QPainter` on the widget
2. Iterates `self.actions()`, skipping separators, tracking a `visual_idx`
3. For each non-separator action, calls `self.actionGeometry(action)` to get its pixel rect
4. Fills the rect with `even_color` (idx % 2 == 0) or `odd_color`
5. Closes the painter
6. Calls `super().paintEvent(event)` — the normal QMenu paint draws text, icons, separators, and selected/disabled highlights **on top** of the pre-painted backgrounds

### Why per-instance QSS is needed

The `MainWindow` QSS sets `QMenu { background-color: <color>; }`. When `super().paintEvent()` runs, it fills the entire menu widget with that color, overwriting the alternating backgrounds painted in step 1–4. To prevent this, the `AlternatingContextMenu` instance gets a per-instance stylesheet:

```python
menu.setStyleSheet("QMenu { background-color: transparent; }")
```

This overrides only the `background-color` property for this specific menu. Other inherited properties (border, font, color, `::item:selected`, `::item:disabled`) cascade normally from the `MainWindow` QSS.

### Action lifecycle

`WebEngineView.contextMenuEvent()` calls `self.createStandardContextMenu()` to get the standard `QMenu` with all the browser-generated actions (back, forward, reload, copy image, etc.). It then creates an `AlternatingContextMenu` and transfers the actions:

```python
standard_menu = self.createStandardContextMenu()
menu = AlternatingContextMenu(even_color=alt_bg, odd_color=menu_bg, parent=self)
for action in standard_menu.actions():
    menu.addAction(action)
menu._source_menu = standard_menu  # prevent GC
```

`QMenu.addAction()` adds the `QAction` to the new menu's action list without re-parenting it — the actions remain children of `standard_menu`. The `_source_menu` reference prevents `standard_menu` from being garbage-collected (which would destroy the child actions) while the custom menu is alive. `WA_DeleteOnClose` is set on the custom menu so both menus are cleaned up when the user dismisses the context menu.

## Files involved

| File | Role |
|------|------|
| `qutebrowser/config/configdata.yml` | Config option definitions (`colors.contextmenu.*`) |
| `qutebrowser/mainwindow/mainwindow.py` | QSS template (`MainWindow.STYLESHEET`) with `QMenu` rules |
| `qutebrowser/config/stylesheet.py` | Jinja2 rendering and live-reload of QSS |
| `qutebrowser/browser/webengine/webview.py` | `AlternatingContextMenu` class and `WebEngineView.contextMenuEvent()` |
| `qtwebengine/src/webenginewidgets/api/qwebengineview.cpp` | C++ `createStandardContextMenu()` and default `contextMenuEvent()` |
