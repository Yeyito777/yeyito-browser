## About this project
You're in my fork of qutebrowser's source. This fork includes a custom build of QtWebEngine (Chromium/Blink) to enable deep browser engine modifications.

**Important**: Do NOT run git commands (commit, push, checkout, etc.) unless explicitly instructed by the user. The user manages git operations manually to avoid confusion about which commit they're on.

**Important**: After editing Blink/QtWebEngine C++ files, always run `./install.sh --dirty` to build and install. Don't wait for the user to ask.

## Reference Files - Read These First

| When user mentions... | Read this first |
|-----------------------|-----------------|
| **shader**, element shader, colors, CSS transforms | `reference/element-shader.md` |
| **build**, install, compile | `reference/build.md` |
| **bindings**, SIP, PyQt6-WebEngine, enum, WebAttribute | `reference/build.md` (Phase 4) |
| **new setting**, WebAttribute, mojom, IPC, preferences | `reference/adding-a-web-setting.md` |
| **dependencies**, packages, pacman, system updates, upstream sync | `reference/dependencies.md` |
| **pdf.js patching**, PDF.js, polyfill, "is not a function" | `reference/pdfjs-polyfills.md` |
| **user activation**, autoplay, video.play(), grant activation, session resume | `reference/user-activation.md` |
| **localStorage**, auth loss, cookies, tokens, login, rebuild persistence | `reference/local-storage-persistence.md` |
| **screenshot**, tab capture, CopyFromSurface, focus suppression | `reference/screenshot.md` |

## Custom QtWebEngine Build

This is a **monorepo** — qtwebengine, qtwebengine-chromium, and pyqt6-webengine sources live directly in the tree as plain directories (no submodules).

**If your task involves modifying QtWebEngine or Blink**, read these reference files:
- `reference/element-shader.md` - Element shader implementation spec
- `reference/build.md` - Build process, directory structure, verification
- `reference/adding-a-web-setting.md` - Full pipeline for adding a new QWebEngineSettings attribute (10 touch points across 9 files)
- `reference/dependencies.md` - System packages, what we build vs use from system

### Quick Reference

| Command | Purpose |
|---------|---------|
| `./install.sh` | Build (skips if commit unchanged) |
| `./install.sh --dirty` | Force rebuild with uncommitted changes |
| `~/.local/bin/qutebrowser` | Launch with custom QtWebEngine |

### Monorepo Structure

```
Qutebrowser/                     ← single repo (Yeyito777/yeyito-browser)
├── qutebrowser/                 ← Python browser code
├── qtwebengine/                 ← C++ engine (formerly Yeyito777/yeyitowebengine)
│   └── src/3rdparty/            ← Chromium source (formerly Yeyito777/qtwebengine-chromium)
│       └── chromium/...         ← Blink source lives here
└── pyqt6-webengine/             ← SIP bindings (formerly Yeyito777/pyqt6-webengine)
    └── sip/QtWebEngineCore/     ← SIP bindings (includes custom enum values)
```

### Key Blink Files (in `qtwebengine/src/3rdparty/chromium/`)

| File | Purpose |
|------|---------|
| `third_party/blink/renderer/core/css/resolver/style_resolver.cc` | Style resolution hook point |
| `third_party/blink/renderer/core/css/computed_style.h` | ComputedStyle object |
| `third_party/blink/public/common/switches.cc` | CLI flag definitions |
| `content/browser/browser_main_loop.cc` | Browser process init |

### Key Bindings Files (in `pyqt6-webengine/`)

| File | Purpose |
|------|---------|
| `sip/QtWebEngineCore/qwebenginesettings.sip` | WebAttribute enum (includes `ElementShaderEnabled`) |
| `pyproject.toml` | SIP build configuration |

### Build Times
- No changes: instant (skipped)
- Single .cc file: 1-5 minutes
- Full rebuild: ~2 hours
- SIP bindings rebuild: ~30-60 seconds

## Testing Environment (Python/qutebrowser)

The `.venv` directory contains a Python virtual environment with PyQt6 and test dependencies. To run tests:

```bash
source .venv/bin/activate
QT_QPA_PLATFORM=offscreen PYTHONPATH=. pytest tests/unit/path/to/test.py -v
```

The `QT_QPA_PLATFORM=offscreen` prevents Qt windows from appearing during tests.

Available packages: PyQt6, PyQt6-WebEngine, pytest, pytest-qt, pytest-mock, hypothesis, and other pytest plugins.
