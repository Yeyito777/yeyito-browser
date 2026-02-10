#!/usr/bin/env bash
# ============================================================
# Diagnostic script for Discord/WhatsApp logout investigation
#
# THEORY A: qutebrowser's Service Worker nuking
#   - qutebrowser deletes Service Worker dir on Qt/QtWebEngine version change
#   - File: qutebrowser/misc/backendproblem.py:293-324
#   - Triggered by: configfiles.state.qt_version_changed or qtwe_version_changed
#
# THEORY B: Chromium's quota eviction system
#   - Chromium evicts "best-effort" IndexedDB data when disk > 70% of pool
#   - Runs every 30 minutes automatically
#   - LRU eviction hits least-recently-used origins first
#   - Build artifacts (~10+ GB) could push disk over the threshold
#
# Run this BEFORE launching qutebrowser / BEFORE a rebuild
# ============================================================
set -euo pipefail

data_dir="${XDG_DATA_HOME:-$HOME/.local/share}/qutebrowser"
webengine_dir="${data_dir}/webengine"
state_file="${data_dir}/state"
sw_dir="${webengine_dir}/Service Worker"
indexeddb_dir="${webengine_dir}/IndexedDB"
localstorage_dir="${webengine_dir}/Local Storage"
log_file="${data_dir}/logout-diagnosis.log"

ts() { date '+%Y-%m-%d %H:%M:%S'; }

log() { echo "[$(ts)] $*" | tee -a "$log_file"; }

divider() { log "========================================"; }

divider
log "LOGOUT DIAGNOSIS START"
divider

# ============================================================
# CHECK 1: State file - version tracking
# ============================================================
log ""
log "--- CHECK 1: qutebrowser state file (version tracking) ---"
if [[ -f "$state_file" ]]; then
    log "State file exists: $state_file"
    log "Stored versions:"
    grep -E 'qt_version|qtwe_version|chromium_version|^version' "$state_file" | while read -r line; do
        log "  $line"
    done
else
    log "WARNING: State file does not exist yet"
fi

