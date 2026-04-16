# SPDX-FileCopyrightText: Florian Bruhin (The Compiler) <mail@qutebrowser.org>
#
# SPDX-License-Identifier: GPL-3.0-or-later

"""Live network capture via Chromium's DevTools Protocol.

This augments qutebrowser's existing network tooling with data that isn't
available from ResourceLoadComplete alone, most importantly:

- full request headers from *ExtraInfo* events (Cookie, Origin, Referer,
  Sec-Fetch-*, X-*, ...)
- POST request bodies via Network.getRequestPostData
- exact response bodies via Network.getResponseBody
- append-only per-request capture files for important requests

The monitor runs a small asyncio event loop in a background thread and keeps one
DevTools websocket attached to each tracked tab.
"""

from __future__ import annotations

import asyncio
import base64
import json
import threading
import time
from pathlib import Path
from typing import Any, Optional

try:
    import websockets
except ImportError:  # pragma: no cover - exercised only when dependency missing
    websockets = None  # type: ignore[assignment]

from qutebrowser.utils import log
from qutebrowser.misc import remotedebugging


_MAX_LIVE_REQUESTS = 1000
_MAX_CAPTURE_BODY_BYTES = 1024 * 1024
_CONNECT_RETRY_SECONDS = 1.0
_COMMAND_TIMEOUT_SECONDS = 5.0


def _stringify(value: Any) -> str:
    if value is None:
        return ''
    if isinstance(value, str):
        return value
    if isinstance(value, bool):
        return 'true' if value else 'false'
    return str(value)


def _headers_to_dict(headers: Optional[dict[str, Any]]) -> dict[str, str]:
    if not headers:
        return {}
    return {str(key): _stringify(value) for key, value in headers.items()}


def _normalize_type(value: Optional[str]) -> str:
    mapping = {
        'Document': 'document',
        'Stylesheet': 'stylesheet',
        'Script': 'script',
        'Image': 'image',
        'Media': 'media',
        'Font': 'font',
        'Fetch': 'fetch',
        'XHR': 'fetch',
        'EventSource': 'fetch',
        'Manifest': 'manifest',
        'Ping': 'fetch',
        'Preflight': 'fetch',
        'CSPViolationReport': 'report',
        'WebSocket': 'websocket',
    }
    return mapping.get(value or '', 'other')


def _normalize_timing(timing: Optional[dict[str, Any]]) -> dict[str, float]:
    timing = timing or {}

    def _value(name: str) -> float:
        try:
            val = float(timing.get(name, 0))
        except (TypeError, ValueError):
            return 0
        return 0 if val < 0 else val

    return {
        'dnsStartMs': _value('dnsStart'),
        'dnsEndMs': _value('dnsEnd'),
        'connectStartMs': _value('connectStart'),
        'connectEndMs': _value('connectEnd'),
        'sslStartMs': _value('sslStart'),
        'sslEndMs': _value('sslEnd'),
        'sendStartMs': _value('sendStart'),
        'sendEndMs': _value('sendEnd'),
        'receiveHeadersStartMs': _value('receiveHeadersStart'),
        'receiveHeadersEndMs': _value('receiveHeadersEnd'),
    }


def _infer_raw_body_bytes(record: dict[str, Any]) -> int:
    headers = record.get('responseHeaders') or {}
    content_length = headers.get('Content-Length') or headers.get('content-length')
    if content_length:
        try:
            return int(content_length)
        except (TypeError, ValueError):
            pass

    body = record.get('body')
    if body is None:
        return 0

    if record.get('bodyBase64Encoded'):
        try:
            return len(base64.b64decode(body))
        except Exception:  # pragma: no cover - defensive
            return 0

    return len(body.encode('utf-8'))


def _cookie_header_from_associated(associated: list[dict[str, Any]]) -> str:
    pairs = []
    for entry in associated:
        cookie = entry.get('cookie') or {}
        blocked_reasons = entry.get('blockedReasons') or []
        if blocked_reasons:
            continue
        name = cookie.get('name')
        value = cookie.get('value')
        if name is None or value is None:
            continue
        pairs.append(f'{name}={value}')
    return '; '.join(pairs)


