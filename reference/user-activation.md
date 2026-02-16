# User Activation API

Exposes Chromium's frame-level "user activation" to Python, allowing programmatic calls like `video.play()` to succeed even when `PlaybackRequiresUserGesture` is enabled (i.e., `content.autoplay = False`).

## Why This Exists

Chromium enforces autoplay restrictions via "user activation" — a per-frame flag set by real user input (clicks, key presses). When `content.autoplay = False`, qutebrowser sets `PlaybackRequiresUserGesture = true` in WebSettings, which blocks `video.play()` unless the frame has user activation. This is intentional for normal browsing, but breaks session restore: a greasemonkey script that wants to resume a video that was playing before `:wq` cannot call `play()` because the restored page has no user activation.

The solution: expose `RenderFrameHost::NotifyUserActivation()` through the Qt/Python layers so qutebrowser can grant activation programmatically.

## Architecture

```
Python: page.notifyUserActivation()
  → QWebEnginePage::notifyUserActivation()          [qwebenginepage.cpp]
    → WebContentsAdapter::notifyUserActivation()     [web_contents_adapter.cpp]
      → RenderFrameHost::NotifyUserActivation(kInteraction)  [Chromium]
        → blink::LocalFrame::NotifyUserActivationInFrameTree()
```

This is a **direct frame method** (like `runJavaScript`), not a WebSettings pipeline. No mojom IPC changes, no 10-file pipeline — just a method call forwarded through the adapter.

## Key Difference from WebSettings

| Aspect | WebSettings (e.g., ElementShader) | Frame Method (UserActivation) |
|--------|-----------------------------------|-------------------------------|
| Scope | Global browser setting | Per-frame, per-call |
| IPC | Requires mojom pipeline (10 files) | Direct RenderFrameHost call |
| Persistence | Saved across navigations | Sticky until page close |
| Files changed | ~10 across 3 layers | 5 files |
| Build time | ~2 hours (first build) | 1-5 minutes |

## User Activation Types

Chromium defines several notification types in `user_activation_notification_type.mojom`:
- `kInteraction` (1) — what we use, simulates trusted user interaction
- `kTest` (46) — for testing
- Many others for extensions, media APIs, etc.

We hardcode `kInteraction` since it grants both **sticky** activation (persists until page close) and **transient** activation (expires after ~5 seconds). Sticky activation is what matters for `video.play()`.

## Files Modified

### Chromium (no changes needed)
The API already exists at `content/public/browser/render_frame_host.h:995`:
```cpp
virtual void NotifyUserActivation(
    blink::mojom::UserActivationNotificationType notification_type) = 0;
```

### QtWebEngine Core

**`qtwebengine/src/core/web_contents_adapter.h`**
```cpp
void notifyUserActivation(quint64 frameId);
```

**`qtwebengine/src/core/web_contents_adapter.cpp`**
```cpp
void WebContentsAdapter::notifyUserActivation(quint64 frameId)
{
    if (!isInitialized())
        return;
    auto *rfh = renderFrameHostFromFrameId(frameId);
    if (!rfh)
        return;
    rfh->NotifyUserActivation(
        blink::mojom::UserActivationNotificationType::kInteraction);
}
```
Follows the exact same pattern as `runJavaScript()`: check initialized, get RenderFrameHost via `renderFrameHostFromFrameId()`, call the Chromium API.

Added include: `third_party/blink/public/mojom/frame/user_activation_notification_type.mojom.h`

**`qtwebengine/src/core/api/qwebenginepage.h`**
```cpp
void notifyUserActivation();
```

**`qtwebengine/src/core/api/qwebenginepage.cpp`**
```cpp
void QWebEnginePage::notifyUserActivation()
{
    Q_D(QWebEnginePage);
    d->ensureInitialized();
    d->adapter->notifyUserActivation(WebContentsAdapter::kUseMainFrameId);
}
```
Always targets the main frame — subframe activation isn't needed for our use case.

### SIP Bindings

