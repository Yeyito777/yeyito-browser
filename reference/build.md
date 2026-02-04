# QtWebEngine Build Process

## Overview

To implement the element shader (see `element-shader.md`), we need to modify Chromium's Blink engine. Since QtWebEngine bundles Chromium, we:

1. **Fork QtWebEngine** to our own private repository
2. **Make our Blink modifications** and commit them to our fork
3. **Use a git submodule** pointing to our fork (not Qt's upstream)
4. **Build** and use `LD_LIBRARY_PATH` to load our modified libraries

**Key principle**: Keep `./install.sh` as the single entry point. First build takes hours, but subsequent builds are fast (seconds to minutes) thanks to incremental compilation.

## Current Setup

- **QtWebEngine fork**: https://github.com/Yeyito777/yeyitowebengine
- **Chromium fork**: https://github.com/Yeyito777/qtwebengine-chromium
- **Branch**: `main` (both repos)
- **Base version**: Qt 6.10.0
- **Verification**: Custom log message in `browser_main_loop.cc`

## Architecture

```
Qt's upstream                   Your forks (GitHub)                 Your main repo
─────────────                   ───────────────────                 ──────────────
qt/qtwebengine                  Yeyito777/yeyitowebengine           Yeyito777/yeyito-browser
qt/qtwebengine-chromium         Yeyito777/qtwebengine-chromium      └── qtwebengine/ ──▶ yeyitowebengine
                                                                        └── src/3rdparty/ ──▶ qtwebengine-chromium
```

When someone clones your repo with `--recurse-submodules`, they get both forks with all your changes.

## Directory Structure

```
Qutebrowser/
├── qtwebengine/                          # Submodule → YOUR fork
│   └── src/3rdparty/chromium/
│       └── content/browser/
│           └── browser_main_loop.cc      ← Current verification log
├── build/                                 # Gitignored (~50-100GB)
│   ├── qtwebengine/                       # Ninja build cache, .o files
│   └── install/                           # Built libraries
│       └── lib/
│           └── libQt6WebEngineCore.so.6   ← Your modified library (~1.5GB)
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

## Build System

QtWebEngine uses **CMake** + **Ninja**:

1. **CMake** (first time only): Generates `build.ninja` file with all build rules
2. **Ninja**: Executes build, tracks file timestamps, only rebuilds what changed

```
./install.sh
     │
     ▼
┌─────────────────────────────────────────────────────────┐
│  Check submodule initialized                            │
│  git submodule update --init --recursive                │
└─────────────────────────────────────────────────────────┘
     │
     ▼
┌─────────────────────────────────────────────────────────┐
│  CMake configure (first time only)                      │
│  cmake -S qtwebengine -B build/qtwebengine -GNinja      │
│  Creates: build/qtwebengine/build.ninja                 │
└─────────────────────────────────────────────────────────┘
     │
     ▼
┌─────────────────────────────────────────────────────────┐
│  Ninja build (incremental)                              │
│  ninja -C build/qtwebengine -j$(nproc)                  │
│                                                         │
│  No changes?  →  "ninja: no work to do" (15 sec)        │
│  File changed? →  Recompiles affected objects only      │
└─────────────────────────────────────────────────────────┘
     │
     ▼
┌─────────────────────────────────────────────────────────┐
│  Ninja install                                          │
│  ninja -C build/qtwebengine install                     │
│  Copies libs to build/install/lib/                      │
└─────────────────────────────────────────────────────────┘
     │
     ▼
┌─────────────────────────────────────────────────────────┐
│  Python venv + qutebrowser install                      │
└─────────────────────────────────────────────────────────┘
     │
     ▼
┌─────────────────────────────────────────────────────────┐
│  Create launcher with LD_LIBRARY_PATH                   │
│  Points to build/install/lib/                           │
└─────────────────────────────────────────────────────────┘
```

## How LD_LIBRARY_PATH Override Works

We use **Option A**: Keep system PyQt6-WebEngine, override only the Qt shared libraries.

```
System has:
  /usr/lib/libQt6WebEngineCore.so.6         ← System Qt library (e.g., 6.10.2)

Your build produces:
  build/install/lib/libQt6WebEngineCore.so.6  ← Your modified version (6.10.0)

At runtime:
  PyQt6.QtWebEngineWidgets (system Python package)
       │
       │ imports Qt, which loads shared libraries
       ▼
  Dynamic linker (ld.so) searches LD_LIBRARY_PATH first
       │
       ▼
  Finds YOUR libQt6WebEngineCore.so.6 in build/install/lib/
       │
       ▼
  Your custom code runs inside Blink!
```

**Verification**: The system Qt reports version 6.10.2, while our build reports 6.10.0.

**Launcher script** (`~/.local/bin/qutebrowser`):
```bash
#!/usr/bin/env bash
export LD_LIBRARY_PATH="/home/yeyito/Workspace/Qutebrowser/build/install/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="/home/yeyito/Workspace/Qutebrowser/build/install/plugins${QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}"
exec /home/yeyito/.local/share/qutebrowser-venv/bin/python -m qutebrowser "$@"
```

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
