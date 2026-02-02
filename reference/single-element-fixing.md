# Single Element Fixing

A JavaScript-based system that automatically modifies DOM elements as they are added to the page. It applies the following rules:
- If the element has a gradient → `background-image: linear-gradient(to bottom, #00050f, #090d35) !important`
- If the element has a non-transparent background → `background-color: #00050f !important`
- If the element has a border → `border-color: #1d9bf0 !important`
- If the element has text and is NOT a code block → `color: #ffffff !important`
  - Code block detection includes: `<pre>`, `<code>`, elements with `.hljs`, `[class*="language-"]`, or `.textLayer` classes, and any descendants of these
- If the element is an SVG or SVG child → `fill: #ffffff !important` and `stroke: #ffffff !important`

The system uses MutationObserver to watch for new elements only (not attribute changes - reprocessing existing elements causes feedback loops with some sites), and handles Shadow DOM by intercepting `attachShadow`.

## Relevant Files
- `qutebrowser/javascript/element_fix.js` - Core JavaScript that detects and fixes elements with non-transparent backgrounds.
- `qutebrowser/browser/webengine/webenginetab.py` - Injects the element_fix.js script into pages via `_WebEngineScripts.init()`.

## Debugging / Diagnostics

The script includes comprehensive diagnostic logging. All logs are prefixed with `[qb-element-fix]` in the browser console.

### Console Functions (run in browser devtools or via `:jseval`)

| Function | Description |
|----------|-------------|
| `window._qb_fix_stats` | Object containing live stats (elements processed, errors, mutation rate, etc.) |
| `window._qb_fix_log_stats()` | Print formatted stats to console |
| `window._qb_fix_status()` | Print full status report |
| `window._qb_fix_disable()` | **Kill switch** - immediately stops all processing |
| `window._qb_fix_enable()` | Re-enable processing after disabling |

### Warning Signs in Logs
- `HIGH MUTATION RATE: X mutations/sec` - Possible feedback loop, script may be causing mutations that trigger itself
- `CRITICAL: Processing depth exceeded 10` - Infinite loop detected, processing aborted
- `Slow mutation batch: Xms` - Processing took >50ms, may cause UI jank
- `Processing large element tree: X elements` - Page has >5000 elements, initial processing may be slow

### Stats Object Fields
```javascript
{
  elementsProcessed: 0,  // Total elements checked
  elementsFixed: 0,      // Elements that had styles applied
  mutationBatches: 0,    // Number of mutation callbacks
  mutationsTotal: 0,     // Total mutations handled
  errors: 0,             // Errors caught during processing
  mutationsPerSecond: 0, // Current mutation rate
  isProcessing: false,   // Currently in mutation handler
  processingDepth: 0     // Re-entrancy depth (should be 0)
}
```

**IF YOU ARE AN AGENT AND READ THIS FILE**:
Make sure to modify it if any changer occur.