# Current runtime versions (requires the venv)
venv_python="${HOME}/.local/share/qutebrowser-venv/bin/python"
if [[ -x "$venv_python" ]]; then
    log ""
    log "Current runtime versions:"

    qt_ver=$("$venv_python" -c "
from PyQt6.QtCore import qVersion
print(f'  Qt runtime: {qVersion()}')
" 2>/dev/null || echo "  Qt runtime: UNAVAILABLE")
    log "$qt_ver"

    qtwe_ver=$("$venv_python" -c "
try:
    from PyQt6.QtWebEngineCore import qWebEngineVersion, qWebEngineChromiumVersion
    print(f'  QtWebEngine: {qWebEngineVersion()}')
    print(f'  Chromium: {qWebEngineChromiumVersion()}')
except Exception as e:
    print(f'  QtWebEngine: ERROR - {e}')
" 2>&1) || true
    # Use LD_LIBRARY_PATH from the launcher to pick up custom build
    install_lib="${HOME}/Workspace/Qutebrowser/build/install/lib"
    if [[ -d "$install_lib" ]]; then
        log "  (with system libs):"
        log "$qtwe_ver"
        log ""
        qtwe_ver_custom=$(LD_LIBRARY_PATH="${install_lib}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
            "$venv_python" -c "
try:
    from PyQt6.QtWebEngineCore import qWebEngineVersion, qWebEngineChromiumVersion
    print(f'  QtWebEngine: {qWebEngineVersion()}')
    print(f'  Chromium: {qWebEngineChromiumVersion()}')
except Exception as e:
    print(f'  QtWebEngine: ERROR - {e}')
" 2>&1) || true
        log "  (with custom build libs):"
        log "$qtwe_ver_custom"
    else
        log "$qtwe_ver"
    fi

    log ""
    log "VERSION MISMATCH CHECK:"
    if [[ -f "$state_file" ]]; then
        stored_qtwe=$(grep -oP 'qtwe_version = \K.*' "$state_file" 2>/dev/null || echo "NOT_FOUND")
        stored_qt=$(grep -oP 'qt_version = \K.*' "$state_file" 2>/dev/null || echo "NOT_FOUND")
        log "  Stored qt_version = $stored_qt"
        log "  Stored qtwe_version = $stored_qtwe"
        log "  (If current != stored, Service Workers WILL be nuked on next launch)"
    fi
else
    log "WARNING: venv python not found at $venv_python"
fi

# ============================================================
# CHECK 2: Disk space / quota pressure
# ============================================================
log ""
log "--- CHECK 2: Disk space (quota eviction triggers at ~70% pool usage) ---"
if [[ -d "$webengine_dir" ]]; then
    disk_info=$(df -h "$webengine_dir" | tail -1)
    log "  Filesystem: $disk_info"

    disk_usage_pct=$(df "$webengine_dir" | tail -1 | awk '{print $5}' | tr -d '%')
    log "  Usage: ${disk_usage_pct}%"
    if [[ "$disk_usage_pct" -ge 70 ]]; then
        log "  WARNING: Disk usage >= 70% — Chromium quota eviction may be active!"
    fi

    # Build directory size
    build_dir="${HOME}/Workspace/Qutebrowser/build"
    if [[ -d "$build_dir" ]]; then
        build_size=$(du -sh "$build_dir" 2>/dev/null | cut -f1)
        log "  Build directory size: $build_size"
    fi
fi

# ============================================================
# CHECK 3: IndexedDB contents (per-origin)
# ============================================================
log ""
log "--- CHECK 3: IndexedDB databases on disk ---"
if [[ -d "$indexeddb_dir" ]]; then
    log "  IndexedDB directory: $indexeddb_dir"
    for origin_dir in "$indexeddb_dir"/*/; do
        if [[ -d "$origin_dir" ]]; then
            origin_name=$(basename "$origin_dir")
            origin_size=$(du -sh "$origin_dir" 2>/dev/null | cut -f1)
            file_count=$(find "$origin_dir" -type f 2>/dev/null | wc -l)
            log "  $origin_name: $origin_size ($file_count files)"
        fi
    done
else
    log "  IndexedDB directory does not exist: $indexeddb_dir"
fi

# ============================================================
# CHECK 4: localStorage contents
# ============================================================
log ""
log "--- CHECK 4: Local Storage on disk ---"
if [[ -d "$localstorage_dir" ]]; then
    ls_size=$(du -sh "$localstorage_dir" 2>/dev/null | cut -f1)
    log "  Local Storage total: $ls_size"
    # leveldb directory listing
    if [[ -d "$localstorage_dir/leveldb" ]]; then
        file_count=$(find "$localstorage_dir/leveldb" -type f 2>/dev/null | wc -l)
        log "  LevelDB files: $file_count"
    fi
else
    log "  Local Storage directory does not exist"
fi

# ============================================================
# CHECK 5: Service Worker directory
# ============================================================
log ""
log "--- CHECK 5: Service Worker directory ---"
if [[ -d "$sw_dir" ]]; then
    sw_size=$(du -sh "$sw_dir" 2>/dev/null | cut -f1)
    log "  Service Worker dir: $sw_size"
else
    log "  Service Worker dir does NOT exist (already nuked?)"
fi
if [[ -d "${sw_dir}-bak" ]]; then
    log "  Service Worker BACKUP exists (was nuked on last launch!)"
    bak_size=$(du -sh "${sw_dir}-bak" 2>/dev/null | cut -f1)
    log "  Backup size: $bak_size"
fi

# ============================================================
# CHECK 6: Snapshot fingerprint (run before and after to compare)
# ============================================================
log ""
log "--- CHECK 6: Storage fingerprint (compare before/after) ---"
snapshot_file="${data_dir}/storage-snapshot-$(date +%s).txt"
{
    echo "=== Snapshot at $(ts) ==="
    echo ""
    echo "--- IndexedDB origins ---"
    if [[ -d "$indexeddb_dir" ]]; then
        for d in "$indexeddb_dir"/*/; do
            [[ -d "$d" ]] && echo "$(basename "$d"): $(du -sb "$d" | cut -f1) bytes"
        done
    fi
    echo ""
    echo "--- Service Worker ---"
    [[ -d "$sw_dir" ]] && echo "EXISTS: $(du -sb "$sw_dir" | cut -f1) bytes" || echo "MISSING"
    echo ""
    echo "--- Local Storage ---"
    [[ -d "$localstorage_dir" ]] && echo "$(du -sb "$localstorage_dir" | cut -f1) bytes" || echo "MISSING"
} > "$snapshot_file"
log "  Snapshot saved to: $snapshot_file"
log "  (Run this script again after the logout event and diff the snapshots)"

# ============================================================
# CHECK 7: Set up filesystem watcher (optional)
# ============================================================
log ""
log "--- CHECK 7: Filesystem watcher ---"
if command -v inotifywait &>/dev/null; then
    log "  inotifywait is available"
    log "  To watch for deletions in real-time, run in a separate terminal:"
    log ""
    log "    inotifywait -m -r -e delete,moved_from,moved_to '$webengine_dir' 2>&1 | tee '$data_dir/fs-watch.log'"
    log ""
else
    log "  inotifywait not found. Install inotify-tools for real-time monitoring:"
    log "    sudo pacman -S inotify-tools"
fi

divider
log "DIAGNOSIS COMPLETE"
log "Log saved to: $log_file"
divider

echo ""
echo "NEXT STEPS:"
echo "  1. Run this script now (baseline snapshot)"
echo "  2. Start the filesystem watcher (inotifywait command above) in another terminal"
echo "  3. Launch qutebrowser normally and check if Discord/WhatsApp are logged out"
echo "  4. If logged out: check the inotifywait log and run this script again to diff snapshots"
echo "  5. If NOT logged out: rebuild with ./install.sh --dirty, then repeat step 3-4"
echo ""
