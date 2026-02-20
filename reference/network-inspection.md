# Network Inspection

Query network requests made by any tab from the command line. Each tab gets a `network.sh` script in its runtime directory that saves JSON data to a file and prints a pretty summary to stdout — just like `snapshot-dom.sh` saves the DOM to `dom.html`.

## Quick Start

```bash
# List all requests for tab 0
~/.runtime/qutebrowser-yeyito/runtime/tabs/0/network.sh

# Same thing (explicit subcommand)
~/.runtime/qutebrowser-yeyito/runtime/tabs/0/network.sh list

# Get detail for a specific request (use the id from list output)
~/.runtime/qutebrowser-yeyito/runtime/tabs/0/network.sh detail 685216

# Only errors (status >= 400 or network failures)
~/.runtime/qutebrowser-yeyito/runtime/tabs/0/network.sh list --errors

# Only fetch/XHR requests
~/.runtime/qutebrowser-yeyito/runtime/tabs/0/network.sh list --type fetch

# URL pattern match (jq regex)
~/.runtime/qutebrowser-yeyito/runtime/tabs/0/network.sh list --url "api/v2"

# Combine filters
~/.runtime/qutebrowser-yeyito/runtime/tabs/0/network.sh list --type script --url "cdn"
```

## Subcommands

### `list` (default)

Saves all captured requests to `network.json` and prints a summary.

```bash
network.sh list [--errors] [--type <type>] [--url <pattern>] [--timeout <s>]
```

Stdout:
```
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  Network data saved to network.json
  25.7KB │ 89 requests
  1 document, 7 fetch, 19 font, 10 image, 45 script, 7 stylesheet
  1 request(s) with errors
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
```

The full JSON is in `network.json`:
```json
{
  "requests": [
    {
      "id": 685216,
      "url": "https://example.com/",
      "method": "GET",
      "status": 200,
      "type": "document",
      "mimeType": "text/html",
      "size": 52341,
      "cached": false
    }
  ],
  "count": 89
}
```

The `netError` field only appears when non-zero (blocked requests, DNS failures, etc. have status 0 and a `netError` code).

### `detail <request_id>`

Saves full request details to `request-{id}.json` (per-request file) and prints a summary with timing, body size, and response header count.

```bash
network.sh detail 685216
```

Stdout:
```
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  Request saved to request-685216.json
  163.7KB │ GET 200 document
  https://example.com/engineering/effective-harnesses-fo...
  139.3KB body │ 16 response headers
  93.184.216.34:443
  dns 80ms │ tcp 70ms │ tls 56ms │ ttfb 25ms
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
```

Cross-origin resources without CORS show a yellow error instead of body stats:
```
  body: Failed to fetch
```

The full JSON is in `request-685216.json`:
```json
{
  "id": 685216,
  "url": "https://example.com/",
  "originalUrl": "https://example.com/",
  "method": "GET",
  "status": 200,
  "type": "document",
  "mimeType": "text/html",
  "cached": false,
  "netError": 0,
  "rawBodyBytes": 52341,
  "totalReceivedBytes": 53102,
  "remoteEndpoint": "93.184.216.34:443",
  "timing": {
    "dnsStartMs": 20.4,
    "dnsEndMs": 98.1,
    "connectStartMs": 98.2,
    "connectEndMs": 171.5,
    "sslStartMs": 118.3,
    "sslEndMs": 171.4,
    "sendStartMs": 171.8,
    "sendEndMs": 172.1,
    "receiveHeadersStartMs": 305.6,
    "receiveHeadersEndMs": 306.2
  },
  "responseHeaders": {
    "content-type": "text/html; charset=utf-8",
    "content-encoding": "br",
    "cache-control": "private, no-cache",
    "server": "cloudflare"
  },
  "body": "<!doctype html>\n<html>..."
}
```

All timing values are in milliseconds relative to `request_start` (time zero). Cached requests have all-zero timing. The timing breakdown:

| Phase | Fields | What it measures |
|-------|--------|-----------------|
| DNS | `dnsStartMs` → `dnsEndMs` | Domain name resolution |
| TCP | `connectStartMs` → `connectEndMs` | TCP handshake |
| TLS | `sslStartMs` → `sslEndMs` | TLS negotiation (within connect) |
| Send | `sendStartMs` → `sendEndMs` | Request bytes sent |
| TTFB | `sendEndMs` → `receiveHeadersStartMs` | Server processing (time to first byte) |
| Headers | `receiveHeadersStartMs` → `receiveHeadersEndMs` | Response headers received |

The response body and headers are fetched via `fetch(url, {cache: "force-cache"})` in the page's JS context. This pulls from the browser's HTTP cache with no network round-trip. When the fetch fails, the C++ metadata is still saved but with a `bodyError` field instead of `body`/`responseHeaders`:

```json
{
  "id": 27420,
  "url": "https://cdn.example.com/image.svg",
  "status": 200,
  "type": "image",
  "bodyError": "Failed to fetch"
}
```

Common reasons for `bodyError`:
- **Cross-origin without CORS** — resources loaded via `<img>`, `<link>`, etc. that lack CORS headers
- **POST requests** — can't replay from cache
- **`no-store` responses** — cache directive prevents retrieval
- **Binary resources** — `r.text()` succeeds but produces garbled output (large file, not useful as text)