class _TabSession:

    def __init__(self, tab_id: str, tab_dir: Path, ws_url: str):
        self.tab_id = tab_id
        self.tab_dir = tab_dir
        self.ws_url = ws_url
        self.capture_dir = tab_dir / 'network-captures'
        self.capture_dir.mkdir(parents=True, exist_ok=True)

        self._lock = threading.RLock()
        self._closed = False
        self._connected = False
        self._connect_error: Optional[str] = None
        self._send_lock: Optional[asyncio.Lock] = None
        self._websocket = None
        self._pending: dict[int, asyncio.Future[Any]] = {}
        self._command_id = 0
        self._task: Optional[asyncio.Task[Any]] = None

        self._next_request_id = 1
        self._records_by_cdp: dict[str, dict[str, Any]] = {}
        self._live_records_by_id: dict[int, dict[str, Any]] = {}
        self._live_ids: list[int] = []
        self._persisted_paths: dict[int, str] = {}

    def start(self, loop: asyncio.AbstractEventLoop) -> None:
        if self._task is None:
            self._task = loop.create_task(self._run())

    def close(self) -> None:
        with self._lock:
            self._closed = True
        if self._task is not None:
            self._task.cancel()

    def reset_live_buffer(self) -> None:
        with self._lock:
            self._records_by_cdp.clear()
            self._live_records_by_id.clear()
            self._live_ids.clear()

    def capture_dir_path(self) -> str:
        return str(self.capture_dir)

    def persisted_count(self) -> int:
        with self._lock:
            return len(self._persisted_paths)

    def query_list(self) -> dict[str, Any]:
        with self._lock:
            requests = []
            for request_id in self._live_ids:
                record = self._live_records_by_id.get(request_id)
                if not record or not record.get('finished'):
                    continue
                requests.append({
                    'id': request_id,
                    'url': record.get('url', ''),
                    'method': record.get('method', ''),
                    'status': record.get('status', 0),
                    'type': record.get('type', 'other'),
                    'mimeType': record.get('mimeType', ''),
                    'size': record.get('rawBodyBytes', 0),
                    'cached': bool(record.get('cached', False)),
                    **({'netError': record['netError']} if record.get('netError') else {}),
                })

            result: dict[str, Any] = {
                'requests': requests,
                'count': len(requests),
                'captureDir': str(self.capture_dir),
                'persistedCount': len(self._persisted_paths),
                'source': 'devtools',
            }
            if not self._connected and self._connect_error and not requests:
                result['warning'] = self._connect_error
            return result

    def query_detail(self, request_id: str | int) -> dict[str, Any]:
        try:
            numeric_id = int(request_id)
        except (TypeError, ValueError):
            return {'error': 'request not found', 'requestId': request_id}

        record: Optional[dict[str, Any]]
        with self._lock:
            record = self._live_records_by_id.get(numeric_id)
            persisted_path = self._persisted_paths.get(numeric_id)

        if record is None and persisted_path:
            try:
                return json.loads(Path(persisted_path).read_text())
            except (OSError, json.JSONDecodeError):
                pass

        if record is None:
            return {'error': 'request not found', 'requestId': numeric_id}

        event = record.get('_artifacts_event')
        if event is not None and record.get('finished'):
            event.wait(_COMMAND_TIMEOUT_SECONDS)

        return self._public_record(record)

    def query_body(self, request_id: str | int) -> tuple[Optional[bytes], Optional[str]]:
        detail = self.query_detail(request_id)
        if 'error' in detail:
            return None, json.dumps(detail, indent=2) + '\n'

        body = detail.get('body')
        if body is None:
            err = detail.get('bodyError', 'response body not captured')
            return None, json.dumps({'error': err}, indent=2) + '\n'

        if detail.get('bodyBase64Encoded'):
            try:
                return base64.b64decode(body), None
            except Exception as exc:  # pragma: no cover - defensive
                return None, json.dumps({'error': str(exc)}, indent=2) + '\n'

        return body.encode('utf-8'), None

    def _public_record(self, record: dict[str, Any]) -> dict[str, Any]:
        result: dict[str, Any] = {
            'id': record['id'],
            'cdpRequestId': record['cdpRequestId'],
            'url': record.get('url', ''),
            'originalUrl': record.get('originalUrl', ''),
            'method': record.get('method', ''),
            'status': record.get('status', 0),
            'type': record.get('type', 'other'),
            'mimeType': record.get('mimeType', ''),
            'cached': bool(record.get('cached', False)),
            'netError': record.get('netError', 0),
            'rawBodyBytes': record.get('rawBodyBytes', 0),
            'totalReceivedBytes': record.get('totalReceivedBytes', 0),
            'timing': record.get('timing', _normalize_timing(None)),
        }

        if record.get('remoteEndpoint'):
            result['remoteEndpoint'] = record['remoteEndpoint']
        if record.get('requestHeaders'):
            result['requestHeaders'] = dict(record['requestHeaders'])
        if record.get('responseHeaders'):
            result['responseHeaders'] = dict(record['responseHeaders'])
        if record.get('requestBody') is not None:
            result['requestBody'] = record['requestBody']
        if record.get('requestBodyError'):
            result['requestBodyError'] = record['requestBodyError']
        if record.get('requestCookies'):
            result['requestCookies'] = list(record['requestCookies'])
        if record.get('body') is not None:
            result['body'] = record['body']
            result['bodyBase64Encoded'] = bool(record.get('bodyBase64Encoded', False))
        if record.get('bodyError'):
            result['bodyError'] = record['bodyError']
        if record.get('persistedPath'):
            result['persistedPath'] = record['persistedPath']
        if record.get('wallTime'):
            result['wallTime'] = record['wallTime']
        if record.get('responseHeadersText'):
            result['responseHeadersText'] = record['responseHeadersText']
        return result

    def _new_record(self, cdp_request_id: str) -> dict[str, Any]:
        record = {
            'id': self._next_request_id,
            'cdpRequestId': cdp_request_id,
            'url': '',
            'originalUrl': '',
            'method': '',
            'status': 0,
            'type': 'other',
            'mimeType': '',
            'cached': False,
            'netError': 0,
            'rawBodyBytes': 0,
            'totalReceivedBytes': 0,
            'remoteEndpoint': '',
            'requestHeaders': {},
            'requestCookies': [],
            'responseHeaders': {},
            'responseHeadersText': '',
            'requestBody': None,
            'requestBodyError': None,
            'body': None,
            'bodyBase64Encoded': False,
            'bodyError': None,
            'timing': _normalize_timing(None),
            'finished': False,
            'persistedPath': None,
            'wallTime': None,
            '_artifacts_event': threading.Event(),
        }
        self._next_request_id += 1
        self._records_by_cdp[cdp_request_id] = record
        self._live_records_by_id[record['id']] = record
        self._live_ids.append(record['id'])

        while len(self._live_ids) > _MAX_LIVE_REQUESTS:
            evicted_id = self._live_ids.pop(0)
            evicted = self._live_records_by_id.pop(evicted_id, None)
            if evicted is not None:
                self._records_by_cdp.pop(evicted.get('cdpRequestId', ''), None)

        return record

    def _get_or_create_record(self, cdp_request_id: str) -> dict[str, Any]:
        with self._lock:
            record = self._records_by_cdp.get(cdp_request_id)
            if record is None:
                record = self._new_record(cdp_request_id)
            return record

    def _mark_artifacts_ready(self, record: dict[str, Any]) -> None:
        event = record.get('_artifacts_event')
        if event is not None:
            event.set()

    def _should_capture_request_body(self, record: dict[str, Any]) -> bool:
        return record.get('method', '').upper() not in {'', 'GET', 'HEAD', 'OPTIONS'}

    def _should_capture_response_body(self, record: dict[str, Any]) -> bool:
        total = int(record.get('totalReceivedBytes') or 0)
        if total > _MAX_CAPTURE_BODY_BYTES:
            return False

        method = record.get('method', '').upper()
        if method not in {'', 'GET', 'HEAD', 'OPTIONS'}:
            return True

        if int(record.get('status') or 0) >= 400:
            return True

        if record.get('type') == 'fetch':
            return True

        return False

    def _should_persist(self, record: dict[str, Any]) -> bool:
        method = record.get('method', '').upper()
        if method not in {'', 'GET', 'HEAD', 'OPTIONS'}:
            return True
        if int(record.get('status') or 0) >= 400:
            return True
        return record.get('type') == 'fetch'

    def _persist_record(self, record: dict[str, Any]) -> None:
        if record.get('persistedPath'):
            path = Path(record['persistedPath'])
        else:
            timestamp = time.strftime('%Y%m%d-%H%M%S')
            path = self.capture_dir / f'{timestamp}-request-{record["id"]}.json'

        with self._lock:
            record['persistedPath'] = str(path)
            self._persisted_paths[record['id']] = str(path)
            data = self._public_record(record)

        path.write_text(json.dumps(data, indent=2) + '\n')

    async def _send_command(self, method: str, params: Optional[dict[str, Any]] = None) -> dict[str, Any]:
        if self._websocket is None or self._send_lock is None:
            raise RuntimeError('DevTools websocket is not connected')

        loop = asyncio.get_running_loop()
        future: asyncio.Future[Any] = loop.create_future()

        async with self._send_lock:
            self._command_id += 1
            command_id = self._command_id
            self._pending[command_id] = future
            payload = {'id': command_id, 'method': method}
            if params:
                payload['params'] = params
            await self._websocket.send(json.dumps(payload))

        result = await asyncio.wait_for(future, timeout=_COMMAND_TIMEOUT_SECONDS)
        return result

    async def _capture_artifacts(self, cdp_request_id: str) -> None:
        with self._lock:
            record = self._records_by_cdp.get(cdp_request_id)
        if record is None:
            return

        request_body_needed = self._should_capture_request_body(record)
        response_body_needed = self._should_capture_response_body(record)

        if request_body_needed and record.get('requestBody') is None:
            try:
                result = await self._send_command('Network.getRequestPostData', {
                    'requestId': cdp_request_id,
                })
                with self._lock:
                    record['requestBody'] = result.get('postData')
            except Exception as exc:
                with self._lock:
                    record['requestBodyError'] = _stringify(exc)

        if response_body_needed:
            try:
                result = await self._send_command('Network.getResponseBody', {
                    'requestId': cdp_request_id,
                })
                with self._lock:
                    record['body'] = result.get('body')
                    record['bodyBase64Encoded'] = bool(result.get('base64Encoded', False))
                    record['rawBodyBytes'] = _infer_raw_body_bytes(record)
            except Exception as exc:
                with self._lock:
                    if int(record.get('totalReceivedBytes') or 0) > _MAX_CAPTURE_BODY_BYTES:
                        record['bodyError'] = (
                            f'response body skipped ({record.get("totalReceivedBytes", 0)} bytes exceeds '
                            f'{_MAX_CAPTURE_BODY_BYTES} byte limit)')
                    else:
                        record['bodyError'] = _stringify(exc)
        else:
            with self._lock:
                if int(record.get('totalReceivedBytes') or 0) > _MAX_CAPTURE_BODY_BYTES:
                    record['bodyError'] = (
                        f'response body skipped ({record.get("totalReceivedBytes", 0)} bytes exceeds '
                        f'{_MAX_CAPTURE_BODY_BYTES} byte limit)')

        with self._lock:
            if not record.get('rawBodyBytes'):
                record['rawBodyBytes'] = _infer_raw_body_bytes(record)
            should_persist = self._should_persist(record)
        self._mark_artifacts_ready(record)

        if should_persist:
            try:
                self._persist_record(record)
            except OSError as exc:
                log.network.warning(f'Failed to persist network capture for tab {self.tab_id}: {exc}')

    def _handle_request_will_be_sent(self, params: dict[str, Any]) -> None:
        cdp_request_id = params.get('requestId')
        if not cdp_request_id:
            return

        request = params.get('request') or {}
        record = self._get_or_create_record(cdp_request_id)
        with self._lock:
            if not record['originalUrl']:
                record['originalUrl'] = request.get('url', '')
            record['url'] = request.get('url', '')
            record['method'] = request.get('method', '')
            record['type'] = _normalize_type(params.get('type'))
            if request.get('headers') and not record['requestHeaders']:
                record['requestHeaders'] = _headers_to_dict(request.get('headers'))
            if request.get('hasPostData') and request.get('postData') is not None:
                record['requestBody'] = request.get('postData')
            if params.get('wallTime') is not None:
                record['wallTime'] = params.get('wallTime')

    def _handle_request_will_be_sent_extra_info(self, params: dict[str, Any]) -> None:
        cdp_request_id = params.get('requestId')
        if not cdp_request_id:
            return

        record = self._get_or_create_record(cdp_request_id)
        headers = _headers_to_dict(params.get('headers'))
        associated = params.get('associatedCookies') or []

        with self._lock:
            if headers:
                record['requestHeaders'] = headers
            if associated:
                record['requestCookies'] = associated
                if 'Cookie' not in record['requestHeaders'] and 'cookie' not in record['requestHeaders']:
                    cookie_header = _cookie_header_from_associated(associated)
                    if cookie_header:
                        record['requestHeaders']['Cookie'] = cookie_header

    def _handle_response_received(self, params: dict[str, Any]) -> None:
        cdp_request_id = params.get('requestId')
        if not cdp_request_id:
            return

        response = params.get('response') or {}
        record = self._get_or_create_record(cdp_request_id)
        remote_ip = response.get('remoteIPAddress')
        remote_port = response.get('remotePort')
        remote_endpoint = ''
        if remote_ip and remote_port is not None:
            remote_endpoint = f'{remote_ip}:{remote_port}'
        elif remote_ip:
            remote_endpoint = remote_ip

        with self._lock:
            record['status'] = int(response.get('status', 0) or 0)
            record['mimeType'] = response.get('mimeType', '')
            record['cached'] = bool(
                response.get('fromDiskCache') or response.get('fromPrefetchCache'))
            if remote_endpoint:
                record['remoteEndpoint'] = remote_endpoint
            if response.get('timing'):
                record['timing'] = _normalize_timing(response.get('timing'))
            if response.get('headers') and not record['responseHeaders']:
                record['responseHeaders'] = _headers_to_dict(response.get('headers'))
            if params.get('type'):
                record['type'] = _normalize_type(params.get('type'))

    def _handle_response_received_extra_info(self, params: dict[str, Any]) -> None:
        cdp_request_id = params.get('requestId')
        if not cdp_request_id:
            return

        record = self._get_or_create_record(cdp_request_id)
        headers = _headers_to_dict(params.get('headers'))

        with self._lock:
            if headers:
                record['responseHeaders'] = headers
            if params.get('headersText'):
                record['responseHeadersText'] = params.get('headersText')
            if params.get('statusCode') is not None:
                record['status'] = int(params.get('statusCode') or 0)

    def _handle_loading_finished(self, params: dict[str, Any]) -> None:
        cdp_request_id = params.get('requestId')
        if not cdp_request_id:
            return

        record = self._get_or_create_record(cdp_request_id)
        with self._lock:
            encoded = int(params.get('encodedDataLength', 0) or 0)
            record['totalReceivedBytes'] = encoded
            if not record['rawBodyBytes']:
                record['rawBodyBytes'] = encoded
            record['finished'] = True

        asyncio.create_task(self._capture_artifacts(cdp_request_id))

    def _handle_loading_failed(self, params: dict[str, Any]) -> None:
        cdp_request_id = params.get('requestId')
        if not cdp_request_id:
            return

        record = self._get_or_create_record(cdp_request_id)
        with self._lock:
            record['finished'] = True
            record['status'] = 0
            record['netError'] = -1
            record['bodyError'] = params.get('errorText') or 'request failed'
        self._mark_artifacts_ready(record)

        with self._lock:
            should_persist = self._should_persist(record)
        if should_persist:
            try:
                self._persist_record(record)
            except OSError as exc:
                log.network.warning(f'Failed to persist failed request for tab {self.tab_id}: {exc}')

    async def _on_message(self, payload: dict[str, Any]) -> None:
        if 'id' in payload:
            future = self._pending.pop(payload['id'], None)
            if future is None:
                return
            if 'error' in payload:
                future.set_exception(RuntimeError(payload['error'].get('message', 'DevTools command failed')))
            else:
                future.set_result(payload.get('result', {}))
            return

        method = payload.get('method')
        params = payload.get('params') or {}
        if method == 'Network.requestWillBeSent':
            self._handle_request_will_be_sent(params)
        elif method == 'Network.requestWillBeSentExtraInfo':
            self._handle_request_will_be_sent_extra_info(params)
        elif method == 'Network.responseReceived':
            self._handle_response_received(params)
        elif method == 'Network.responseReceivedExtraInfo':
            self._handle_response_received_extra_info(params)
        elif method == 'Network.loadingFinished':
            self._handle_loading_finished(params)
        elif method == 'Network.loadingFailed':
            self._handle_loading_failed(params)

    async def _read_messages(self, websocket) -> None:
        async for raw in websocket:
            payload = json.loads(raw)
            await self._on_message(payload)

    async def _run(self) -> None:
        if websockets is None:
            with self._lock:
                self._connect_error = 'python websockets package is not installed'
            return

        while True:
            with self._lock:
                if self._closed:
                    return

            reader_task: Optional[asyncio.Task[Any]] = None
            try:
                async with websockets.connect(
                        self.ws_url,
                        open_timeout=_COMMAND_TIMEOUT_SECONDS,
                        close_timeout=1,
                        max_size=(2 * _MAX_CAPTURE_BODY_BYTES)) as websocket:
                    with self._lock:
                        self._websocket = websocket
                        self._connected = True
                        self._connect_error = None
                        self._send_lock = asyncio.Lock()

                    reader_task = asyncio.create_task(self._read_messages(websocket))

                    await self._send_command('Network.enable', {
                        'maxPostDataSize': _MAX_CAPTURE_BODY_BYTES,
                    })
                    log.network.debug(f'DevTools network capture attached to tab {self.tab_id}')

                    await reader_task
            except asyncio.CancelledError:
                if reader_task is not None:
                    reader_task.cancel()
                return
            except Exception as exc:
                with self._lock:
                    self._connect_error = _stringify(exc)
                    self._connected = False
                    self._websocket = None
                    self._send_lock = None
                for future in list(self._pending.values()):
                    if not future.done():
                        future.set_exception(RuntimeError(self._connect_error))
                self._pending.clear()

                if reader_task is not None and not reader_task.done():
                    reader_task.cancel()

                log.network.debug(
                    f'DevTools network capture disconnected for tab {self.tab_id}: {exc}')
                await asyncio.sleep(_CONNECT_RETRY_SECONDS)
            else:
                with self._lock:
                    self._connected = False
                    self._websocket = None
                    self._send_lock = None
                if not self._closed:
                    await asyncio.sleep(_CONNECT_RETRY_SECONDS)


