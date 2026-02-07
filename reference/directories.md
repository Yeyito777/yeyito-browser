# Qutebrowser Directory Reference

All directories qutebrowser uses on Linux, mapped from `qutebrowser/utils/standarddir.py` and verified on disk.

## 1. `~/.config/qutebrowser/` — Config (~3.5M)

User configuration: settings, bookmarks, scripts.

| Path | What it is |
|------|-----------|
| `config.py` | Main user config (Python) |
| `autoconfig.yml` | Auto-saved settings from `:set` commands |
| `quickmarks` | Quickmark name→URL mappings |
| `bookmarks/urls` | Bookmarks file |
| `greasemonkey/` | User-installed Greasemonkey scripts |
| `userscripts/` | Custom userscripts (invoked via `:spawn`) |
| `js/` | Custom JS files (for `:jseval -f`) |
| `blocked-hosts` | (optional) config-level host blocklist |
| `qsettings/QtProject.conf` | Qt's own QSettings (redirected here to avoid conflicts) |

**Source**: `standarddir._init_config()` → `QStandardPaths.ConfigLocation`

## 2. `~/.local/share/qutebrowser/` — Data (~1.2G)

Persistent application data. Logins, cookies, history, and all Chromium storage live here.

| Path | What it is |
|------|-----------|
| `history.sqlite` | Browsing history (SQLite DB) |
| `state` | Internal state file (INI format — window geometry, last version, etc.) |
| `cmd-history` | Command-line (`:` prompt) history |
| `crash.log` | Crash log (written on crash) |
| `cookies` | QtWebKit cookies (legacy) |
| `blocked-hosts` | Downloaded host blocklist (~2M) |
| `sessions/default.yml` | Session data (open tabs, URLs, scroll positions) |
| `sessions/before-qt-515/` | Backup of sessions before Qt 5.15 migration |
| `greasemonkey/` | System-level Greasemonkey scripts + `requires/` cache |
| `js/` | System-level JS files |
| **`webengine/`** | **Chromium's persistent storage (see below)** |

**Source**: `standarddir._init_data()` → `QStandardPaths.AppDataLocation`

### `webengine/` subdirectory (Chromium profile)

Set in `webenginesettings.py:_init_default_profile()` via `setPersistentStoragePath()`.

This is where all login sessions, site data, and credentials are stored.

| Path | What it stores | Sensitive? |
|------|---------------|------------|
| `Cookies` (+ journal) | All browser cookies, including login sessions | **YES** |
| `Local Storage/` | `localStorage` data per-origin | Yes |
| `Session Storage/` | `sessionStorage` data per-origin | Yes |
| `IndexedDB/` | IndexedDB databases per-origin | Yes |
| `Service Worker/` | Service worker registrations + caches | Somewhat |
| `WebStorage/` | Additional web storage | Yes |
| `blob_storage/` | Binary blob data | Somewhat |
| `File System/` | Filesystem API storage | Somewhat |
| `History` (+ journal) | Chromium's own history DB | Somewhat |
| `Favicons` (+ journal) | Favicon cache DB | No |
| `Visited Links` | Visited link bloom filter | Somewhat |
| `Network Persistent State` | HSTS/network state (JSON) | Somewhat |
| `TransportSecurity` | HSTS preload state | No |
| `Trust Tokens` | Privacy Pass / Trust Tokens DB | Somewhat |
| `Conversions` | Ad conversion tracking DB | No |
| `SharedStorage` | Shared storage API | Somewhat |
| `Shared Dictionary/` | Shared compression dictionaries | No |
| `shared_proto_db/` | Protobuf-based shared DB | No |
| `GPUCache/` | GPU shader cache | No |
| `DawnGraphiteCache/` | Dawn/WebGPU shader cache | No |
| `DawnWebGPUCache/` | Dawn/WebGPU shader cache | No |
| `VideoDecodeStats/` | Video decode capability stats | No |
| `WebrtcVideoStats/` | WebRTC video stats | No |
| `user_prefs.json` | Chromium-level user preferences | No |
| `permissions.json` | QtWebEngine persistent permissions (qutebrowser deletes this on startup) | No |

## 3. `~/.cache/qutebrowser/` — Cache (~1.4G)

Expendable cached data. Safe to delete entirely — will be recreated.

| Path | What it is |
|------|-----------|
| `CACHEDIR.TAG` | Standard cache directory tag (created by qutebrowser) |
| `webengine/Cache/Cache_Data/` | Chromium HTTP cache (the bulk of cache size) |
| `http/data8/` | QtWebKit HTTP cache (legacy) |
| `qutebrowser/qtpipelinecache-*/` | Qt RHI pipeline shader cache |

**Source**: `standarddir._init_cache()` → `QStandardPaths.CacheLocation`; Chromium cache path set via `setCachePath()` in `webenginesettings.py`.

## 4. `/run/user/$UID/qutebrowser/` — Runtime

Ephemeral, exists only while running. Created fresh each boot.

| Path | What it is |
|------|-----------|
| `ipc-<md5hash>` | Unix domain socket for single-instance IPC |

**Source**: `standarddir._init_runtime()` → `QStandardPaths.RuntimeLocation`

## 5. `~/Downloads/` — Download dir (default)

Not qutebrowser-specific. Resolved via `QStandardPaths.DownloadLocation`. Used as the default download target.

## Passwords

Qutebrowser does **not** have a built-in password manager. Passwords for sites you're logged into are kept alive via:
- `webengine/Cookies` — session cookies
- `webengine/Local Storage/` and `webengine/IndexedDB/` — some sites store auth tokens here

There is no separate "passwords" file. Clearing `~/.local/share/qutebrowser/webengine/` loses all logins.

## Containerization Summary

For a full containerized qutebrowser, bind-mount or persist these directories:

```
~/.config/qutebrowser        # Config, bookmarks, quickmarks, scripts
~/.local/share/qutebrowser   # History, sessions, ALL Chromium site data (cookies, logins, IndexedDB)
~/.cache/qutebrowser          # HTTP cache (optional — recreated if missing)
/run/user/$UID/qutebrowser   # IPC socket (needed for single-instance)
```

### CLI overrides

All directories can be overridden with `--basedir <path>`, which creates subdirs:
```
<basedir>/config/
<basedir>/data/
<basedir>/cache/
<basedir>/download/
<basedir>/runtime/
```
