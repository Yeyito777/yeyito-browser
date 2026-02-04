## About this project
You're in my fork of qutebrowser's source. This fork includes a custom build of QtWebEngine (Chromium/Blink) to enable deep browser engine modifications.

**Important**: Do NOT run git commands (commit, push, checkout, etc.) unless explicitly instructed by the user. This includes the qtwebengine submodule. The user manages git operations manually to avoid confusion about which commit they're on.

## Reference Files - Read These First

| When user mentions... | Read this first |
|-----------------------|-----------------|
| **shader**, element shader, colors, CSS transforms | `reference/element-shader.md` |
| **git** commands, commit, push, revert, submodule | `reference/submodules.md` |
| **build**, install, compile | `reference/build.md` |

## Custom QtWebEngine Build

This repo uses a **git submodule** (`qtwebengine/`) pointing to a custom fork of QtWebEngine. The custom build allows modifying Chromium's Blink engine directly.

**If your task involves modifying QtWebEngine or Blink**, read these reference files:
- `reference/element-shader.md` - Element shader implementation spec
- `reference/build.md` - Build process, directory structure, verification
- `reference/submodules.md` - Git submodule workflow

### Quick Reference

| Command | Purpose |
|---------|---------|
| `./install.sh` | Build (skips if commit unchanged) |
| `./install.sh --dirty` | Force rebuild with uncommitted changes |
| `~/.local/bin/qutebrowser` | Launch with custom QtWebEngine |
| `./scripts/ladder-commit "msg"` | Commit through all 3 submodule levels |
| `./scripts/ladder-commit "msg" --push` | Commit and push through all 3 levels |

### Workflow for Blink Changes (for the user, not the agent)

```bash
# 1. Edit Blink source
vim qtwebengine/src/3rdparty/chromium/...

# 2. Build and test
./install.sh --dirty
~/.local/bin/qutebrowser

# 3. Commit up the ladder (all 3 submodule levels)
./scripts/ladder-commit "Your commit message" --push
```

### Submodule Structure

```
Qutebrowser/                     ← main repo (Yeyito777/yeyito-browser)
└── qtwebengine/                 ← submodule (Yeyito777/yeyitowebengine)
    └── src/3rdparty/            ← nested submodule (Yeyito777/qtwebengine-chromium)
        └── chromium/...         ← Blink source lives here
```

### Key Blink Files (in `qtwebengine/src/3rdparty/chromium/`)

| File | Purpose |
|------|---------|
| `third_party/blink/renderer/core/css/resolver/style_resolver.cc` | Style resolution hook point |
| `third_party/blink/renderer/core/css/computed_style.h` | ComputedStyle object |
| `third_party/blink/public/common/switches.cc` | CLI flag definitions |
| `content/browser/browser_main_loop.cc` | Browser process init |

### Build Times
- No changes: instant (skipped)
- Single .cc file: 1-5 minutes
- Full rebuild: ~2 hours

## Testing Environment (Python/qutebrowser)

The `.venv` directory contains a Python virtual environment with PyQt6 and test dependencies. To run tests:

```bash
source .venv/bin/activate
QT_QPA_PLATFORM=offscreen PYTHONPATH=. pytest tests/unit/path/to/test.py -v
```

The `QT_QPA_PLATFORM=offscreen` prevents Qt windows from appearing during tests.

Available packages: PyQt6, PyQt6-WebEngine, pytest, pytest-qt, pytest-mock, hypothesis, and other pytest plugins.