class DevToolsNetworkMonitor:

    def __init__(self) -> None:
        self._loop = asyncio.new_event_loop()
        self._thread = threading.Thread(
            target=self._run_loop,
            name='qutebrowser-devtools-network',
            daemon=True,
        )
        self._thread.start()
        self._lock = threading.RLock()
        self._sessions: dict[str, _TabSession] = {}

    def _run_loop(self) -> None:
        asyncio.set_event_loop(self._loop)
        self._loop.run_forever()

    def is_available(self) -> bool:
        return (websockets is not None and
                remotedebugging.current_remote_debugging_address() is not None)

    def register_tab(self, tab_id: str, page_devtools_id: str, tab_dir: Path) -> None:
        address = remotedebugging.current_remote_debugging_address()
        if not page_devtools_id or address is None or websockets is None:
            return

        host, port = address
        ws_url = f'ws://{host}:{port}/devtools/page/{page_devtools_id}'

        with self._lock:
            old = self._sessions.pop(tab_id, None)
            if old is not None:
                old.close()
            session = _TabSession(tab_id=tab_id, tab_dir=tab_dir, ws_url=ws_url)
            self._sessions[tab_id] = session

        self._loop.call_soon_threadsafe(session.start, self._loop)

        info_path = tab_dir / 'network-capture.info'
        info = {
            'source': 'devtools',
            'wsUrl': ws_url,
            'captureDir': str(session.capture_dir),
        }
        try:
            info_path.write_text(json.dumps(info, indent=2) + '\n')
        except OSError:
            pass

    def unregister_tab(self, tab_id: str) -> None:
        with self._lock:
            session = self._sessions.pop(tab_id, None)
        if session is not None:
            session.close()

    def reset_live_buffer(self, tab_id: str) -> None:
        with self._lock:
            session = self._sessions.get(tab_id)
        if session is not None:
            session.reset_live_buffer()

    def query_list(self, tab_id: str) -> Optional[dict[str, Any]]:
        with self._lock:
            session = self._sessions.get(tab_id)
        if session is None:
            return None
        return session.query_list()

    def query_detail(self, tab_id: str, request_id: str | int) -> Optional[dict[str, Any]]:
        with self._lock:
            session = self._sessions.get(tab_id)
        if session is None:
            return None
        return session.query_detail(request_id)

    def query_body(self, tab_id: str, request_id: str | int) -> Optional[tuple[Optional[bytes], Optional[str]]]:
        with self._lock:
            session = self._sessions.get(tab_id)
        if session is None:
            return None
        return session.query_body(request_id)

    def shutdown(self) -> None:
        with self._lock:
            sessions = list(self._sessions.values())
            self._sessions.clear()
        for session in sessions:
            session.close()
        self._loop.call_soon_threadsafe(self._loop.stop)


_instance: Optional[DevToolsNetworkMonitor] = None


def instance() -> DevToolsNetworkMonitor:
    global _instance
    if _instance is None:
        _instance = DevToolsNetworkMonitor()
    return _instance
