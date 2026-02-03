# Qutebrowser Hinting System

This document describes how the hinting system works in qutebrowser, covering the flow from user input to element selection and action execution.

## Overview

When you press `f` (or another hint-triggering key), qutebrowser:
1. Determines which CSS selector to use based on the hint group
2. Executes JavaScript to find matching elements in the page
3. Filters elements by visibility
4. Assigns hint labels (letters/numbers/words) to each element
5. Displays visual overlays on the page
6. Waits for user to type a hint or filter text
7. Executes the target action on the selected element

## Architecture

### Key Files

| File | Purpose |
|------|---------|
| `qutebrowser/browser/hints.py` | Python hint manager, coordinates hinting, creates HintLabel widgets |
| `qutebrowser/browser/webelem.py` | Python web element abstraction |
| `qutebrowser/javascript/webelem.js` | JavaScript element finding, visibility checking, and serialization |
| `qutebrowser/config/configdata.yml` | Default CSS selectors for hint groups (lines 1810-1890) |

### Flow Diagram

```
User presses 'f'
       │
       ▼
HintManager.start()
       │
       ├─► Get CSS selector from config (hints.selectors[group])
       │
       ▼
tab.elements.find_css(selector, only_visible=True)
       │
       ▼
JavaScript: webelem.find_css()
       │
       ├─► Query document, iframes, shadow DOMs
       ├─► If :qb-hover in selector, also run CSS hover detection
       ├─► Filter by visibility (is_visible)
       └─► Serialize elements back to Python
       │
       ▼
HintManager._start_cb(elements)
       │
       ├─► Generate hint strings (letter/number/word)
       ├─► Create HintLabel widgets (batched, deferred show)
       └─► Enter hint mode
       │
       ▼
User types hint → HintManager._fire() → Target action
```

## JavaScript Element Finding (`webelem.js`)

### Core Function: `find_css(selector, only_visible)`

The core element finding logic searches multiple containers:
1. Main document
2. Same-domain iframes
3. Open shadow roots

```javascript
funcs.find_css = (selector, only_visible) => {
    // Check for special :qb-hover marker
    const includeCssHover = selector.includes(":qb-hover");

    // Find elements via CSS selectors
    for (const [container, frame] of containers) {
        for (const elem of container.querySelectorAll(selector)) {
            elems.push([elem, frame]);
        }
    }

    // If :qb-hover, also find elements with CSS :hover rules
    if (includeCssHover) {
        const hoverElems = find_elements_with_css_hover(containers);
        // merge into elems...
    }

    // Filter by visibility and serialize
    for (const [elem, frame] of elems) {
        if (!only_visible || is_visible(elem, frame)) {
            out.push(serialize_elem(elem, frame, includeCssHover));
        }
    }
};
```

### Visibility Checking (`is_visible`, `is_hidden_css`)

An element is considered **visible** if:
1. CSS `visibility` is "visible"
2. CSS `display` is not "none"
3. CSS `opacity` is not "0" (except for ACE editor and Bootstrap framework classes)
4. **No ancestor has `opacity: 0`** (catches Discord-style hover widgets)
5. The element's bounding rect is within the viewport
6. **Element is at least 4x4 pixels** (catches 1px-wide hidden elements)
7. The element has at least one client rect

### Element Serialization (`serialize_elem`)

Supports two modes:

**Full serialization** (default, used by `all` selector):
- `id`: Index in the elements array
- `rects`: Bounding rectangles (adjusted for iframe offsets)
- `tag_name`: Element tag (e.g., "A", "BUTTON")
- `class_name`: CSS classes
- `value`: Form element value
- `attributes`: All HTML attributes
- `text`: Text content
- `outer_xml`: Full HTML representation
- `caret_position`: Cursor position in text inputs
- `is_content_editable`: Whether element is editable

