# PDF.js Polyfills

## Overview

Qutebrowser uses [PDF.js](https://mozilla.github.io/pdf.js/) to render PDFs inline via `qute://pdfjs/`. The system-installed PDF.js (`/usr/share/pdf.js/`) targets modern browsers (primarily Firefox), so it often uses JavaScript features that our QtWebEngine (Chromium 134) doesn't support yet. We bridge these gaps with **polyfills** — small shims that implement missing methods using older JS features.

## How It Works

```
User opens a PDF
       │
       ▼
pdfjs.py builds the viewer HTML page
       │
       ▼
_get_polyfills() reads pdfjs_polyfills.js
       │
       ▼
Polyfill code is injected into a <script> tag BEFORE PDF.js runs
       │
       ▼
PDF.js calls toHex() / URL.parse() / etc. → finds the polyfill → works
```

### Key files

| File | Purpose |
|------|---------|
| `qutebrowser/javascript/pdfjs_polyfills.js` | The polyfill definitions (the only file you edit) |
| `qutebrowser/browser/pdfjs.py` | Loads polyfills and injects them into the viewer HTML |

## Current Polyfills

| Method | Required by Chromium | Polyfilled since |
|--------|---------------------|------------------|
| `Promise.withResolvers()` | 119 (QtWebEngine 6.8) | Already existed upstream |
| `URL.parse()` | 126 (QtWebEngine 6.9) | Already existed upstream |
| `Uint8Array.prototype.toHex()` | 140 | Our addition |
| `Uint8Array.fromHex()` | 140 | Our addition |
| `Uint8Array.prototype.toBase64()` | 140 | Our addition |
| `Uint8Array.fromBase64()` | 140 | Our addition |
| `Map.prototype.getOrInsertComputed()` | 141 | Our addition |
| `Map.prototype.getOrInsert()` | 141 | Our addition |

The Uint8Array hex/base64 methods are from the [TC39 "Uint8Array to/from base64 and hex" proposal](https://github.com/tc39/proposal-arraybuffer-base64). PDF.js v5.4.624 started using `toHex()` for document fingerprinting, causing `hashOriginal.toHex is not a function` errors on Chromium <140.

The Map upsert methods are from the [TC39 "Map.prototype.upsert" proposal](https://github.com/tc39/proposal-upsert). PDF.js v5.5.207 uses `getOrInsertComputed()` extensively for caching (method promises, intent states, font caches, etc.), causing `this[#methodPromises].getOrInsertComputed is not a function` errors on Chromium <141.

## Adding a New Polyfill

When PDF.js updates and breaks with a new "X is not a function" error:

1. **Identify the method** from the error message
2. **Check [caniuse.com](https://caniuse.com)** to see which Chromium version ships it
3. **Add a block** to `qutebrowser/javascript/pdfjs_polyfills.js` following the existing pattern:

```javascript
// Chromium <version>
// https://caniuse.com/mdn-<path>
if (typeof <object>.<method> === "undefined") {
    <object>.<method> = function(...) {
        // implementation using older JS features
    }
}
```

No build step needed — the file is read at runtime. Just restart qutebrowser.

## Why This Happens

- PDF.js is developed primarily for **Firefox**, which ships new JS features on a faster cadence
- Our QtWebEngine is based on **Chromium 134** (Qt 6.10)
- PDF.js v5.x dropped its "legacy" build entirely, so there's no backwards-compatible option
- As PDF.js adopts newer JS features, we'll need to keep adding polyfills until our Chromium version catches up

## Historical Precedent

This is a recurring pattern. [qutebrowser#7335](https://github.com/qutebrowser/qutebrowser/issues/7335) was the same issue in 2022: PDF.js v2.15 used `Array.prototype.at()` and `structuredClone`, unavailable in Qt5's QtWebEngine (Chromium 87). The polyfill approach was adopted then and has been the standard fix since.
