# System Dependencies & Package Ownership

## Overview

This project builds QtWebEngine and PyQt6-WebEngine from source, replacing the system packages. Everything else comes from the system package manager (pacman). This document tracks what we build, what we depend on, and what to watch for during system updates.

## What We Build vs What We Use From the System

```
┌─────────────────────────────────────────────────────────────────────┐
│                        BUILT FROM SOURCE                            │
│                   (our forks, we control these)                     │
│                                                                     │
│  ┌──────────────────────┐  ┌─────────────────────────────────────┐  │
│  │ PyQt6-WebEngine      │  │ QtWebEngine (6.10.0)                │  │
│  │ (SIP bindings)       │  │ ├── Qt WebEngine libs               │  │
│  │ pyqt6-webengine/     │  │ │   libQt6WebEngineCore.so          │  │
│  │                      │  │ │   libQt6WebEngineWidgets.so       │  │
│  │ Provides:            │  │ │                                   │  │
│  │  - Python ↔ C++      │  │ └── Chromium/Blink (134.0.6998.208) │  │
│  │    bridge for our    │  │     └── V8, Skia, LevelDB, etc.     │  │
│  │    custom enums      │  │         (all bundled in build)      │  │
│  └──────────┬───────────┘  └──────────────┬──────────────────────┘  │
│             │ builds against              │ builds against          │
└─────────────┼─────────────────────────────┼─────────────────────────┘
              │                             │
              ▼                             ▼
┌──────────────────────────────────────────────────────────────────────┐
│                     SYSTEM PACKAGES (pacman)                         │
│                (we depend on these, don't modify them)               │
│                                                                      │
│  ┌──────────────────────┐  ┌──────────────────────────────────────┐  │
│  │ python-pyqt6         │  │ Qt6 base framework                   │  │
│  │ python-pyqt6-sip     │  │ qt6-base        (QtCore, QtGui,      │  │
│  │                      │  │                  QtWidgets, QtNetwork│  │
│  │ Provides:            │  │ qt6-declarative  (QtQml, QtQuick)    │  │
│  │  - QtCore bindings   │  │ qt6-positioning  (geolocation API)   │  │
│  │  - QtGui bindings    │  │ qt6-webchannel   (JS ↔ C++ bridge)   │  │
│  │  - QtWidgets bindings│  │                                      │  │
│  │  - SIP runtime       │  │ Plus transitive deps:                │  │
│  │                      │  │ icu, nss, ffmpeg, mesa, etc.         │  │
│  └──────────────────────┘  └──────────────────────────────────────┘  │
│                                                                      │
│  ┌──────────────────────────────────────────────────────────────┐    │
│  │ Build tools (needed for make install)                        │    │
│  │ cmake, ninja, gcc/clang, python, pip, sip tools              │    │
│  └──────────────────────────────────────────────────────────────┘    │
│                                                                      │
│  ┌──────────────────────────────────────────────────────────────┐    │
│  │ Runtime tools                                                │    │
│  │ python (3.x), pip, venv                                      │    │
│  └──────────────────────────────────────────────────────────────┘    │
└──────────────────────────────────────────────────────────────────────┘

  REMOVED (no longer needed):
    ✗ qt6-webengine           — we build our own (was causing version conflicts)
    ✗ python-pyqt6-webengine  — we build our own SIP bindings
    ✗ qutebrowser             — we run from source
```

## Package Status

