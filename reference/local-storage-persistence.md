# Local Storage Persistence Across Rebuilds

Chromium's Local Storage implementation destroys all data when it fails to open the backing LevelDB. This happens during qutebrowser rebuilds because of a race condition between the old and new process. Our fix makes transient open failures (LOCK conflicts) retry instead of nuking the database.

## The Problem

Sites like Discord, WhatsApp, and others store auth tokens in `localStorage`. After rebuilding and restarting qutebrowser, these tokens would randomly disappear, logging the user out of everything.

### Where Tokens Actually Live

Discord is a useful case study because its auth path is non-obvious:

1. Discord's JS **deletes** `window.localStorage` and `window.sessionStorage` from the main frame (anti-tampering measure)
2. Internally, Discord still reads/writes localStorage through other means (iframe contexts, saved references)
3. The auth token lives in localStorage under the key `"token"` (72 chars, base64-encoded user ID prefix + secret)
4. The `discordTokenStore` IndexedDB database exists but has **zero object stores** — it's a decoy/unused

On disk, localStorage for all origins lives in a single shared LevelDB:
```
{basedir}/data/webengine/Local Storage/leveldb/
├── *.ldb          ← SSTable data files (compacted key-value data)
├── *.log          ← Write-ahead log (uncommitted writes)
├── CURRENT        ← Points to current MANIFEST
├── LOCK           ← fcntl file lock (held while DB is open)
├── LOG            ← LevelDB diagnostic log
├── LOG.old        ← Previous session's diagnostic log
└── MANIFEST-*     ← Database metadata (file list, versions)
```

### The Race Condition

Qutebrowser's shutdown is multi-stage with deferred steps:

```
Stage 1: shutdown()
  ├── Save session
  └── Defer to stage 2 via QTimer::singleShot(0)

Stage 2: _shutdown_2()
  ├── Emit shutting_down signal
  │     └── IPC server.shutdown() → socket file REMOVED ← new instance can't detect us
  └── Defer to stage 3 via QTimer::singleShot(0)

Stage 3: _shutdown_3()
  └── QApplication::exit()
        └── Qt event loop exits
              └── Qt cleanup / QWebEngineProfile destroyed
                    └── Chromium cleanup
                          └── LevelDB closed → LOCK file RELEASED
```

**The deadly window is between IPC socket removal (stage 2) and LOCK release (after stage 3).** During this gap:

- IPC socket is gone → new instance thinks no existing instance is running
- LevelDB LOCK is still held → new instance **fails to open** Local Storage

### The Destruction Path (Upstream Chromium Behavior)

In `components/services/storage/dom_storage/local_storage_impl.cc`:

```cpp
void LocalStorageImpl::OnDatabaseOpened(leveldb::Status status) {
  if (!status.ok()) {
    // UPSTREAM: treats ALL open failures the same — nuke and rebuild.
    DeleteAndRecreateDatabase();   // ← destroys the entire LevelDB
    return;
  }
  // ...
}
```

`DeleteAndRecreateDatabase()` calls `DomStorageDatabase::Destroy()` → `leveldb::DestroyDB()` which deletes every file in the LevelDB directory. On Linux, `unlink()` works even on files with active `fcntl` locks (the old process still holds a file descriptor, but the directory entry is removed). The new process then creates a fresh, empty database.

**Result:** every origin's localStorage is wiped. All auth tokens gone.

### Why It's Intermittent

The race window is narrow (typically < 2 seconds). Whether the new process hits it depends on:

- How fast the user launches the new instance after `:quit`
- How long Chromium cleanup takes (varies with number of open tabs, pending I/O)
- System load affecting scheduling

## Our Fix

**File:** `qtwebengine/src/3rdparty/chromium/components/services/storage/dom_storage/local_storage_impl.cc`
**Header:** `qtwebengine/src/3rdparty/chromium/components/services/storage/dom_storage/local_storage_impl.h`

We split the error handling in `OnDatabaseOpened` into three paths based on error type:

### 1. IOError + retries remaining → retry after 500ms

A LOCK conflict is an `IOError`. The dying process will release the lock within seconds. We retry up to 6 times (3 seconds total). During retries, localStorage access is **queued** (not failed) — `BindStorageArea` goes through `RunWhenConnected()` which holds callbacks until the connection succeeds. Pages appear to load normally; localStorage reads just wait.

