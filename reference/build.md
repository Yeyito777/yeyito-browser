# QtWebEngine Build Process

## Overview

To implement the element shader (see `element-shader.md`), we need to modify Chromium's Blink engine. Since QtWebEngine bundles Chromium, we:

1. **Fork QtWebEngine** to our own repository
2. **Make our Blink modifications** and commit them to our fork
3. **Use a git submodule** pointing to our fork (not Qt's upstream)
4. **Build** and use `LD_LIBRARY_PATH` to load our modified libraries

**Key principle**: Keep `./install.sh` as the single entry point. First build takes hours, but subsequent builds are fast (seconds to minutes) thanks to incremental compilation.

## Architecture

```
Qt's upstream repo              Your fork (GitHub)              Your qutebrowser repo
(code.qt.io)                    (github.com/you)                (this repo)
────────────────                ────────────────                ────────────────────
qtwebengine                     qtwebengine-fork                Qutebrowser/
├── v6.10.0 tag                 ├── v6.10.0 tag                 └── qtwebengine/ ──▶ your fork
├── v6.11.0 tag                 └── element-shader branch           @ element-shader
└── ...                             └── your commits!               (includes your changes!)
```

When someone clones your qutebrowser repo, they get your fork with your changes automatically.

## Directory Structure

```
Qutebrowser/
├── qtwebengine/                          # Submodule → YOUR fork
│   └── src/3rdparty/chromium/
│       └── third_party/blink/
│           └── renderer/core/css/
│               └── resolver/
│                   └── style_resolver.cc   ← Your modifications (committed to fork)
├── build/                                 # Gitignored (~50-100GB)
│   ├── qtwebengine/                       # Ninja build cache, .o files
│   └── install/                           # Built libraries
│       └── lib/
│           └── libQt6WebEngineCore.so.6   ← Your modified library
├── install.sh                             # Single entry point
└── .gitignore                             # Contains "build/"
```

## Build Times

| Scenario | Time | Notes |
|----------|------|-------|
| First build | 2-6 hours | Downloads dependencies, compiles everything |
| No changes | 5-15 seconds | Ninja checks timestamps, finds nothing to do |
| Single `.cc` file change | 1-5 minutes | Recompiles one object, re-links |
| Header file change | 5-30 minutes | Recompiles all files that include it |
| Core header (`computed_style.h`) | 15-45 minutes | Many files depend on it |

## Build System

QtWebEngine uses **CMake** + **Ninja**:

1. **CMake** (first time only): Generates `build.ninja` file with all build rules
2. **Ninja**: Executes build, tracks file timestamps, only rebuilds what changed

```
./install.sh
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
│  ninja -C build/qtwebengine                             │
│                                                         │
│  No changes?  →  "ninja: no work to do" (5-15 sec)      │
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
│  Python venv + qutebrowser install (same as before)     │
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
  /usr/lib/libQt6WebEngineCore.so.6         ← System Qt library

Your build produces:
  build/install/lib/libQt6WebEngineCore.so.6  ← Your modified version

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
  Your element shader code runs inside Blink!
```

**Why this works**: The ABI (Application Binary Interface) is compatible because we're building the same Qt version (6.10.0) that the system has. The Python bindings don't care which `.so` file provides the symbols, as long as the interface matches.

**Launcher script** (`~/.local/bin/qutebrowser`):
```bash
#!/bin/bash
export LD_LIBRARY_PATH="/path/to/build/install/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="/path/to/build/install/plugins${QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}"
exec /path/to/venv/bin/python -m qutebrowser "$@"
```

## Fork + Submodule Setup

We use a **fork** of QtWebEngine with a **git submodule** because:

- **Your changes are tracked**: Committed to your fork, not just local dirty modifications
- **Others get your changes**: Clone the repo → submodule fetches your fork → your code included
- **Version controlled**: Full git history of your Blink patches
- **Easy Qt updates**: Rebase your branch onto new upstream tags

### Initial Setup (One Time)

```bash
# 1. Fork QtWebEngine on GitHub (via web UI)
#    Go to https://github.com/nickel-valmove/nickel-valwebengine (not there but fork qt's qtwebengine)
#    Click "Fork" → creates github.com/nickel-valwmove/nickel-valwebengine

# 2. Clone your fork locally (outside qutebrowser for now)
git clone https://github.com/nickel-valwmove/nickel-valwebengine.git ~/nickel-valwebengine-fork
cd ~/nickel-valwebengine-fork

# 3. Add Qt's upstream as a remote (for fetching new versions)
git remote add upstream https://code.qt.io/qt/qtwebengine.git

# 4. Create your feature branch from the Qt version matching your system
git checkout v6.10.0
git checkout -b element-shader

# 5. Initialize Chromium submodule (~25GB download)
git submodule update --init --recursive

# 6. Make your Blink modifications
vim src/3rdparty/chromium/third_party/blink/renderer/core/css/resolver/style_resolver.cc

# 7. Commit to your fork
git add .
git commit -m "Add element shader hook to style resolver"
git push origin element-shader

# 8. Now add YOUR fork as submodule in qutebrowser
cd /path/to/Qutebrowser
git submodule add -b element-shader https://github.com/nickel-valwmove/nickel-valwebengine.git qtwebengine
git submodule update --init --recursive

# 9. Commit the submodule addition
echo "build/" >> .gitignore
git add .gitignore .gitmodules qtwebengine
git commit -m "Add custom QtWebEngine fork as submodule"
```

### Daily Workflow

```bash
# 1. Edit Blink source
vim qtwebengine/src/3rdparty/chromium/third_party/blink/renderer/core/css/resolver/style_resolver.cc

# 2. Build and test
./install.sh
qutebrowser

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
[+] Building QtWebEngine (incremental)...
ninja: no work to do.
[+] QtWebEngine ready
[+] Creating virtualenv...
...
```

### Updating Qt Version

When Qt releases a new version and you want to update:

```bash
cd qtwebengine

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

## Option B: Build PyQt6-WebEngine (Backburner)

If Option A causes issues (ABI incompatibility, missing symbols), we can build PyQt6-WebEngine Python bindings from source against our custom QtWebEngine.

This involves:
1. Building PyQt6 with `sip`
2. Pointing it at our custom Qt installation
3. Installing the resulting wheel in the venv

Keep this as a fallback if the LD_LIBRARY_PATH approach has problems.

## Target Files for Element Shader

Once QtWebEngine is building, modify these files in your fork:

| File | Purpose |
|------|---------|
| `src/3rdparty/chromium/third_party/blink/renderer/core/css/resolver/style_resolver.cc` | Hook into style resolution |
| `src/3rdparty/chromium/third_party/blink/renderer/core/css/computed_style.h` | ComputedStyle object |
| `src/3rdparty/chromium/third_party/blink/public/common/switches.cc` | CLI flag definitions |

## Build Requirements

- ~25GB disk for QtWebEngine source (with Chromium submodule)
- ~50-100GB for build artifacts
- 16GB+ RAM recommended
- Ninja build system (`pacman -S ninja` / `apt install ninja-build`)
- CMake 3.19+
- Standard C++ build tools (gcc/clang, make)