| Package | Source | Status | Notes |
|---------|--------|--------|-------|
| `qt6-webengine` | ~~pacman~~ | **Removed** | We build our own. System version (6.10.2) conflicted with our custom build (6.10.0), causing qutebrowser to nuke Service Workers on every version mismatch. Phase 4 of scripts/install.sh patches `.pri`/`.prl` files so the SIP bindings build works without this package. The launcher sets `QTWEBENGINEPROCESS_PATH`, `QTWEBENGINE_RESOURCES_PATH`, and `QTWEBENGINE_LOCALES_PATH` to find runtime files in our custom install. |
| `python-pyqt6-webengine` | ~~pacman/pip~~ | **Removed** | We build custom SIP bindings (Phase 4 of scripts/install.sh) that include our custom enums like `ElementShaderEnabled`. |
| `qutebrowser` | ~~pacman~~ | **Removed** | We run from source via the venv. |
| `python-pyqt6` | pacman | **Keep** | Provides base bindings (QtCore, QtGui, QtWidgets). Our scripts/install.sh Phase 4 copies its `bindings/` directory as a foundation. |
| `python-pyqt6-sip` | pacman | **Keep** | SIP runtime needed by all PyQt6 bindings. |
| `qt6-base` | pacman | **Keep** | Core Qt6 libraries. Everything builds against this. |
| `qt6-declarative` | pacman | **Keep** | QtQml/QtQuick. Runtime dependency of WebEngine. |
| `qt6-positioning` | pacman | **Keep** | Geolocation APIs. Runtime dependency of WebEngine. |
| `qt6-webchannel` | pacman | **Keep** | JS-C++ bridge. Runtime dependency of WebEngine. |

## Version Pinning

Our custom build is pinned to a specific Qt version:

| Component | Our version | System version | Must match? |
|-----------|------------|----------------|-------------|
| Qt6 base (`qVersion()`) | N/A (uses system) | 6.10.x | System provides this |
| QtWebEngine (`qWebEngineVersion()`) | **6.10.0** | ~~6.10.2~~ removed | N/A — only ours exists now |
| Chromium | **134.0.6998.208** | ~~134.0.6998.208~~ removed | N/A |
| V8 serializer format | **15** | — | Stable across 6.10.x |
| Blink serializer format | **21** | — | Stable across 6.10.x |

## What to Watch During System Updates (`pacman -Syu`)

### Safe updates (no action needed)
- Minor `qt6-base` patch updates (6.10.x → 6.10.y) — ABI compatible
- `python-pyqt6` patch updates — just Python bindings, won't affect our WebEngine
- Kernel, mesa, ffmpeg, etc. — unrelated to our build

### Updates that need attention
- **`qt6-base` major bump (6.10 → 6.11+)**: Our QtWebEngine was built against 6.10 headers. A major Qt version bump may require rebuilding our QtWebEngine from source to match. Watch for ABI breakage (crashes on startup, missing symbols).
- **`python-pyqt6` major bump**: If the SIP ABI changes, our custom WebEngine bindings (Phase 4) may need a rebuild. Symptoms: import errors for `PyQt6.QtWebEngineCore`.

### How to verify after a system update
```bash
# Quick smoke test — does it launch?
~/.local/bin/qutebrowser

# If it crashes, rebuild everything:
make install-dirty
```

## Upstream Sync Strategy

We maintain three forks that diverge from upstream. Periodically syncing keeps us on supported Chromium versions and picks up security patches.

| Fork | Upstream | What to sync | Frequency |
|------|----------|-------------|-----------|
| `yeyitowebengine` | `qt/qtwebengine` | Qt's WebEngine patches, build system changes | When Qt releases a new minor (e.g., 6.11) |
| `qtwebengine-chromium` | `qt/qtwebengine-chromium` | Chromium security patches, V8 updates | Same cadence as above (Qt bundles specific Chromium versions) |
| `pyqt6-webengine` | PyQt6-WebEngine (PyPI) | SIP binding definitions, new API surface | When PyQt6-WebEngine releases |

### Sync procedure
1. Check Qt's release notes for the new version
2. Diff the upstream changes against our modifications (primarily in `style_resolver.cc` and related shader files)
3. Rebase or merge our changes onto the new upstream tag
4. Rebuild: `make install-dirty`
5. Test the element shader still works
6. Commit and push: `git add -A && git commit -m "Sync with upstream Qt 6.x.y" && git push`