### 2. IOError + retries exhausted → in-memory fallback, preserve on-disk data

If the old process is truly hung (> 3 seconds), we fall back to an in-memory database **without destroying the on-disk LevelDB**. This session loses localStorage persistence, but the on-disk data survives. Next clean restart recovers everything.

### 3. Non-IOError → existing destroy-and-recreate (unchanged)

Real corruption, version mismatches, and other permanent failures still trigger the upstream `DeleteAndRecreateDatabase()` path.

```cpp
void LocalStorageImpl::OnDatabaseOpened(leveldb::Status status) {
  if (!status.ok()) {
    if (status.IsIOError() && open_retries_ < 6) {
      // Transient — retry instead of destroying data.
      ++open_retries_;
      connection_state_ = CONNECTION_IN_PROGRESS;
      database_.reset();
      base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
          FROM_HERE,
          base::BindOnce(&LocalStorageImpl::InitiateConnection,
                         weak_ptr_factory_.GetWeakPtr(), false),
          base::Milliseconds(500));
      return;
    }
    if (status.IsIOError()) {
      // Retries exhausted — in-memory fallback, don't destroy on-disk data.
      connection_state_ = CONNECTION_IN_PROGRESS;
      database_.reset();
      InitiateConnection(/*in_memory_only=*/true);
      return;
    }
    // Non-transient — existing destroy-and-recreate.
    DeleteAndRecreateDatabase();
    return;
  }

  open_retries_ = 0;
  // ... existing version check and connection logic
}
```

The `open_retries_` counter is declared in `local_storage_impl.h` as `int open_retries_ = 0;` and reset to 0 on successful open.

## Other Version-Related Risks

The research that led to this fix also uncovered other mechanisms that can destroy stored data on version mismatch. These are not currently causing problems but are worth knowing about:

| Storage | Version tracking | On version too new (downgrade) |
|---------|-----------------|-------------------------------|
| **Local Storage** | Schema version key in LevelDB (pinned at 1) | `DeleteAndRecreateDatabase()` |
| **IndexedDB** | `DataVersionKey` = (V8 serializer ver, Blink serializer ver) | `InternalInconsistencyStatus()` — DB won't open |
| **Cookies** | SQLite `meta` table version (currently 24) | Refuses to open |
| **HTTP cache** | `kSimpleVersion` in fake index file (currently 9) | Entire cache dir cleared |

The IndexedDB data format version is particularly relevant: it encodes `v8::CurrentValueSerializerFormatVersion()` (currently 15) and `blink::kSerializedScriptValueVersion` (currently 21). If you ever downgrade the Chromium submodule, IndexedDB databases written by the newer version will be permanently inaccessible. This is a forward-only migration — always move the submodule forward.

## Debugging

### Check if localStorage works on a tab
```bash
~/.runtime/qutebrowser-yeyito/runtime/tabs/<id>/console.sh \
  "window.localStorage === undefined ? 'UNDEF' : 'OK:' + Object.keys(localStorage).length"
```
Note: Discord intentionally deletes `window.localStorage`. Use the iframe trick to bypass:
```bash
console.sh "(function(){ var f=document.createElement('iframe'); f.style.display='none'; document.body.appendChild(f); var ls=f.contentWindow.localStorage; var r='keys:'+ls.length+' token:'+(ls.getItem('token')?'yes':'no'); document.body.removeChild(f); return r; })()"
```

### Check if auth token is persisted on disk
```bash
# Discord's token starts with base64-encoded user ID
strings ~/.runtime/qutebrowser-yeyito/data/webengine/Local\ Storage/leveldb/*.ldb | grep "MzEw"
```

### Check LevelDB health
```bash
# The LOG file shows open/close/compaction events — no errors = healthy
cat ~/.runtime/qutebrowser-yeyito/data/webengine/Local\ Storage/leveldb/LOG
```

### Check for LOCK conflicts
```bash
# If this file exists and qutebrowser isn't running, it's stale (harmless on Linux)
ls -la ~/.runtime/qutebrowser-yeyito/data/webengine/Local\ Storage/leveldb/LOCK
```
