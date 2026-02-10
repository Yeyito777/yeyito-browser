# QtWebEngine Build Process

## Overview

To implement the element shader (see `element-shader.md`), we need to modify Chromium's Blink engine. Since QtWebEngine bundles Chromium, we:

1. **Fork QtWebEngine** to our own private repository
2. **Make our Blink modifications** and commit them to our fork
3. **Use a git submodule** pointing to our fork (not Qt's upstream)
4. **Build** and use environment variables to load our modified libraries, process binary, and resources

**Key principle**: Keep `./install.sh` as the single entry point. First build takes hours, but subsequent builds are fast (seconds to minutes) thanks to incremental compilation.

## Current Setup

- **QtWebEngine fork**: https://github.com/Yeyito777/yeyitowebengine
- **Chromium fork**: https://github.com/Yeyito777/qtwebengine-chromium
- **PyQt6-WebEngine fork**: https://github.com/Yeyito777/pyqt6-webengine
- **Branch**: `main` (all repos)
- **Base version**: Qt 6.10.0
- **Verification**: Custom log message in `browser_main_loop.cc`

## Architecture

```
Qt's upstream                   Your forks (GitHub)                 Your main repo
─────────────                   ───────────────────                 ──────────────
qt/qtwebengine                  Yeyito777/yeyitowebengine           Yeyito777/yeyito-browser
qt/qtwebengine-chromium         Yeyito777/qtwebengine-chromium      ├── qtwebengine/ ──▶ yeyitowebengine
PyQt6-WebEngine (PyPI)          Yeyito777/pyqt6-webengine           │   └── src/3rdparty/ ──▶ qtwebengine-chromium
                                                                    └── pyqt6-webengine/ ──▶ pyqt6-webengine
```

When someone clones your repo with `--recurse-submodules`, they get all forks with all your changes.

## Directory Structure

```
Qutebrowser/
├── qtwebengine/                          # Submodule → YOUR fork
│   └── src/3rdparty/chromium/
│       └── content/browser/
│           └── browser_main_loop.cc      ← Current verification log
├── pyqt6-webengine/                      # Submodule → YOUR fork (SIP bindings)
│   └── sip/QtWebEngineCore/
│       └── qwebenginesettings.sip        ← Has ElementShaderEnabled enum
├── build/                                 # Gitignored (~50-100GB)
│   ├── qtwebengine/                       # Ninja build cache, .o files
│   └── install/                           # Built libraries + runtime files
│       ├── lib/
│       │   ├── libQt6WebEngineCore.so.6   ← Your modified library (~1.5GB)
│       │   └── qt6/
│       │       └── QtWebEngineProcess     ← Renderer subprocess binary
│       └── share/qt6/
│           ├── resources/                 ← .pak resource files
│           └── translations/
│               └── qtwebengine_locales/   ← Locale .pak files
├── install.sh                             # Single entry point
└── .gitignore                             # Contains "build/"
```

## Build Dependencies (Arch Linux)

```bash
sudo pacman -S cmake ninja gn gperf nodejs python-html5lib qt6-tools
```

Full list of required packages:
- `cmake` - Build system generator
- `ninja` - Fast build tool
- `gn` - Generate Ninja files for Chromium
- `gperf` - Perfect hash function generator
- `nodejs` - JavaScript runtime (v14.9+ required)
- `python-html5lib` - HTML5 parsing for Python
- `qt6-tools` - Qt6 development tools

## Build Times (Actual)

On a 12-core system with 31GB RAM:

| Scenario | Time | Notes |
|----------|------|-------|
| First build | ~2 hours | 22769 Chromium targets + Qt wrapper |
| No changes | ~15 seconds | Ninja checks timestamps |
| Single `.cc` file change | 1-5 minutes | Recompiles affected targets |
| Chromium submodule download | ~1 hour | ~6GB compressed data |
| PyQt6-WebEngine bindings (first) | ~30-60 seconds | SIP generates + compiles C++ wrappers |
| PyQt6-WebEngine bindings (no changes) | instant | Commit tracking skips build |

## Build System

QtWebEngine uses **CMake** + **Ninja**:

1. **CMake** (first time only): Generates `build.ninja` file with all build rules
2. **Ninja**: Executes build, tracks file timestamps, only rebuilds what changed

```
./install.sh
     │
     ▼
┌─────────────────────────────────────────────────────────┐
│  Phase 1: Check submodules initialized                  │
│  git submodule update --init --recursive                │
└─────────────────────────────────────────────────────────┘
     │
     ▼
┌─────────────────────────────────────────────────────────┐
│  Phase 2: CMake configure (first time only)             │
│  cmake -S qtwebengine -B build/qtwebengine -GNinja      │
│  Then: Ninja build + install (incremental)              │
└─────────────────────────────────────────────────────────┘
     │
     ▼
┌─────────────────────────────────────────────────────────┐
│  Phase 3: Python venv + qutebrowser install             │
└─────────────────────────────────────────────────────────┘
     │
     ▼
┌─────────────────────────────────────────────────────────┐
│  Phase 4: Build custom PyQt6-WebEngine bindings         │
│  1. Patch .pri/.prl files to use custom install paths   │
│     (no system qt6-webengine; qmake needs absolute paths)│
│  2. QMAKEPATH → custom mkspecs for module discovery     │
│  3. sip-install from pyqt6-webengine/ submodule         │
│  4. Installs .abi3.so into venv site-packages           │
└─────────────────────────────────────────────────────────┘
     │
     ▼
┌─────────────────────────────────────────────────────────┐
│  Phase 5: Create launcher script                        │
│  Sets 5 env vars: LD_LIBRARY_PATH, QT_PLUGIN_PATH,     │
│  QTWEBENGINEPROCESS_PATH, QTWEBENGINE_RESOURCES_PATH,   │
│  QTWEBENGINE_LOCALES_PATH → all point to build/install/ │
└─────────────────────────────────────────────────────────┘
```

## How the Runtime Override Works

Two layers of override work together, with no system `qt6-webengine` package required:

**Layer 1: C++ shared libraries + runtime files** — The launcher sets five environment variables that redirect the dynamic linker, Qt plugin system, and WebEngine subprocess/resources to our custom build in `build/install/`. This is where Blink modifications (element shader, scrollbar theming, etc.) live.

| Variable | Points to | Purpose |
|----------|-----------|---------|
| `LD_LIBRARY_PATH` | `build/install/lib` | Load our custom `.so` libraries instead of system |
| `QT_PLUGIN_PATH` | `build/install/plugins` | Qt plugin discovery |
| `QTWEBENGINEPROCESS_PATH` | `build/install/lib/qt6/QtWebEngineProcess` | Chromium renderer subprocess binary |
| `QTWEBENGINE_RESOURCES_PATH` | `build/install/share/qt6/resources` | `.pak` resource files (DevTools, error pages, etc.) |
| `QTWEBENGINE_LOCALES_PATH` | `build/install/share/qt6/translations/qtwebengine_locales` | Locale `.pak` files |

**Layer 2: Python bindings** — Custom-built `PyQt6.QtWebEngineCore.abi3.so` in the venv knows about our custom C++ API additions (e.g., `ElementShaderEnabled` enum). The venv's site-packages takes priority over system site-packages, so our custom binding module is loaded instead of the stock one.

```
At runtime:
  Python imports PyQt6.QtWebEngineCore
       │
       ▼
  Finds custom .abi3.so in venv (knows about ElementShaderEnabled)
       │
       │ calls setAttribute(ElementShaderEnabled, True)
       ▼
  SIP marshals enum value 38 to C++ correctly
       │
       ▼
  Dynamic linker loads YOUR libQt6WebEngineCore.so.6 via LD_LIBRARY_PATH
       │
       ▼
  C++ QWebEngineSettings receives value 38 → enables element shader
```

**Verification**: The system Qt reports version 6.10.2, while our build reports 6.10.0.

**Launcher script** (`~/.local/bin/qutebrowser`):
```bash
#!/usr/bin/env bash
export LD_LIBRARY_PATH="…/build/install/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="…/build/install/plugins${QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}"
export QTWEBENGINEPROCESS_PATH="…/build/install/lib/qt6/QtWebEngineProcess"
export QTWEBENGINE_RESOURCES_PATH="…/build/install/share/qt6/resources"
export QTWEBENGINE_LOCALES_PATH="…/build/install/share/qt6/translations/qtwebengine_locales"
exec …/qutebrowser-venv/bin/python -m qutebrowser "$@"
```

## Phase 4 Details: SIP Bindings Without System qt6-webengine

Since the system `qt6-webengine` package is removed (see `dependencies.md`), Phase 4 must teach qmake where to find our custom WebEngine modules. This involves three patching steps before `sip-install` runs:

### 1. `.pri` module spec patching

CMake installs `.pri` files (e.g., `qt_lib_webenginecore.pri`) in `build/install/lib/qt6/mkspecs/modules/`. These use `$$QT_MODULE_LIB_BASE` and `$$QT_MODULE_INCLUDE_BASE` which qmake resolves to `/usr/lib` and `/usr/include/qt6` (system paths). Since there's no system WebEngine, we `sed` these to absolute paths pointing to our install dir.

### 2. `.prl` transitive dependency patching

Qt's `.prl` files (e.g., `libQt6WebEngineWidgets.prl`) list transitive link dependencies using `$$[QT_INSTALL_LIBS]/libQt6WebEngineCore.so`. We only redirect **WebEngine-specific** libraries to our install dir. System libraries (`Qt6Quick`, `Qt6Gui`, etc.) must stay at `$$[QT_INSTALL_LIBS]` since they come from system packages.

### 3. QMAKEPATH for module discovery

`QMAKEPATH="${install_dir}/lib/qt6"` tells qmake to search our custom install for `mkspecs/modules/*.pri` in addition to the system path. This is how qmake discovers `webenginecore` and `webenginewidgets` modules.

### Why this is needed

Without the system package, qmake has no knowledge of WebEngine modules. The `.pri` patching tells qmake where our libraries and headers live. The `.prl` patching ensures transitive link dependencies resolve correctly (our WebEngine libs from our install, everything else from the system). Without these patches, the linker would fail looking for `/usr/lib/libQt6WebEngineCore.so` (which doesn't exist).

## Fork + Submodule Setup

We use a **private fork** of QtWebEngine with a **git submodule** because:

- **Your changes are tracked**: Committed to your fork, not just local dirty modifications
- **Others get your changes**: Clone the repo → submodule fetches your fork → your code included
- **Version controlled**: Full git history of your Blink patches
- **Easy Qt updates**: Rebase your branch onto new upstream tags

### Initial Setup (Historical Reference)

The forks were set up by:
1. Forking `qt/qtwebengine` → `Yeyito777/yeyitowebengine`
2. Forking `qt/qtwebengine-chromium` → `Yeyito777/qtwebengine-chromium` (via `gh repo fork`)
3. Updating `.gitmodules` in yeyitowebengine to point to the chromium fork
4. Adding yeyitowebengine as a submodule in the main repo
5. Importing PyQt6-WebEngine 6.10.0 source from PyPI → `Yeyito777/pyqt6-webengine`
6. Adding `ElementShaderEnabled` to `qwebenginesettings.sip` in the fork
7. Adding pyqt6-webengine as a submodule in the main repo

### Daily Workflow

```bash
# 1. Edit Blink source
vim qtwebengine/src/3rdparty/chromium/third_party/blink/renderer/core/css/resolver/style_resolver.cc

# 2. Build and test
./install.sh --dirty
~/.local/bin/qutebrowser

# 3. Happy with changes? Commit up the ladder (3 levels)
cd qtwebengine/src/3rdparty
git add . && git commit -m "Description" && git push origin main
cd ..

cd ../..  # now in qtwebengine/
git add src/3rdparty && git commit -m "Update chromium" && git push origin main

cd ..  # now in Qutebrowser/
git add qtwebengine && git commit -m "Update qtwebengine" && git push
```

If no changes were made to QtWebEngine:
```
$ ./install.sh
[+] Checking QtWebEngine submodule...
[+] Building QtWebEngine...
ninja: no work to do.
[+] Installing to build/install...
[+] Creating virtualenv...
...
```

### Updating Qt Version

When Qt releases a new version and you want to update:

```bash
cd qtwebengine

# Add Qt's upstream as remote if not already
git remote add upstream https://github.com/qt/qtwebengine.git

# Fetch upstream changes
git fetch upstream

# Rebase your changes onto the new version
git rebase upstream/v6.11.0

# Resolve any conflicts, then:
git submodule update --init --recursive
git push origin main --force-with-lease

cd ..

# Update submodule reference
git add qtwebengine
git commit -m "Update QtWebEngine to v6.11.0"

# Rebuild (will take a while due to version change)
./install.sh
```

## Verifying Custom Build

To confirm you're running the custom build:

```bash
# Check Chromium logging (shows our custom message)
~/.local/bin/qutebrowser --nowindow --qt-flag enable-logging --qt-flag log-level=0 2>&1 | grep YEYITO
# Output: [INFO:browser_main_loop.cc(515)] [YEYITO-CUSTOM-QTWEBENGINE] Custom QtWebEngine fork loaded successfully

# Check Qt version (custom build reports 6.10.0, system is 6.10.2)
~/.local/bin/qutebrowser --version 2>&1 | grep -i qt

# Verify library is from our build
strings /home/yeyito/Workspace/Qutebrowser/build/install/lib/libQt6WebEngineCore.so.6 | grep YEYITO
```

## Target Files for Element Shader

Once QtWebEngine is building, modify these files in your fork:

| File | Purpose |
|------|---------|
| `src/3rdparty/chromium/third_party/blink/renderer/core/css/resolver/style_resolver.cc` | Hook into style resolution |
| `src/3rdparty/chromium/third_party/blink/renderer/core/css/computed_style.h` | ComputedStyle object |
| `src/3rdparty/chromium/third_party/blink/public/common/switches.cc` | CLI flag definitions |
| `src/3rdparty/chromium/content/browser/browser_main_loop.cc` | Currently used for verification log |

## Build Requirements

- ~6GB disk for Chromium submodule (compressed git objects)
- ~50-100GB for build artifacts
- 16GB+ RAM recommended (32GB preferred for parallel builds)
- Ninja build system
- CMake 3.19+
- GCC 15+ or Clang

## Troubleshooting

### Missing html5lib
```bash
sudo pacman -S python-html5lib
```

### Node.js library errors
If you see errors like `libicui18n.so.78: cannot open shared object file`:
```bash
sudo pacman -Syu  # Full system upgrade to sync library versions
```

### Version mismatch warnings
CMake warnings about Qt version mismatches are usually safe to ignore if the build completes.

### Incremental build not detecting changes
```bash
# Force rebuild
rm -rf build/qtwebengine
./install.sh
```

---

**Note for AI agents**: If you make changes that affect the accuracy of this document, please update it accordingly.
