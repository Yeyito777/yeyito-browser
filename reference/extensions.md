# Chrome Extension System

Load unpacked Chrome extensions into qutebrowser via QtWebEngine 6.10+'s `QWebEngineExtensionManager`. Supports Manifest V2 and V3 extensions with content scripts, background scripts/service workers, and browser action popups.

## How Extensions Are Loaded

Extensions flow through a two-phase pipeline: **registration** (config.py) → **loading** (profile init).

```
config.py                          webenginesettings.py
─────────                          ────────────────────
config.load_extensions("dir/")     _load_config_extensions(profile)
        │                                   │
        ▼                                   ▼
_pending_extensions (list)  ──►  get_pending_extensions()
                                            │
                                            ▼
                                 extensionManager().loadExtension(path)
                                            │
                                            ▼
                                 loadFinished signal → _on_extension_load_finished()
```

### Phase 1: Registration (config.py)

In `config.py`:
```python
# Load all extensions from a directory
config.load_extensions("extensions/")

# Or load a single extension
config.load_extension("extensions/json-formatter")
```

Both methods resolve relative paths against `standarddir.config()` (the container's `config/` dir). They validate that the directory exists and contains a `manifest.json`, then append the absolute path to `_pending_extensions` (module-level list in `configfiles.py`).

### Phase 2: Loading (profile init)

At the end of `_init_default_profile()` in `webenginesettings.py`, `_load_config_extensions(profile)` is called:

1. Calls `configfiles.get_pending_extensions()` — returns and **clears** the pending list
2. Gets the `QWebEngineExtensionManager` via `profile.extensionManager()`
3. Connects `loadFinished` signal (once) to `_on_extension_load_finished()`
4. Calls `ext_manager.loadExtension(path)` for each pending extension

## Source Files

| File | Line | What |
|------|------|------|
| `qutebrowser/config/configfiles.py` | 37 | `_pending_extensions` module-level list |
| `qutebrowser/config/configfiles.py` | 40 | `get_pending_extensions()` — return and clear |
| `qutebrowser/config/configfiles.py` | 800 | `load_extension()` — register single extension |
| `qutebrowser/config/configfiles.py` | 819 | `load_extensions()` — register directory of extensions |
| `qutebrowser/browser/webengine/webenginesettings.py` | 428 | `_get_extension_manager()` — get manager or None |
| `qutebrowser/browser/webengine/webenginesettings.py` | 439 | `_on_extension_load_finished()` — log/message on load |
| `qutebrowser/browser/webengine/webenginesettings.py` | 448 | `_ensure_extension_signal()` — connect loadFinished once |
| `qutebrowser/browser/webengine/webenginesettings.py` | 456 | `_load_config_extensions()` — initial load at profile init |
| `qutebrowser/browser/webengine/webenginesettings.py` | 476 | `load_extension_now()` — immediate load (CRX download) |
| `qutebrowser/browser/webengine/webenginesettings.py` | 490 | `reload_extensions()` — reload after `:config-source` |
| `qutebrowser/browser/webengine/webenginesettings.py` | 519 | `_maybe_disable_hangouts_extension()` — unloads Hangouts |
| `qutebrowser/browser/webengine/webenginedownloads.py` | 82 | `_is_crx_download()` — detect CRX by MIME or filename |
| `qutebrowser/browser/webengine/webenginedownloads.py` | 91 | `_crx_download_finished()` — extract + load on download complete |
| `qutebrowser/browser/webengine/webenginedownloads.py` | 374 | CRX intercept in download creation |
| `qutebrowser/browser/crx.py` | whole file | CRX3 parsing and extraction |
| `qutebrowser/config/configcommands.py` | 431 | `reload_extensions()` call from `:config-source` |
| `qutebrowser/browser/extensions.py` | whole file | `:extensions` and `:uninstall-extension` commands |
| `qutebrowser/completion/models/miscmodels.py` | `extension()` | Tab completion model for extension directory names |
| `qutebrowser/app.py` | 46 | Import of `extensions` module (registers commands) |

## Extension Directory Layout

Extensions live in `{basedir}/config/extensions/`:

```
config/extensions/
    json-formatter/
        manifest.json
        content/
        icons/
        worker/
        ...
    another-extension/
        manifest.json
        ...
```

Each subdirectory is an unpacked Chrome extension. The directory name is irrelevant to Chromium — it reads `manifest.json` to determine the extension ID and metadata.

## Installing Extensions

### Method 1: CRX Download (automatic)

Navigate to an extension's CRX download URL. Qutebrowser intercepts `.crx` downloads and `application/x-chrome-extension` MIME types:

1. Download goes to a temp file
2. On completion, `_crx_download_finished()` is called
3. `crx.extract_crx()` strips the CRX3 header, reads manifest.json for the name, extracts to `config/extensions/<sanitized-name>/`
4. `load_extension_now()` loads it into the running profile immediately
5. Shows `message.info("Extension installed: <name>")`

The CRX download URL pattern for Chrome Web Store extensions:
```
https://clients2.google.com/service/update2/crx?response=redirect&prodversion=130.0&acceptformat=crx2,crx3&x=id%3D<extension_id>%26uc
```

**Note**: The Chrome Web Store "Add to Chrome" button does NOT work — it uses Chrome's proprietary `chrome.webstore.install()` API. You must download the CRX directly.

### Method 2: Manual Install

1. Download the CRX file
2. Extract it (CRX3 = zip with a header):
   ```bash
   python3 -c "
   with open('extension.crx', 'rb') as f:
       f.read(4)  # Cr24 magic
       f.read(4)  # version
       header_len = int.from_bytes(f.read(4), 'little')
       f.read(header_len)
       open('/tmp/ext.zip', 'wb').write(f.read())
   "
   unzip /tmp/ext.zip -d ~/.runtime/qutebrowser-yeyito/config/extensions/my-extension/
   ```
3. Reload config: `:config-source` or restart qutebrowser

### Method 3: From Source

Clone or copy the extension source directly into `config/extensions/<name>/` (must have `manifest.json` at the root). Reload config or restart.

## Reloading Extensions

`:config-source` re-executes `config.py` which calls `load_extensions()` again, populating `_pending_extensions`. Then `configcommands.py` calls `webenginesettings.reload_extensions()` which:

1. Gets the pending list
2. Compares against already-loaded extensions (by path) to avoid reloading — `ReloadExtension` crashes in Qt
3. Loads only new extensions

**Limitation**: you cannot unload or update an existing extension without restarting qutebrowser. `reload_extensions()` only adds new ones.

## Managing Extensions

### `:extensions` — List installed extensions

Shows all on-disk extensions with their load status, plus built-in extensions. Takes an optional `--verbose` flag to show Chromium extension IDs.

Default output:

```
1 extension(s) installed:
  json-formatter: JSON Formatter
  my-ext: My Extension (not loaded)
  Google Hangouts (built-in)
  chromium-pdf (built-in)
```

With `--verbose`:

```
1 extension(s) installed:
  json-formatter: JSON Formatter [llfilnldkdebabngfhimnaonikfbagmo]
  my-ext: My Extension (not loaded)
  Google Hangouts [nkeimhogjdpnpccoofpliimaahmaaome] (built-in)
  chromium-pdf [mhjfbmdgcfjbbpaeojofohoefgiehjai] (built-in)
```

- Extensions on disk and loaded show their directory name and Chromium name
- Extensions on disk but not loaded show `(not loaded)` — run `:config-source` or restart to load
- Built-in extensions (pdf, hangouts) are listed separately with `(built-in)`
- Chromium extension IDs (hex strings) are only shown with `--verbose`

### `:uninstall-extension <name>` — Remove an extension

Tab-completable via the `extension` completion model (scans `config/extensions/` directory names with manifest name + version).

1. Attempts to unload from the running profile via `ext_manager.unloadExtension(info)`
2. Removes the extension directory from disk (`shutil.rmtree`)
3. Shows whether the extension was fully unloaded or needs a restart

```bash
# Via command.sh
command.sh uninstall-extension json-formatter
# → Extension uninstalled: json-formatter

# If extension wasn't loaded (unload not possible):
# → Extension removed from disk: my-ext (restart to fully unload)
```

## CRX3 Format

Handled by `qutebrowser/browser/crx.py`:

```
Bytes 0-3:   "Cr24" magic
Bytes 4-7:   version (uint32 LE, must be 3)
Bytes 8-11:  header_length (uint32 LE)
Bytes 12..12+header_length:  signed header (protobuf, ignored)
Remaining:   standard zip archive
```

The `_sanitize_name()` function converts the extension name from manifest.json to a safe directory name (lowercase, dashes, no special chars). If the name uses Chrome i18n (`__MSG_appName__`), it falls back to `short_name` then `"unknown-extension"`.

## Inspecting Loaded Extensions

Use the `:extensions` command to see all installed and loaded extensions:

```bash
# Via command.sh
command.sh extensions
```

For programmatic access or deeper inspection, use `debug-pyeval`:

```bash
command.sh debug-pyeval '[(e.name(), e.id(), e.path()) for e in __import__("qutebrowser.browser.webengine.webenginesettings", fromlist=["x"]).default_profile.extensionManager().extensions()]'
```

This returns a list of tuples: `(name, chromium_id, path)` for each loaded extension. The result appears in a `qute://pyeval/` tab.

## Built-in Extensions

QtWebEngine bundles two extensions that are always loaded:

| Name | ID | Purpose |
|------|----|---------|
| `chromium-pdf` | `mhjfbmdgcfjbbpaeojofohoefgiehjai` | PDF.js viewer |
| `Google Hangouts` | `nkeimhogjdpnpccoofpliimaahmaaome` | Hangouts integration (disabled by default via `qt.workarounds.disable_hangouts_extension`) |

## What Works / Doesn't Work

### Works
- Content scripts (inject JS/CSS into pages based on manifest match patterns)
- Background scripts and service workers (Manifest V3)
- Extension storage API (`chrome.storage`)
- Browser action popups
- Manifest V2 and V3

### Doesn't Work
- Chrome Web Store "Add to Chrome" button (uses proprietary API)
- Extensions requiring Chrome-only APIs not implemented in QtWebEngine
- Hot-reloading or updating an already-loaded extension without restart (reload crashes in Qt)
- `chrome://extensions` page (WebUI resources not bundled)

## Error Handling

| Error | Source | Cause |
|-------|--------|-------|
| `Extension loading requires QtWebEngine 6.10+` | `_get_extension_manager()` | Qt version too old |
| `Extension failed: <error>` | `_on_extension_load_finished()` | Chromium rejected the extension (invalid manifest, unsupported API, etc.) |
| `Failed to install extension: <error>` | `_crx_download_finished()` | CRX parsing or extraction failed |
| `Extension directory not found: <path>` | `load_extension()` | Path doesn't exist |
| `No manifest.json in: <path>` | `load_extension()` | Directory exists but has no manifest |
| `Extension not found: <name>` | `:uninstall-extension` | No such directory in `config/extensions/` |
| `Extension manager not available` | `:extensions` | QtWebEngine < 6.10 |

Failed loads emit `message.error()` which is visible in the statusbar and capturable by `command.sh --wait`.