**Lightweight serialization** (used by `hoverables` selector):
- `id`, `rects`, `tag_name` only
- Other fields set to empty defaults
- Significantly faster for large element counts

## CSS Hover Detection (`find_elements_with_css_hover`)

The `:qb-hover` marker in a selector triggers CSS hover detection, which:

### Phase 1: Scan Stylesheets
- Iterates through all same-origin stylesheets
- Finds CSS rules containing `:hover`
- Extracts base selectors (e.g., `.message:hover` → `.message`)
- **Skips trivial properties**: `cursor`, `outline`, `text-decoration` (these don't indicate meaningful hover interactions)

### Phase 2: Query DOM
- Queries DOM once per unique selector (optimized from per-rule)
- Uses Set for O(1) duplicate checking

### Phase 3: Smart Filtering (large pages only)
For pages with >200 hover candidates (e.g., Discord):
- **Filters to elements with hidden clickable children**
- Clickable = `a`, `button`, `[onclick]`, `[role="button"]`, `[tabindex]`, etc.
- Hidden = `visibility: hidden`, `display: none`, or `opacity: 0`
- This dramatically reduces hints on complex pages while keeping actionable hovers

For pages with ≤200 candidates: returns all candidates without filtering.

## CSS Selectors

The `hints.selectors` config option defines which elements to hint for each group.

### Default Groups

| Group | Purpose | Key Selectors |
|-------|---------|---------------|
| `all` | All clickable elements | `a`, `button`, `[onclick]`, `[role="button"]`, `[tabindex]`, etc. |
| `links` | Hyperlinks only | `a[href]`, `area[href]`, `link[href]` |
| `images` | Images | `img` |
| `media` | Media elements | `audio`, `video`, `img` |
| `url` | Elements with URLs | `[src]`, `[href]` |
| `inputs` | Form inputs | `input[type="text"]`, `textarea`, etc. |
| `hoverables` | Elements with hover behavior | Attribute selectors + `:qb-hover` magic marker |

### The `hoverables` Selector Group

The `hoverables` group combines:

**Attribute-based selectors:**
- `[title]`, `[data-tooltip]`, `[data-tip]` - tooltip elements
- `[aria-describedby]` - ARIA descriptions
- `[onmouseover]`, `[onmouseenter]`, `[onmousemove]` - mouse event handlers
- `[role="article"]`, `[aria-roledescription]` - content items (Discord messages, etc.)
- `abbr`, `acronym` - abbreviations

**Magic marker:**
- `:qb-hover` - triggers CSS hover detection (see above)

### The `:qb-hover` Magic Marker

When `:qb-hover` appears in a selector:
1. It's stripped from the selector before CSS querying
2. CSS hover detection is triggered (`find_elements_with_css_hover`)
3. Lightweight serialization is used (faster)
4. Results from both attribute selectors and CSS hover detection are merged

### Customizing Selectors

```python
c.hints.selectors['custom'] = ['div.my-class', '[data-clickable]']
```

### Framework Support

The default `all` selector includes framework-specific attributes:
- Angular: `[ng-click]`, `[ngClick]`, `[data-ng-click]`, `[x-ng-click]`
- ARIA: `[role="button"]`, `[role="link"]`, `[aria-haspopup]`, etc.

## Hint Targets

The `target` parameter determines what happens when a hint is selected:

| Target | Action |
|--------|--------|
| `normal` | Open link (follows `tabs.background` setting) |
| `current` | Open in current tab |
| `tab` | Open in new tab |
| `tab-fg` | Open in new foreground tab |
| `tab-bg` | Open in new background tab |
| `window` | Open in new window |
| `hover` | Hover over element |
| `right-click` | Right-click element |
| `yank` | Copy URL to clipboard |
| `yank-primary` | Copy URL to primary selection |
| `run` | Run command with URL |
| `fill` | Fill command line |
| `download` | Download the link |
| `userscript` | Run userscript with URL |
| `javascript` | Execute JavaScript on element |
| `spawn` | Spawn external command |
| `delete` | Delete element from DOM |

## Python-Side Optimizations (`hints.py`)

### Batched HintLabel Creation

HintLabels are created with deferred operations for performance:

```python
# Create labels with deferred show, positioning, sizing
for elem, string in zip(elems, strings):
    label = HintLabel(elem, self._context, show=False,
                      connect_signals=False, position=False)
    label.update_text('', string, adjust_size=False)

# Batch: adjustSize, position, and show all labels
for label in self._context.all_labels:
    label.adjustSize()
    label._move_to_elem()
    label.show()
```

This reduces Qt paint cycles from N to ~1.

### HintLabel Parameters

| Parameter | Default | Purpose |
|-----------|---------|---------|
| `show` | `True` | Whether to call `show()` in constructor |
| `connect_signals` | `True` | Whether to connect `contents_size_changed` signal |
| `position` | `True` | Whether to call `_move_to_elem()` in constructor |
| `adjust_size` | `True` | Whether to call `adjustSize()` in `update_text()` |

## Performance Considerations

### JavaScript Side

| Technique | Applied To | Benefit |
|-----------|------------|---------|
| Lightweight serialization | `hoverables` | Skips expensive `outerHTML`, `textContent`, attributes |
| Selector deduplication | CSS hover detection | Query DOM once per unique selector |
| Trivial property filtering | CSS hover detection | Skip cursor/outline-only hover rules |
| Smart filtering (>200 elements) | CSS hover detection | Dramatically reduces hints on complex pages |
| Min size check (4px) | All selectors | Filters 1px-wide hidden elements |
| Ancestor opacity check | All selectors | Catches parent containers with `opacity: 0` |

### Python Side

| Technique | Applied To | Benefit |
|-----------|------------|---------|
| Batched `show()` | All selectors | Reduces paint cycles |
| Batched `adjustSize()` | All selectors | Reduces layout recalculations |
| Batched `_move_to_elem()` | All selectors | Batches positioning |
| Skip signal connections | All selectors | Reduces overhead during creation |

## Hint Modes

The `hints.mode` setting controls how hints are labeled:

- `letter`: Use characters from `hints.chars` (e.g., "a", "s", "as")
- `number`: Use numeric hints (e.g., "1", "2", "12")
- `word`: Use words based on element content

## Related Configuration

| Setting | Description |
|---------|-------------|
| `hints.auto_follow` | When to auto-follow single matches |
| `hints.auto_follow_timeout` | Delay before auto-following |
| `hints.chars` | Characters used for letter hints |
| `hints.dictionary` | Word list for word hints |
| `hints.find_implementation` | Use JS or Python for positioning |
| `hints.hide_unmatched_rapid_hints` | Hide hints in rapid mode |
| `hints.leave_on_load` | Exit hint mode on page load |
| `hints.min_chars` | Minimum hint length |
| `hints.mode` | Default hint mode (letter/number/word) |
| `hints.next_regexes` | Patterns for "next page" detection |
| `hints.padding` | Visual padding around hints |
| `hints.prev_regexes` | Patterns for "previous page" detection |
| `hints.radius` | Corner radius of hint labels |
| `hints.scatter` | Distribute hints evenly |
| `hints.selectors` | CSS selectors per group |
| `hints.uppercase` | Display hints in uppercase |

## Interaction with Iframes and Shadow DOM

The hinting system searches for elements in:
1. **Main document**: The primary page content
2. **Same-domain iframes**: Iframes that don't trigger CORS errors
3. **Open shadow roots**: Shadow DOM with `mode: "open"`

Cross-origin iframes and closed shadow roots cannot be searched due to browser security restrictions.

## See Also

- [JavaScript Hint Target](javascript-hinting.md) - Documentation for the `javascript` target

---

**Note for AI agents**: If you make changes that affect the accuracy of this document, please update it accordingly.