**`pyqt6-webengine/sip/QtWebEngineCore/qwebenginepage.sip`**
```sip
void notifyUserActivation();
```
Simple method binding, no special SIP annotations needed (unlike `runJavaScript` which needs `%MethodCode` for callback handling).

### Qutebrowser Python

**`qutebrowser/browser/commands.py`** — `:grant-user-activation <tab_id>` command:
```python
@cmdutils.register(instance='command-dispatcher', scope='window')
def grant_user_activation(self, tab_id: int):
    ...
    tab._widget.page().notifyUserActivation()
```

**`qutebrowser/browser/tabruntime.py`** — Auto-resume hook:
- `_on_youtube_resume_signal` listens on the `console_message` signal for the `[yt-resume-ready]` marker emitted by the greasemonkey script
- Connected per-tab in `_on_new_tab` — no URL checks or timers needed (the greasemonkey script only emits the signal on YouTube watch pages when `restoredPlaying` is true)
- On signal: immediately grants user activation and runs `video.play()` (guarded by `_yt_resumed` flag to prevent duplicates)
- No fixed delays — playback starts as soon as the seek settles

## Greasemonkey Script Integration

The `youtube-resume.user.js` userscript (in `config/greasemonkey/`) handles save/restore:

**Saving** (every 3s interval + beforeunload):
```json
{"time": 96.35, "playing": true, "duration": 220.36, "savedAt": 1771256492518}
```
The `beforeunload` handler preserves the `playing` value from the last interval save, since the browser pauses media during shutdown (making `video.paused` unreliable at that point).

**Restoring** (on `yt-navigate-finish`, not initial load):
- Seeks to saved time with retry logic (YouTube's player init can overwrite seeks)
- Does NOT call `play()` — that's handled by the Python-side auto-resume hook
- When seek settles and `restoredPlaying` is true, emits `console.log('[yt-resume-ready]')` — this is the signal that Python watches for via `console_message` to grant activation and play immediately

**Why `yt-navigate-finish`**: YouTube is a SPA. On session restore, the initial page load fires `document-idle` before YouTube's player is initialized. Seeking at that point gets overwritten ~3-5 seconds later when `yt-navigate-finish` fires. So the script only seeks after that event.

**Why saving is deferred to `yt-navigate-finish`**: The initial `startForVideo(false)` call only sets `currentVideoId` (for `beforeunload`) and returns without starting the save interval. If saving started immediately at `document-idle`, the first interval tick would see the video paused (no user activation yet) and overwrite the pre-restore `playing: true` entry with `playing: false`, causing the Python-side auto-resume to skip the tab.

**Restore grace period (`restoredPlaying`)**: Even with saving deferred to `yt-navigate-finish`, the save interval starts at ~4s and its first tick fires at ~7s — still before Python reads at 8s. To prevent this, `startForVideo(true)` captures the pre-restore `playing` value into `restoredPlaying` before seeking. During saves, `savePosition` uses `restoredPlaying` instead of `!video.paused` while it's set. The `playing` event on the `<video>` element clears `restoredPlaying` (fires when auto-resume or user click actually starts playback), switching saves back to live state. `stopSaving()` also clears it so SPA navigations start fresh.

## Important Caveats

- **User activation is sticky**: once granted, it persists until the page is closed. This means any JS on the page can call `play()` after activation is granted. The `restoredPlaying` check + `_yt_resumed` flag ensures we only grant it to tabs that were actually playing, and only once.
- **`content.autoplay = False` is preserved**: the WebSettings restriction stays in place. We grant activation selectively from Python, not broadly.
- **`@run-at document-start` does not work** with qutebrowser's greasemonkey implementation — the observer and event listeners don't fire. Always use `document-idle`.

## Usage

### Manual (command)
```
:grant-user-activation <tab_id>
```

### Programmatic (Python)
```python
tab._widget.page().notifyUserActivation()
```

### Automatic (session restore)
Handled by `TabRuntimeManager._try_youtube_resume()` — no user action needed.
