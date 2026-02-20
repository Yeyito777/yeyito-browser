# Network Tab Tool — Technical Reference

Deep technical reference for the network inspection system. Covers every file, function, and line involved in getting network request data from Chromium's internals to `network.sh` stdout.

## Architecture Overview

```
Chromium renderer completes a resource load
  → content::WebContentsObserver::ResourceLoadComplete() fires
  → WebContentsDelegateQt::ResourceLoadComplete() extracts fields into NetworkRequestEntry
  → NetworkRequestBuffer::addEntry() stores in std::vector (max 1000, FIFO eviction)

User runs: network.sh list
  → Shell script sends IPC JSON via socat to Unix socket
  → qutebrowser IPC dispatches :network-list command
  → CommandDispatcher.network_list() → TabRuntimeManager.network_list()
  → WebEngineTab.network_query('list', {}, callback)
  → QWebEnginePage.networkQuery("list", "{}") [SIP binding]
  → WebContentsAdapter::networkQuery("list", "{}") [C++ core]
  → NetworkRequestBuffer::queryList() → JSON QString
  → Callback writes JSON to network.json file
  → Shell script polls for file, applies jq filters, writes filtered data back
  → Prints colored summary to stdout (count, types, errors)
  → Full JSON remains in network.json for programmatic access
```

## File Map

### C++ Layer (QtWebEngine)

| File | Lines | Role |
|------|-------|------|
| `qtwebengine/src/core/net/network_request_buffer.h` | 1–66 | `NetworkRequestEntry` struct + `NetworkRequestBuffer` class definition |
| `qtwebengine/src/core/net/network_request_buffer.cpp` | 1–97 | JSON serialization (`toSummaryJson`, `toDetailJson`), buffer management (`addEntry`, `clear`, `queryList`, `queryDetail`) |
| `qtwebengine/src/core/web_contents_delegate_qt.h` | 12,153,223 | `#include`, `networkBuffer()` accessor, `m_networkBuffer` member |
| `qtwebengine/src/core/web_contents_delegate_qt.cpp` | 942–1038 | `requestDestinationToType()`, `timeDeltaMs()`, `ResourceLoadComplete()` populator |
| `qtwebengine/src/core/web_contents_delegate_qt.cpp` | 481–483 | `PrimaryPageChanged()` — `m_networkBuffer.clear()` |
| `qtwebengine/src/core/web_contents_adapter.h` | 148 | `networkQuery()` declaration |
| `qtwebengine/src/core/web_contents_adapter.cpp` | 1111–1143 | `networkQuery()` — dispatcher for list/detail/body/ws/headers/cookies |
| `qtwebengine/src/core/api/qwebenginepage.h` | 292 | `networkQuery()` public API declaration |
| `qtwebengine/src/core/api/qwebenginepage.cpp` | 2089–2095 | `networkQuery()` — delegates to adapter |
| `qtwebengine/src/core/CMakeLists.txt` | 140 | Build system entry for `net/network_request_buffer.cpp` and `.h` |

### SIP Binding

| File | Line | Role |
|------|------|------|
| `pyqt6-webengine/sip/QtWebEngineCore/qwebenginepage.sip` | 166 | `QString networkQuery(const QString &queryType, const QString &argsJson = QString()) const;` |

Simple return type (QString), no callback marshaling needed. SIP auto-generates the Python↔C++ bridge.

### Python Layer (Qutebrowser)

| File | Lines | Role |
|------|-------|------|
| `qutebrowser/browser/browsertab.py` | 1271–1276 | `AbstractTab.network_query()` — abstract method definition |
| `qutebrowser/browser/webengine/webenginetab.py` | 1438–1442 | `WebEngineTab.network_query()` — calls `page().networkQuery()` via SIP |
| `qutebrowser/browser/tabruntime.py` | 423–533 | `network_list()`, `network_detail()`, `network_body()`, `network_ws_frames()` — write results to files |
| `qutebrowser/browser/tabruntime.py` | 535–740 | `_write_network_script()` — generates the `network.sh` shell script |
| `qutebrowser/browser/commands.py` | 1970–2068 | `:network-list`, `:network-detail`, `:network-body`, `:network-ws-frames` IPC commands |

### Shell Script (generated per tab at runtime)

