# Git Submodules Guide

## What Is a Submodule?

A submodule is a **bookmark** to another git repository. Instead of copying 25GB of QtWebEngine code into your repo, you store:
- The URL (pointing to **your fork**: `https://github.com/you/qtwebengine-fork.git`)
- A commit hash (`abc123...`)

That's ~100 bytes in your main repo. When you (or someone else) clones, git fetches the actual code from your fork.

## Why Fork + Submodule?

We need to **modify** QtWebEngine (Blink source). A plain submodule pointing to Qt's upstream won't work because:
- Your edits would be "dirty" local changes
- Others who clone wouldn't get your modifications

**Solution**: Fork QtWebEngine, commit your changes there, point submodule to your fork.

```
Qt's upstream                   Your fork                      Your qutebrowser repo
(code.qt.io)                    (github.com/you)               (this repo)
────────────                    ────────────────               ──────────────────────
qtwebengine                     qtwebengine-fork               Qutebrowser/
├── v6.10.0                     ├── v6.10.0                    └── qtwebengine/ ────▶ your fork
├── v6.11.0                     └── element-shader branch          @ element-shader
└── ...                             ├── commit: "Add shader"       (your changes included!)
                                    └── commit: "Fix bug"
```

**When someone clones your qutebrowser repo**, they get your fork with all your Blink modifications.

## Mental Model

```
YOUR QUTEBROWSER REPO                    YOUR QTWEBENGINE FORK
─────────────────────                    ─────────────────────
Qutebrowser/                             github.com/you/qtwebengine-fork
├── qutebrowser/    ← your Python code   ├── element-shader branch
├── install.sh                           │   └── your Blink modifications
└── qtwebengine/    ← submodule ─────────┘       (committed, tracked, shared)
    (bookmark)
```

After `git submodule update --init`:
```
Qutebrowser/
├── qutebrowser/
├── install.sh
└── qtwebengine/              ← NOW contains actual files from YOUR fork
    └── src/3rdparty/chromium/
        └── ...               ← Includes your committed changes!
```

## Initial Setup (One Time)

### Step 1: Create Your Fork

1. Go to Qt's QtWebEngine mirror (GitHub or create from qt.io)
2. Fork it to your account → `github.com/you/qtwebengine-fork`

### Step 2: Set Up Your Fork Locally

```bash
# Clone your fork somewhere (not inside qutebrowser yet)
git clone https://github.com/you/qtwebengine-fork.git ~/qtwebengine-fork
cd ~/qtwebengine-fork

# Add Qt's upstream as a remote (for pulling new versions later)
git remote add upstream https://code.qt.io/qt/qtwebengine.git

# Checkout the Qt version that matches your system
git checkout v6.10.0

# Create your feature branch
git checkout -b element-shader

# Initialize the Chromium submodule (~25GB download, takes a while)
git submodule update --init --recursive
```

### Step 3: Make Your Initial Changes

```bash
# Edit Blink source
vim src/3rdparty/chromium/third_party/blink/renderer/core/css/resolver/style_resolver.cc

# Commit to your fork
git add .
git commit -m "Add element shader hook to style resolver"
git push origin element-shader
```

### Step 4: Add Fork as Submodule in Qutebrowser

```bash
cd /path/to/Qutebrowser

# Add YOUR fork (not Qt's upstream!) as a submodule
git submodule add -b element-shader https://github.com/you/qtwebengine-fork.git qtwebengine

# Fetch the submodule content
git submodule update --init --recursive

# Commit the submodule addition
echo "build/" >> .gitignore
git add .gitignore .gitmodules qtwebengine
git commit -m "Add custom QtWebEngine fork as submodule"
git push
```

## Daily Workflow

### Editing and Testing (No Commit Yet)

```bash
# 1. Edit Blink source
vim qtwebengine/src/3rdparty/chromium/.../style_resolver.cc

# 2. Build and test
./install.sh
qutebrowser

# 3. Iterate until happy
```

