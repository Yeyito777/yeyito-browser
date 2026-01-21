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
| `qutebrowser/browser/hints.py` | Python hint manager, coordinates hinting |
| `qutebrowser/browser/webelem.py` | Python web element abstraction |
| `qutebrowser/javascript/webelem.js` | JavaScript element finding and serialization |
| `qutebrowser/config/configdata.yml` | Default CSS selectors for hint groups |

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
       ├─► Filter by visibility (is_visible)
       └─► Serialize elements back to Python
       │
       ▼
HintManager._start_cb(elements)
       │
       ├─► Generate hint strings (letter/number/word)
       ├─► Create HintLabel widgets for each element
       └─► Enter hint mode
       │
       ▼
User types hint → HintManager._fire() → Target action
```

## JavaScript Element Finding

The core element finding logic is in `webelem.js`. The `find_css` function:

```javascript
funcs.find_css = (selector, only_visible) => {
    // Search in multiple containers:
    // 1. Main document
    // 2. Same-domain iframes
    // 3. Open shadow roots

    // Use Set to avoid duplicate elements
    const elemSet = new Set();

    // Query each container with the CSS selector
    for (const [container, frame] of containers) {
        for (const elem of container.querySelectorAll(selector)) {
            if (!elemSet.has(elem)) {
                elems.push([elem, frame]);
                elemSet.add(elem);
            }
        }
    }

    // Filter by visibility and serialize
    for (const [elem, frame] of elems) {
        if (!only_visible || is_visible(elem, frame)) {
            out.push(serialize_elem(elem, frame));
        }
    }
};
```

### Visibility Checking

An element is considered visible if:
1. CSS `visibility` is "visible"
2. CSS `display` is not "none"
3. CSS `opacity` is not "0" (except for certain framework classes)
4. The element's bounding rect is within the viewport
5. The element has at least one client rect

### Element Serialization

Each found element is serialized into a Python-usable object containing:
- `id`: Index in the elements array (for later manipulation)
- `rects`: Bounding rectangles (adjusted for iframe offsets)
- `tag_name`: Element tag (e.g., "A", "BUTTON")
- `class_name`: CSS classes
- `value`: Form element value
- `attributes`: All HTML attributes
- `text`: Text content
- `outer_xml`: Full HTML representation
- `caret_position`: Cursor position in text inputs
- `is_content_editable`: Whether element is editable

## CSS Selectors

The `hints.selectors` config option defines which elements to hint for each group.

### Default Groups

| Group | Purpose | Key Selectors |
|-------|---------|---------------|
| `all` | All clickable elements | `a`, `button`, `[onclick]`, `[role="button"]`, etc. |
| `links` | Hyperlinks only | `a[href]`, `area[href]`, `link[href]` |
| `images` | Images | `img` |
| `media` | Media elements | `audio`, `video`, `img` |
| `url` | Elements with URLs | `[src]`, `[href]` |
| `inputs` | Form inputs | `input[type="text"]`, `textarea`, etc. |

### Customizing Selectors

You can add custom groups or modify existing ones in your config:

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

## Performance Considerations

The hinting system uses a `Set` to track elements and avoid duplicates when searching across multiple containers (document, iframes, shadow roots). This provides O(1) duplicate checking.

Visibility checking involves DOM queries (`getComputedStyle`, `getBoundingClientRect`) which are relatively fast but can add up on pages with thousands of elements.

## See Also

- [JavaScript Hint Target](javascript-hinting.md) - Documentation for the `javascript` target
