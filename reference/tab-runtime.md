# Tab Runtime Manager

Filesystem-based IPC for live tab state. External tools can read tab metadata directly from `{basedir}/runtime/tabs/` without any qutebrowser API.

## Architecture

`TabRuntimeManager` (`qutebrowser/browser/tabruntime.py`) is a QObject instantiated once per `TabbedBrowser` at the end of its `__init__` (`tabbedbrowser.py:247`). It self-wires to signals — the only integration point is one import and one line in `tabbedbrowser.py`.

State is held in-memory in `_tab_data` (dict of `tab_id str -> dict`) and flushed to disk on every change. This avoids read-before-write on updates — signal handlers just mutate the dict and call `_write_tab()`.

## Directory Layout

```
{basedir}/runtime/tabs/
    order                      # tab_ids in tab-bar order, one per line
    {tab_id}/
        tab-data.info          # all tab metadata, key: value format
```

### tab-data.info Format

```
url: https://example.com
title: Example Page
index: 0
pinned: false
load_status: success_https
private: false
audio: none
window: 0
created_at: 2026-02-12T15:06:33.199100
```

### Field Reference

| Field | Type | Values / Notes |
|-------|------|----------------|
| `url` | string | `QUrl.toDisplayString()` — decoded, human-readable |
| `title` | string | Page title from `tab.title()` |
| `index` | int | 0-based position in tab bar |
| `pinned` | bool | `true` / `false` |
| `load_status` | enum | `none` / `loading` / `success` / `success_https` / `error` / `warn` (from `usertypes.LoadStatus`) |
| `private` | bool | `true` / `false` |
| `audio` | enum | `muted` / `unmuted` / `none` — derived from `tab.audio.is_muted()` and `tab.audio.is_recently_audible()` |
| `window` | int | `win_id` integer |
| `created_at` | string | ISO 8601 timestamp, set once at tab creation |

### order File

One `tab_id` per line in tab-bar order. Only includes tabs that have been fully initialized (have a directory). This guards against session-restore races where a tab exists in `widgets()` but `_on_new_tab` hasn't fired yet.

## Signal Wiring

All connections are made in `__init__` (browser-level) and `_on_new_tab` (per-tab).

### Browser-level signals (connected in `__init__`)

| Signal | Source | Handler |
|--------|--------|---------|
| `new_tab(tab, idx)` | `TabbedBrowser` | `_on_new_tab` — creates dir, writes initial state, connects per-tab signals, updates indices |
| `shutting_down` | `TabbedBrowser` | `_on_shutdown` — removes entire `tabs/` dir |
| `tabMoved` | `TabBar` (via `widget.tab_bar()`) | `_update_indices` — rewrites all `index` fields + `order` file |

### Per-tab signals (connected in `_on_new_tab`)

| Signal | Source | Field Updated |
|--------|--------|---------------|
| `url_changed(QUrl)` | `AbstractTab` | `url` |
| `title_changed(str)` | `AbstractTab` | `title` |
| `load_status_changed(LoadStatus)` | `AbstractTab` | `load_status` |
| `pinned_changed(bool)` | `AbstractTab` | `pinned` |
| `muted_changed(bool)` | `AbstractAudio` | `audio` (recomputed via `_audio_state()`) |
| `recently_audible_changed(bool)` | `AbstractAudio` | `audio` (recomputed via `_audio_state()`) |
| `shutting_down` | `AbstractTab` | removes tab dir, updates indices |

All per-tab lambdas capture `tab_id` as a default arg to avoid late-binding issues.

## Lifecycle

1. **Init**: wipes `tabs/` with `shutil.rmtree` (handles crash leftovers), recreates it
2. **Tab open**: `_on_new_tab` creates `{tab_id}/` dir, populates `tab-data.info`, wires signals, calls `_update_indices`
3. **Tab update**: signal fires, `_update_field` mutates `_tab_data[tid][key]`, `_write_tab` flushes to disk
4. **Tab close**: `_on_tab_removed` pops from `_tab_data`, `rmtree`s the dir, calls `_update_indices`
5. **Browser shutdown**: `_on_shutdown` clears `_tab_data`, `rmtree`s entire `tabs/`

## Key Design Decisions

- **Wipe on init**: always starts clean. No stale state from crashes. The runtime dir is ephemeral by definition.
- **In-memory dict + flush**: avoids filesystem reads on every signal. `_tab_data` is the source of truth; disk is just the export.
- **`_update_indices` guards on `_tab_data` membership**: during session restore, `widgets()` can contain tabs whose `_on_new_tab` hasn't fired yet. Only tabs in `_tab_data` get written to `order` and have their `index` updated.
- **`FileNotFoundError` catch in `_write_tab`**: the tab dir may be removed by `_on_tab_removed` before a queued signal handler runs.
- **`tab_id` is auto-incremented globally** (`itertools.count(0)` in `browsertab.py:44`), not per-window. IDs are not reused within a session.

## Reading From External Tools

```bash
# All URLs
for f in ~/.runtime/qutebrowser-yeyito/runtime/tabs/*/tab-data.info; do
    grep '^url:' "$f"
done

# Tab bar order with titles
while read tid; do
    grep '^title:' ~/.runtime/qutebrowser-yeyito/runtime/tabs/$tid/tab-data.info
done < ~/.runtime/qutebrowser-yeyito/runtime/tabs/order

# Specific field from specific tab
awk -F': ' '/^load_status/{print $2}' ~/.runtime/qutebrowser-yeyito/runtime/tabs/0/tab-data.info
```

## Source Files

| File | Role |
|------|------|
| `qutebrowser/browser/tabruntime.py` | `TabRuntimeManager` class — all runtime tab logic |
| `qutebrowser/mainwindow/tabbedbrowser.py:24,247` | Import + instantiation (2 lines total) |
| `qutebrowser/browser/browsertab.py` | `AbstractTab` — tab signals, `tab_id`, `is_private`, `win_id`, `data.pinned`, `audio` sub-API |
| `qutebrowser/utils/usertypes.py:276-285` | `LoadStatus` enum definition |
| `qutebrowser/utils/standarddir.py:239` | `runtime()` — returns runtime directory path |