### Files most likely to conflict on sync
- `third_party/blink/renderer/core/css/resolver/style_resolver.cc` — our shader hook
- `third_party/blink/public/common/switches.cc` — our CLI flags
- `content/browser/browser_main_loop.cc` — our verification log message
- `third_party/blink/public/mojom/webpreferences/web_preferences.mojom` — our `element_shader_enabled` field
- `third_party/blink/public/common/web_preferences/web_preferences_mojom_traits.h` — our serialization getter
- `third_party/blink/common/web_preferences/web_preferences_mojom_traits.cc` — our deserialization
- `pyqt6-webengine/sip/QtWebEngineCore/qwebenginesettings.sip` — our custom enum values

## Build-Time Implications of No System qt6-webengine

Without the system `qt6-webengine` package, `qmake6` has no built-in knowledge of WebEngine modules. Phase 4 of `scripts/install.sh` handles this via three mechanisms:

1. **`.pri` patching**: The CMake install puts module specs in `build/install/lib/qt6/mkspecs/modules/` using `$$QT_MODULE_LIB_BASE` (resolves to `/usr/lib`). We `sed` these to absolute paths pointing to our install dir.

2. **`.prl` patching**: Qt's `.prl` files list transitive link deps. We only redirect WebEngine-specific libraries (`libQt6WebEngineCore.so`, `libQt6WebEngineWidgets.so`, `libQt6WebEngineQuick.so`) to our install path. System libraries (`Qt6Quick`, `Qt6Gui`, etc.) stay at `$$[QT_INSTALL_LIBS]`.

3. **`QMAKEPATH`**: Set to `build/install/lib/qt6` so qmake discovers our custom `mkspecs/modules/*.pri` files.

Without these patches, `sip-install` fails with `Unknown module(s) in QT: webenginecore` or linker errors looking for `/usr/lib/libQt6WebEngineCore.so`.

**Note**: The patches are idempotent (safe to run multiple times). They're applied inside the Phase 4 conditional block, so they only run when bindings need rebuilding.

## Runtime Implications of No System qt6-webengine

The launcher (`~/.local/bin/qutebrowser`) must set five environment variables since Qt can't find WebEngine resources in the default system paths:

| Variable | Default (system) path | Our override |
|----------|----------------------|--------------|
| `LD_LIBRARY_PATH` | `/usr/lib` | `build/install/lib` |
| `QT_PLUGIN_PATH` | `/usr/lib/qt6/plugins` | `build/install/plugins` |
| `QTWEBENGINEPROCESS_PATH` | `/usr/lib/qt6/QtWebEngineProcess` | `build/install/lib/qt6/QtWebEngineProcess` |
| `QTWEBENGINE_RESOURCES_PATH` | `/usr/share/qt6/resources` | `build/install/share/qt6/resources` |
| `QTWEBENGINE_LOCALES_PATH` | `/usr/share/qt6/translations/qtwebengine_locales` | `build/install/share/qt6/translations/qtwebengine_locales` |

Without these, qutebrowser crashes at startup with "Couldn't find webengine resources dir" and "could not find QtWebEngineProcess".

## The Service Worker Nuking Problem (Resolved)

qutebrowser has a workaround (`misc/backendproblem.py:293-324`) that deletes the Service Worker directory whenever it detects a Qt or QtWebEngine version change. When the system `qt6-webengine` (6.10.2) coexisted with our custom build (6.10.0), the stored version in `~/.local/share/qutebrowser/state` would flip between the two, triggering the nuke and logging us out of Discord/WhatsApp.

**Fix**: Removed the system `qt6-webengine` package. Now only our custom build (6.10.0) exists, so the version is stable and the nuke never triggers.

If the problem recurs, check:
```bash
# What version is stored?
grep qtwe_version ~/.local/share/qutebrowser/state

# What version does the runtime report?
LD_LIBRARY_PATH=~/Workspace/Qutebrowser/build/install/lib \
  ~/.local/share/qutebrowser-venv/bin/python -c \
  "from PyQt6.QtWebEngineCore import qWebEngineVersion; print(qWebEngineVersion())"

# These two should match. If they don't, Service Workers will be nuked.
```
