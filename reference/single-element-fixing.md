# Single Element Fixing

A JavaScript-based system that automatically modifies DOM elements as they are added to the page. It applies the following rules:
- If the element has a gradient → `background-image: linear-gradient(to bottom, #00050f, #090d35) !important`
- If the element has a non-transparent background → `background-color: #00050f !important`
- If the element has a border → `border-color: #1d9bf0 !important`
- If the element has text and is NOT a code block → `color: #ffffff !important`
  - Code block detection includes: `<pre>`, `<code>`, elements with `.hljs`, `[class*="language-"]`, or `.textLayer` classes, and any descendants of these
- If the element is an SVG or SVG child → `fill: #ffffff !important` and `stroke: #ffffff !important`

The system uses MutationObserver to watch for new elements and attribute changes, handles Shadow DOM by intercepting `attachShadow`, and periodically re-processes elements to catch dynamically styled content.

## Relevant Files
- `qutebrowser/javascript/element_fix.js` - Core JavaScript that detects and fixes elements with non-transparent backgrounds.
- `qutebrowser/browser/webengine/webenginetab.py` - Injects the element_fix.js script into pages via `_WebEngineScripts.init()`.

**IF YOU ARE AN AGENT AND READ THIS FILE**:
Make sure to modify it if any changer occur.