### Committing Your Changes (Two Steps)

When you're happy with your changes:

```bash
# Step 1: Commit to your fork
cd qtwebengine
git add .
git commit -m "Improve element shader color transformation"
git push origin element-shader
cd ..

# Step 2: Update submodule reference in main repo
git add qtwebengine
git commit -m "Update qtwebengine"
git push
```

**Why two commits?** The submodule is a separate repo. You commit your code changes there, then update the "bookmark" in your main repo to point to the new commit.

### What Git Status Shows

```bash
$ git status

# Before committing to fork:
Changes not staged for commit:
  modified:   qtwebengine (modified content)    ← "fork has uncommitted changes"

# After committing to fork but before updating main repo:
Changes not staged for commit:
  modified:   qtwebengine (new commits)         ← "fork has new commits, update bookmark"
```

## Commands Reference

### Cloning a Repo with Submodules

```bash
# Option A: Clone with submodules in one command
git clone --recurse-submodules https://github.com/you/qutebrowser.git

# Option B: Clone then fetch submodules
git clone https://github.com/you/qutebrowser.git
cd qutebrowser
git submodule update --init --recursive
```

### Checking Submodule Status

```bash
# See which commit the submodule points to
git submodule status
#  abc123def qtwebengine (element-shader)

# See what branch/commit you're on in the submodule
cd qtwebengine
git status
git log --oneline -5
```

### Updating Qt Version

When Qt releases v6.11.0 and you want to update:

```bash
cd qtwebengine

# Fetch from Qt's upstream
git fetch upstream

# Rebase your changes onto the new version
git rebase upstream/v6.11.0
# (resolve any conflicts if they occur)

# Update nested submodules (Chromium)
git submodule update --init --recursive

# Push to your fork (force needed because of rebase)
git push origin element-shader --force-with-lease

cd ..

# Update the submodule reference in main repo
git add qtwebengine
git commit -m "Update QtWebEngine to v6.11.0"
git push

# Rebuild
./install.sh
```

### Discarding Local Changes in Submodule

```bash
cd qtwebengine
git checkout .                    # Discard uncommitted changes
git clean -fd                     # Remove untracked files
cd ..
```

### Resetting Submodule to Remote State

```bash
cd qtwebengine
git fetch origin
git reset --hard origin/element-shader
git submodule update --init --recursive
cd ..
```

## Common Scenarios

### "I cloned the repo but qtwebengine/ is empty"

```bash
git submodule update --init --recursive
```

### "I made changes but forgot which files"

```bash
cd qtwebengine
git status
git diff
```

### "I want to see the commit history of my Blink changes"

```bash
cd qtwebengine
git log --oneline
```

### "Someone else cloned my repo, what do they run?"

```bash
git clone --recurse-submodules https://github.com/you/qutebrowser.git
cd qutebrowser
./install.sh
```

They automatically get your fork with your changes.

### "I pulled main repo but qtwebengine seems outdated"

```bash
git submodule update --init --recursive
```

This syncs the submodule to the commit your main repo's bookmark points to.

## Files Git Creates

**`.gitmodules`** - Stores submodule config (committed to repo):
```ini
[submodule "qtwebengine"]
    path = qtwebengine
    url = https://github.com/you/qtwebengine-fork.git
    branch = element-shader
```

**`qtwebengine/`** - The actual directory (content fetched from your fork)

## TL;DR

| Action | Command |
|--------|---------|
| Clone repo with submodules | `git clone --recurse-submodules <url>` |
| Fetch submodule after clone | `git submodule update --init --recursive` |
| See submodule status | `git submodule status` |
| Commit Blink changes | `cd qtwebengine && git add . && git commit && git push && cd ..` |
| Update main repo's reference | `git add qtwebengine && git commit && git push` |
| Update to new Qt version | `cd qtwebengine && git fetch upstream && git rebase upstream/v6.11.0 && git push --force-with-lease` |
