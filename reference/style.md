# Visual Style Canon

## Canonical font

JetBrains Mono is the canonical font family for this browser.

Use JetBrains Mono for:

- Qt/Python application UI text
- qutebrowser hint/keyhint/status/completion/prompt text
- Chromium/Blink-native UI overlays implemented for this fork
- native Chromium hint indicators
- default web font-family settings controlled by this fork

Fallbacks are acceptable only when JetBrains Mono is unavailable on the system. In
that case, use a fixed-width/monospace fallback so layout-sensitive browser UI
keeps deterministic widths.

## Implementation touch points

- `qutebrowser/app.py`: `CANONICAL_FONT_FAMILY`
- `qutebrowser/config/configdata.yml`: `fonts.default_family` and `fonts.web.family.*`
- `qtwebengine/src/3rdparty/chromium/third_party/blink/renderer/core/html/resources/html.css`: Blink user-agent font defaults
- `qtwebengine/src/3rdparty/chromium/third_party/blink/renderer/core/yeyito_hints/overlay.cc`: Chromium-native hint label typeface

When adding new visual surfaces, prefer wiring them to the existing config/default
font path. If the surface is below Python/Qt config reach, use `JetBrains Mono`
first and a monospace fallback list second.