### `body <request_id>`

Not yet implemented (the `detail` subcommand now includes the body). Returns:
```json
{"error": "body query requires DevTools bridge (not yet implemented)"}
```

### `ws <request_id>`

Not yet implemented. Returns:
```json
{"error": "WebSocket frame query requires DevTools bridge (not yet implemented)"}
```

Requires a future DevTools `Network.webSocketFrame*` event integration.

## Filters

Filters are applied client-side via `jq` after the result arrives. They only work with the `list` subcommand and require `jq` to be installed.

| Flag | Effect |
|------|--------|
| `--errors` | Keep only requests with `status >= 400` or `status == 0` (network failures) |
| `--type <type>` | Keep only requests matching the resource type exactly |
| `--url <pattern>` | Keep only requests whose URL matches the jq regex pattern |

Filters can be combined. They are applied in order: errors → type → url.

## Resource Types

The `type` field in request entries maps from Chromium's `RequestDestination` enum:

| Type | What it is |
|------|-----------|
| `document` | Main page HTML |
| `frame` | iframe/frame HTML |
| `script` | JavaScript |
| `stylesheet` | CSS |
| `image` | Images (png, jpg, svg, etc.) |
| `font` | Web fonts |
| `fetch` | XHR / fetch() / beacon / ping |
| `media` | Audio/video |
| `track` | WebVTT subtitle tracks |
| `manifest` | Web app manifests |
| `worker` | Web/Shared/Service workers |
| `object` | Plugins (embed/object elements) |
| `report` | CSP/reporting API reports |
| `xslt` | XSLT stylesheets |
| `other` | Anything else |

## Options

| Option | Default | Description |
|--------|---------|-------------|
| `--timeout <seconds>` | 5 | How long to wait for qutebrowser to respond |
| `-h`, `--help` | — | Show usage |

## Buffer Behavior

- The buffer holds up to **1000 requests** per tab. Oldest entries are evicted when full.
- The buffer **clears on every navigation** (new page = fresh buffer). This mirrors Chrome DevTools behavior.
- Data is **in-memory only** — zero disk I/O until you query it.
- Each request is captured when its resource load completes (not when it starts). In-flight requests are not visible.

## Data Source

The data comes from Chromium's `ResourceLoadComplete` callback, which fires for every completed resource load. This is the same data source Chrome uses for its Network panel resource summary.

**Available via C++ buffer**: URL, method, status, resource type, MIME type, body size, total bytes, cache status, network error code, DNS/TCP/TLS/send/TTFB timing, remote IP:port, original URL (before redirects).

**Available via JS fetch (detail only)**: response headers, response body text (for same-origin and CORS-enabled resources).

**Not available**: request headers, request body (POST payloads), `set-cookie` headers (stripped by `fetch().headers` per spec), cookies, WebSocket frames, request initiator, redirect chain (individual hops).

## Error Handling

| Error | Cause |
|-------|-------|
| `Error: IPC socket not found (is qutebrowser running?)` | Qutebrowser isn't running, or wrong basedir |
| `Error: network query timed out (5s)` | Qutebrowser didn't respond in time. Use `--timeout 10` |
| `Error: <subcmd> requires a <request_id> argument` | Missing request ID for detail/body/ws |
| `{"error":"request not found","requestId":0}` | The request ID doesn't exist in the buffer (navigated away?) |
| `{"error":"not initialized"}` | Tab's WebEngine page hasn't initialized yet |

## Reading the Data

The script saves JSON to files. Read them after running the command:

```bash
TAB=~/.runtime/qutebrowser-yeyito/runtime/tabs/0

# Run list, then read the data
$TAB/network.sh list
cat $TAB/network.json | jq '.requests | sort_by(-.size) | .[0:5]'

# Run detail, then read it (note: per-request file)
$TAB/network.sh detail 685216
cat $TAB/request-685216.json | jq '.timing'
cat $TAB/request-685216.json | jq '.responseHeaders'
cat $TAB/request-685216.json | jq '.body' -r | head -20

# Run with filter, then read filtered data
$TAB/network.sh list --type fetch
cat $TAB/network.json | jq '.requests[] | {url, status, size}'
```

## Practical Examples

```bash
TAB=~/.runtime/qutebrowser-yeyito/runtime/tabs/0

# Find the largest requests
$TAB/network.sh list && cat $TAB/network.json | jq '.requests | sort_by(-.size) | .[0:5]'

# Get detail + body for the main document
$TAB/network.sh list --type document
ID=$(cat $TAB/network.json | jq '.requests[0].id')
$TAB/network.sh detail $ID
cat $TAB/request-$ID.json | jq '.responseHeaders'

# Read the response body
cat $TAB/request-$ID.json | jq '.body' -r | head -50

# Find all API calls
$TAB/network.sh list --url "api/"
cat $TAB/network.json | jq '.requests[] | {url, status, size}'

# Check what fonts loaded
$TAB/network.sh list --type font
cat $TAB/network.json | jq '.requests[] | .url'

# Total bytes transferred
$TAB/network.sh list
cat $TAB/network.json | jq '[.requests[].size] | add'
```