| File | Role |
|------|------|
| `{basedir}/runtime/tabs/{id}/network.sh` | Generated shell script, hardcoded paths for this tab |
| `{basedir}/runtime/tabs/{id}/network.json` | JSON output file (list, ws queries) |
| `{basedir}/runtime/tabs/{id}/request-{req_id}.json` | JSON output file (detail query — includes response body + headers) |
| `{basedir}/runtime/tabs/{id}/network-body` | Raw bytes output file (body query) |

---

## C++ Internals

### NetworkRequestEntry (`network_request_buffer.h:16–46`)

```cpp
struct NetworkRequestEntry {
    int64_t requestId = 0;        // blink::mojom::ResourceLoadInfo::request_id
    QString url;                   // final_url.spec() (after redirects)
    QString originalUrl;           // original_url.spec() (before redirects)
    QString method;                // HTTP method ("GET", "POST", etc.)
    QString resourceType;          // mapped from RequestDestination enum
    QString mimeType;              // response MIME type
    int httpStatusCode = 0;        // HTTP status (200, 404, etc.)
    int netError = 0;              // net::Error code (0 = OK, -2 = FAILED, etc.)
    bool wasCached = false;        // served from HTTP cache?
    int64_t rawBodyBytes = 0;      // body size before content-encoding
    int64_t totalReceivedBytes = 0;// total bytes including headers

    // Timing: milliseconds relative to request_start (0 = not available)
    double dnsStartMs, dnsEndMs;
    double connectStartMs, connectEndMs;
    double sslStartMs, sslEndMs;
    double sendStartMs, sendEndMs;
    double receiveHeadersStartMs, receiveHeadersEndMs;

    QString remoteEndpoint;        // "IP:port" string
    QJsonObject requestHeaders;    // blink request headers (sub-resource requests only)

    QJsonObject toSummaryJson() const;  // compact: id, url, method, status, type, mimeType, size, cached
    QJsonObject toDetailJson() const;   // everything including timing object
};
```

### NetworkRequestBuffer (`network_request_buffer.h:48–62`)

```cpp
class NetworkRequestBuffer {
public:
    static constexpr int kMaxEntries = 1000;

    void addEntry(NetworkRequestEntry entry);     // push_back, evicts [0] if full
    void clear();                                  // m_entries.clear()
    QString queryList() const;                     // → {"requests":[...], "count":N}
    QString queryDetail(int64_t requestId) const;  // → single entry or {"error":"request not found"}
    int size() const;

private:
    std::vector<NetworkRequestEntry> m_entries;    // ordered by insertion (oldest first)
};
```

**Eviction**: FIFO. When `m_entries.size() >= 1000`, `erase(begin())` removes the oldest entry before pushing new one (`network_request_buffer.cpp:62–64`).

**JSON serialization**: Uses `QJsonDocument::Indented` format. `int64_t` fields require explicit `static_cast<qint64>()` because GCC can't disambiguate `QJsonValue(int64_t)` against the `QJsonValue(qint64)`, `QJsonValue(int)`, `QJsonValue(double)`, and `QJsonValue(bool)` constructors.

### toSummaryJson (`network_request_buffer.cpp:10–24`)

Returns 8 fields: `id`, `url`, `method`, `status`, `type`, `mimeType`, `size`, `cached`. The `netError` field is only included when non-zero (conditional at line 21–22).

### toDetailJson (`network_request_buffer.cpp:26–58`)

Returns all summary fields plus: `originalUrl`, `netError` (always), `rawBodyBytes`, `totalReceivedBytes`, `remoteEndpoint` (conditional on non-empty), `requestHeaders` (conditional on non-empty), and a nested `timing` object with 10 fields.

### requestDestinationToType (`web_contents_delegate_qt.cpp:942–981`)

Maps `network::mojom::RequestDestination` enum values to human-readable type strings:

| Enum value(s) | String |
|---------------|--------|
| `kDocument` | `"document"` |
| `kFrame`, `kIframe` | `"frame"` |
| `kImage` | `"image"` |
| `kScript` | `"script"` |
| `kStyle` | `"stylesheet"` |
| `kFont` | `"font"` |
| `kAudio`, `kVideo` | `"media"` |
| `kTrack` | `"track"` |
| `kManifest` | `"manifest"` |
| `kWorker`, `kSharedWorker`, `kServiceWorker` | `"worker"` |
| `kEmpty` | `"fetch"` |
| `kObject`, `kEmbed` | `"object"` |
| `kReport` | `"report"` |
| `kXslt` | `"xslt"` |
| everything else | `"other"` |

