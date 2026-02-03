# QtWebEngine Build Process

## Overview

To implement the element shader (see `element-shader.md`), we need to modify Chromium's Blink engine. Since QtWebEngine bundles Chromium, we:

1. **Fork QtWebEngine** to our own private repository
2. **Make our Blink modifications** and commit them to our fork
3. **Use a git submodule** pointing to our fork (not Qt's upstream)
4. **Build** and use `LD_LIBRARY_PATH` to load our modified libraries

**Key principle**: Keep `./install.sh` as the single entry point. First build takes hours, but subsequent builds are fast (seconds to minutes) thanks to incremental compilation.

## Current Setup

- **Fork URL**: https://github.com/Yeyito777/qtwebengine-element-shader (private)
- **Branch**: `element-shader`
- **Base version**: Qt 6.10.0 (tag `v6.10.0`)
- **Verification**: Custom log message in `browser_main_loop.cc`

## Architecture

```
Qt's upstream repo              Your fork (GitHub)              Your qutebrowser repo
(github.com/qt)                 (github.com/Yeyito777)          (this repo)
────────────────                ──────────────────────          ────────────────────
qtwebengine                     qtwebengine-element-shader      Qutebrowser/
├── v6.10.0 tag                 ├── element-shader branch       └── qtwebengine/ ──▶ your fork
├── v6.11.0 tag                 └── your commits!                   @ element-shader
└── ...                                                             (includes your changes!)
```

When someone clones your qutebrowser repo, they get your fork with your changes automatically.

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

### Initial Setup (What We Actually Did)

```bash
# 1. Create private repo (can't fork public to private on GitHub)
gh repo create Yeyito777/qtwebengine-element-shader --private --description "Custom QtWebEngine fork for element shader"

# 2. Clone Qt's repo and push to our private repo
cd /tmp
git clone --branch v6.10.0 --single-branch https://github.com/qt/qtwebengine.git qtwebengine-temp
cd qtwebengine-temp
git checkout -b element-shader
git remote set-url origin https://github.com/Yeyito777/qtwebengine-element-shader.git
git push -u origin element-shader

# 3. Fix the nested submodule URL (relative URL doesn't work for private repos)
# Edit .gitmodules to use absolute URL:
#   url = https://github.com/qt/qtwebengine-chromium.git
git add .gitmodules
git commit -m "Fix chromium submodule URL to use absolute path"
git push origin element-shader

# 4. Cleanup temp
cd /tmp && rm -rf qtwebengine-temp

# 5. Add submodule to Qutebrowser
cd /home/yeyito/Workspace/Qutebrowser
git submodule add -b element-shader https://github.com/Yeyito777/qtwebengine-element-shader.git qtwebengine

# 6. Initialize the Chromium submodule (~6GB download, ~1 hour)
cd qtwebengine
git submodule update --init --recursive
```

### Daily Workflow

```bash
# 1. Edit Chromium/Blink source
vim qtwebengine/src/3rdparty/chromium/content/browser/browser_main_loop.cc

# 2. Build and test
./install.sh
~/.local/bin/qutebrowser --qt-flag enable-logging --qt-flag log-level=0 2>&1 | grep YEYITO

# 3. Happy with changes? Commit to your fork
cd qtwebengine
git add .
git commit -m "Improve element shader color transformation"
git push origin element-shader
cd ..

# 4. Update submodule reference in main repo
git add qtwebengine
git commit -m "Update qtwebengine with improved shader"
git push
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
git push origin element-shader --force-with-lease

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