`kEmpty` maps to `"fetch"` because fetch(), XHR, beacon, and ping all use `RequestDestination::kEmpty` in Chromium. The `RequestDestination` enum is defined in `services/network/public/mojom/fetch_api.mojom.h`.

### timeDeltaMs (`web_contents_delegate_qt.cpp:983–988`)

```cpp
static double timeDeltaMs(base::TimeTicks start, base::TimeTicks end)
{
    if (start.is_null() || end.is_null())
        return 0;
    return (end - start).InMillisecondsF();
}
```

Returns 0 for null ticks (cached requests, phases that didn't happen). Uses `base::TimeTicks` — monotonic, not wall-clock.

### ResourceLoadComplete (`web_contents_delegate_qt.cpp:990–1038`)

This is the `content::WebContentsObserver` override that fires for every completed resource load. It receives a `blink::mojom::ResourceLoadInfo` struct from the renderer process via Mojo IPC.

Flow:
1. **Lines 997–999**: Existing logic — sets `m_isDocumentEmpty` for document resources
2. **Lines 1001–1012**: Creates `NetworkRequestEntry`, copies scalar fields directly from `resource_load_info`
3. **Lines 1014–1018**: Extracts remote endpoint from `resource_load_info.network_info->remote_endpoint` (optional field, may be null)
4. **Lines 1020–1035**: Computes timing deltas. `resource_load_info.load_timing_info` is a `net::LoadTimingInfo` struct. Timing is nested:
   - `load_timing_info.request_start` — base time (time zero)
   - `load_timing_info.connect_timing.domain_lookup_start/end` — DNS
   - `load_timing_info.connect_timing.connect_start/end` — TCP
   - `load_timing_info.connect_timing.ssl_start/end` — TLS
   - `load_timing_info.send_start/end` — request sent
   - `load_timing_info.receive_headers_start/end` — response headers
5. **Lines 1037–1040**: Copies request headers from `resource_load_info.request_headers` (a `flat_map<string, string>`) into `entry.requestHeaders` as a `QJsonObject`. Only sub-resource requests have headers populated — document/navigation requests arrive with an empty map due to Chromium's architecture (see Mojo struct section).
6. **Line 1042**: Moves entry into buffer

### PrimaryPageChanged (`web_contents_delegate_qt.cpp:481–483`)

```cpp
void WebContentsDelegateQt::PrimaryPageChanged(content::Page &)
{
    m_networkBuffer.clear();
    ...
}
```

Clears the buffer on every top-level navigation. This is the Chromium equivalent of "navigating to a new page" — fires after the new document commits. Subframe navigations do NOT trigger this.

### WebContentsAdapter::networkQuery (`web_contents_adapter.cpp:1111–1143`)

The C++ dispatcher. Gets the buffer from the delegate via `const_cast` (buffer accessor is non-const because it returns a reference, but the read operations are logically const).

Query type dispatch:

| queryType | Action |
|-----------|--------|
| `"list"` | `buffer.queryList()` |
| `"detail"` | Parse `request_id` from `argsJson`, call `buffer.queryDetail(id)` |
| `"body"` | Returns error: "not yet implemented" |
| `"ws"` or `"ws_frames"` | Returns error: "not yet implemented" |
| `"headers"` | Returns error: "not yet implemented" |
| `"cookies"` | Returns error: "not yet implemented" |
| anything else | Returns error: "unknown query type" |

**Detail arg parsing** (`web_contents_adapter.cpp:1122–1130`): The `argsJson` is `{"request_id": "685216"}` — request_id comes as a string from the shell. The parser handles both `isDouble()` and `isString()` cases because Python's `json.dumps` can produce either depending on input type. String values use `toLongLong()` for int64 parsing.

### QWebEnginePage::networkQuery (`qwebenginepage.cpp:2089–2095`)

Thin public API wrapper. Null-checks the adapter and delegates:

```cpp
QString QWebEnginePage::networkQuery(const QString &queryType, const QString &argsJson) const
{
    Q_D(const QWebEnginePage);
    if (!d->adapter)
        return QStringLiteral("{\"error\":\"not initialized\"}");
    return d->adapter->networkQuery(queryType, argsJson);
}
```

Declared at `qwebenginepage.h:292`.

---

## SIP Binding

`pyqt6-webengine/sip/QtWebEngineCore/qwebenginepage.sip:166`:

```sip
QString networkQuery(const QString &queryType, const QString &argsJson = QString()) const;
```

SIP generates the PyObject↔QString marshaling automatically. The return is a plain `QString` (maps to Python `str`), so no callback or async machinery is needed. This makes the Python call synchronous — it blocks until the C++ method returns.

---

## Python Layer

### AbstractTab.network_query (`browsertab.py:1271`)

```python
def network_query(self, query_type, query_args, callback):
    """Query DevTools network data. Implemented by backend."""
    raise NotImplementedError
```

Three args: `query_type` (str), `query_args` (dict), `callback` (callable taking a str).

### WebEngineTab.network_query (`webenginetab.py:1438`)

```python
def network_query(self, query_type, query_args, callback):
    import json
    args_json = json.dumps(query_args)
    result = self._widget.page().networkQuery(query_type, args_json)
    callback(result)
```

Serializes `query_args` dict to JSON string, calls the SIP-bound `networkQuery`, and immediately invokes the callback with the result. This is synchronous because `networkQuery` reads from an in-memory buffer (no async I/O).

### TabRuntimeManager Network Methods (`tabruntime.py:423–533`)

Four methods, one per subcommand:

| Method | Line | Query type | Output file |
|--------|------|-----------|-------------|
| `network_list(tab_id_str)` | 423 | `'list'` | `network.json` |
| `network_detail(tab_id_str, request_id)` | 440 | `'detail'` → JS fetch | `request-{id}.json` |
| `network_body(tab_id_str, request_id)` | 513 | `'body'` | `network-body` |
| `network_ws_frames(tab_id_str, request_id)` | 518 | `'ws_frames'` | `network.json` |

`network_list`, `network_body`, `network_ws_frames` follow the simple pattern: find tab → query C++ → write result string to file.

`network_detail` uses an async chain to enrich C++ metadata with the response body and headers:

1. `tab.network_query('detail', ...)` returns C++ metadata (timing, status, etc.)
2. Parses the JSON; if it has a `url`, kicks off a JS `fetch(url, {cache: "force-cache"})` in the page's main world
3. The fetch retrieves the response from the browser's HTTP cache — no network round-trip
4. `r.headers.forEach()` collects response headers; `r.text()` gets the body
5. Result is sent back to Python via `console.log` with a sentinel prefix (`__qb_nr_{request_id}`), because QtWebEngine's `runJavaScript` callback doesn't resolve Promises
6. A `console_message` signal handler catches the sentinel, merges `responseHeaders` and `body` (or `bodyError`) into the C++ detail dict, and writes the combined JSON to `request-{id}.json`

Limitations: cross-origin resources without CORS headers fail with `bodyError: "Failed to fetch"` (C++ metadata still written). POST responses can't be re-fetched from cache. `no-store` responses aren't cached.

Note: `network_ws_frames` sends query type `"ws_frames"` (not `"ws"`). The C++ dispatcher accepts both (`web_contents_adapter.cpp:1134`).

### _write_network_script (`tabruntime.py:535–740`)

Generates the `network.sh` shell script with hardcoded paths for:
- `SOCKET` — IPC socket path (MD5-hashed from `{user}-{basedir}`)
- `RESULT_FILE` — `{tabs_dir}/{tab_id}/network.json`
- `BODY_FILE` — `{tabs_dir}/{tab_id}/network-body`
- `TAB_ID` — the tab's string ID
- `TAB_DIR` — `{tabs_dir}/{tab_id}` (used to construct `request-{id}.json` paths)

The `detail` subcommand polls for `$TAB_DIR/request-${REQUEST_ID}.json` instead of `$RESULT_FILE`, matching the per-request output files written by `network_detail()`.

The script is made executable via `chmod +x`.

### IPC Commands (`commands.py:1970–2068`)

Four registered commands, all follow the same pattern:

```python
@cmdutils.register(instance='command-dispatcher', scope='window')
def network_list(self, tab_id: int):
    tab_id_str = str(tab_id)
    if self._tabbed_browser.tab_runtime.network_list(tab_id_str):
        return
    # Cross-window fallback: search other windows
    for win_id in objreg.window_registry:
        if win_id == self._win_id:
            continue
        tabbed_browser = objreg.get('tabbed-browser', scope='window', window=win_id)
        if tabbed_browser.tab_runtime.network_list(tab_id_str):
            return
    raise cmdutils.CommandError("No tab with ID {} found".format(tab_id))
```

| Command | Line | Python method |
|---------|------|--------------|
| `:network-list` | 1970 | `network_list(tab_id)` |
| `:network-detail` | 1993 | `network_detail(tab_id, request_id)` |
| `:network-body` | 2019 | `network_body(tab_id, request_id)` |
| `:network-ws-frames` | 2045 | `network_ws_frames(tab_id, request_id)` |

All commands try the current window first, then iterate `objreg.window_registry` to find the tab in other windows.

---

## Shell Script Internals

### IPC Protocol

The script communicates with qutebrowser via its IPC socket (Unix domain socket). The protocol is JSON-over-newline:

```json
{"args":[":network-list 0"],"target_arg":"tab-silent","protocol_version":1}
```

- `args` — command string (with colon prefix)
- `target_arg` — `"tab-silent"` suppresses window raise and urgency hints
- `protocol_version` — always 1

Sent via `socat - UNIX-CONNECT:{socket_path}`.

### Polling

After sending the IPC command, the script polls for the output file:

- **Interval**: 0.5 seconds
- **Attempts**: `timeout / interval` (default: 5 / 0.5 = 10 attempts)
- **Poll file**: `network.json` for list/ws, `request-{id}.json` for detail, `network-body` for body

The output file is deleted before sending the command (`rm -f`) so its appearance signals completion.

### Client-Side Filtering

Filters are applied via `jq` after the result file appears. Only for the `list` subcommand. The script checks `command -v jq` before each filter — silently skips if jq isn't installed. Filtered output is written back to the result file (replacing unfiltered data).

**--errors**:
```sh
jq '{requests: [.requests[] | select(.status >= 400 or .status == 0)],
     count: ([.requests[] | select(.status >= 400 or .status == 0)] | length)}'
```

**--type**:
```sh
jq --arg t "$FILTER_TYPE" '{requests: [.requests[] | select(.type == $t)],
    count: ([.requests[] | select(.type == $t)] | length)}'
```

**--url**:
```sh
jq --arg p "$FILTER_URL" '{requests: [.requests[] | select(.url | test($p))],
    count: ([.requests[] | select(.url | test($p))] | length)}'
```

### Pretty-Print Output

After the result file appears (and filters are applied for `list`), the script prints a colored summary to stdout instead of dumping raw JSON. This mirrors the `snapshot-dom.sh` pattern.

**list**: Shows file saved, size in KB, request count, type breakdown (e.g. "1 document, 7 fetch, 19 font"), and error count if any.

**detail**: Shows file saved (`request-{id}.json`), method/status/type, truncated URL (60 chars), body size + response header count (or `bodyError` in yellow if fetch failed), remote endpoint, and timing summary (dns/tcp/tls/ttfb in ms).

**body/ws**: Shows file saved and size in KB.

The full JSON data is always available in the output files (`network.json`, `request-{id}.json`, `network-body`) for programmatic consumption.

---

## Chromium Data Source

### blink::mojom::ResourceLoadInfo

Defined in `third_party/blink/public/mojom/loader/resource_load_info.mojom`. This is a Mojo struct sent from the renderer to the browser process when a resource load completes. Fields used:

| Mojo field | C++ type | NetworkRequestEntry field |
|-----------|----------|--------------------------|
| `request_id` | `int64` | `requestId` |
| `final_url` | `url::GURL` | `url` (via `.spec()`) |
| `original_url` | `url::GURL` | `originalUrl` (via `.spec()`) |
| `method` | `std::string` | `method` |
| `request_destination` | `network::mojom::RequestDestination` | `resourceType` (via mapping) |
| `mime_type` | `std::string` | `mimeType` |
| `http_status_code` | `int32` | `httpStatusCode` |
| `net_error` | `int32` | `netError` |
| `was_cached` | `bool` | `wasCached` |
| `raw_body_bytes` | `int64` | `rawBodyBytes` |
| `total_received_bytes` | `int64` | `totalReceivedBytes` |
| `load_timing_info` | `net::LoadTimingInfo` | timing fields |
| `network_info` | `blink::mojom::CommonNetworkInfoPtr` | `remoteEndpoint` |
| `request_headers` | `map<string, string>` | `requestHeaders` |

### net::LoadTimingInfo

Defined in `net/base/load_timing_info.h`. Contains:

```
request_start          (base::TimeTicks) — time zero for all deltas
connect_timing:
  domain_lookup_start  (base::TimeTicks)
  domain_lookup_end    (base::TimeTicks)
  connect_start        (base::TimeTicks)
  connect_end          (base::TimeTicks)
  ssl_start            (base::TimeTicks)
  ssl_end              (base::TimeTicks)
send_start             (base::TimeTicks)
send_end               (base::TimeTicks)
receive_headers_start  (base::TimeTicks)
receive_headers_end    (base::TimeTicks)
```

`base::TimeTicks` is a monotonic clock. Null ticks (`.is_null() == true`) mean the phase didn't happen (e.g., no DNS for cached requests, no SSL for HTTP). The `timeDeltaMs()` helper returns 0 for null ticks.

### network::mojom::RequestDestination

Defined in `services/network/public/mojom/fetch_api.mojom.h`. The header is included in `web_contents_delegate_qt.cpp` at the top of the file. Full enum has ~28 values; `requestDestinationToType()` maps 16 of them to strings with a `default` → `"other"` catch-all.

---

## Build

The C++ files are part of the QtWebEngine core library:

```
qtwebengine/src/core/CMakeLists.txt:140
    net/network_request_buffer.cpp net/network_request_buffer.h
```

Build after C++ changes:
```bash
./install.sh --dirty    # incremental C++ rebuild + SIP + Python install
```

Build after Python-only changes:
```bash
./install.sh            # skips C++ if commit unchanged
```

C++ changes require a qutebrowser restart to take effect (the library is loaded at startup).

---

## What Detail Provides vs What's Missing

The `detail` subcommand now combines C++ metadata with JS-fetched response data:

**Available per request:**

| Field | Source |
|-------|--------|
| `id`, `url`, `originalUrl`, `method`, `status`, `type`, `mimeType` | C++ NetworkRequestBuffer |
| `cached`, `netError`, `rawBodyBytes`, `totalReceivedBytes` | C++ NetworkRequestBuffer |
| `remoteEndpoint` (ip:port) | C++ NetworkRequestBuffer |
| `timing` (dns, tcp, tls, ttfb breakdowns) | C++ NetworkRequestBuffer |
| `responseHeaders` | JS `fetch({cache: "force-cache"}).headers` |
| `body` (full response text) | JS `fetch({cache: "force-cache"}).text()` |

**Available for sub-resource requests only:**

| Field | Source | Note |
|-------|--------|------|
| `requestHeaders` | C++ NetworkRequestBuffer (from Mojo IPC `request_headers`) | Only populated for sub-resource requests (scripts, stylesheets, images, fonts, fetch/XHR). Document/navigation requests have an empty map — their headers are assembled in the browser process via `BeginNavigationParams` and not forwarded to the renderer's `ResourceLoadComplete` callback. Headers include blink-set values like Accept, User-Agent, sec-ch-ua, etc. Network-service-added headers (Accept-Encoding, Host) are NOT included. |

**Not available** (would require C++ changes or DevTools bridge):

| Feature | Why | Workaround |
|---------|-----|------------|
| Request headers (document requests) | Navigation headers assembled in browser process, not forwarded to renderer | Would need `NavigationHandle` or DevTools `Network.requestWillBeSentExtraInfo` |
| Request body (POST payloads) | Not captured by `ResourceLoadComplete` | None — data doesn't reach the observer |
| `set-cookie` headers | Stripped by `fetch().headers` per spec | Would need C++ `HttpResponseHeaders` interception |
| Redirect chain (individual hops) | Only `url` vs `originalUrl` captured | Would need `NavigationHandle` redirect tracking |
| Initiator (what triggered the request) | Not in `ResourceLoadComplete` | Would need `Network.requestWillBeSent` |
| WebSocket frames | Separate protocol, not HTTP cache | Stub exists in C++ (`"ws_frames"` query type) |
| Priority | Not captured | Would need `Network.resourceChangedPriority` |

**Fetch limitations** — the JS `fetch({cache: "force-cache"})` approach can't retrieve:
- Cross-origin resources without CORS headers → `bodyError: "Failed to fetch"`
- POST responses (can't replay from cache)
- `no-store` responses (cache directive prevents retrieval)
- Binary resources produce garbled text via `r.text()` (harmless but not useful)

The C++ dispatcher still has stub handlers for `"body"`, `"headers"`, `"cookies"`, and `"ws_frames"` query types that return explicit error messages.
