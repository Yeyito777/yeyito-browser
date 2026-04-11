# SPDX-FileCopyrightText: Florian Bruhin (The Compiler) <mail@qutebrowser.org>
#
# SPDX-License-Identifier: GPL-3.0-or-later

"""qute-gm://* scheme handler for Greasemonkey cross-origin requests.

Provides a CSP-bypassing HTTP proxy so that GM_xmlhttpRequest can reach
external URLs even on pages with a restrictive Content-Security-Policy.

The scheme is registered with the ContentSecurityPolicyIgnored flag, so
fetches from page JS to qute-gm:// are never blocked by CSP.
"""

import json
import logging
import ssl
from urllib.request import Request, urlopen, build_opener, HTTPSHandler
from urllib.error import URLError, HTTPError

try:
    import certifi
    _SSL_CTX = ssl.create_default_context(cafile=certifi.where())
except ImportError:
    _SSL_CTX = ssl.create_default_context()
    _SSL_CTX.check_hostname = False
    _SSL_CTX.verify_mode = ssl.CERT_NONE

from qutebrowser.qt.core import (QBuffer, QIODevice, QByteArray, QThread,
                                  pyqtSignal, QObject)
from qutebrowser.qt.webenginecore import (QWebEngineUrlSchemeHandler,
                                           QWebEngineUrlRequestJob,
                                           QWebEngineUrlScheme)

log = logging.getLogger(__name__)

_GM = QByteArray(b'qutegm')


class _FetchWorker(QObject):
    """Runs an HTTP fetch in a background thread."""

    finished = pyqtSignal(dict)

    def __init__(self, spec):
        super().__init__()
        self._spec = spec

    def run(self):
        spec = self._spec
        url = spec.get('url', '')
        method = spec.get('method', 'GET').upper()
        headers = spec.get('headers', {})
        data = spec.get('data')

        try:
            req = Request(url, method=method)
            # Set a browser-like User-Agent by default to avoid Cloudflare blocks
            req.add_header('User-Agent',
                           'Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 '
                           '(KHTML, like Gecko) QtWebEngine/6.9.0 Chrome/130.0.0.0 '
                           'Safari/537.36')
            for k, v in headers.items():
                req.add_header(k, v)

            body = None
            if data is not None:
                body = data.encode('utf-8') if isinstance(data, str) else data

            resp = urlopen(req, body, timeout=30, context=_SSL_CTX)
            resp_body = resp.read()
            resp_headers = '\r\n'.join(
                f'{k}: {v}' for k, v in resp.getheaders()
            )
            result = {
                'status': resp.status,
                'statusText': resp.reason,
                'responseHeaders': resp_headers,
                'finalUrl': resp.url,
                'response': resp_body.decode('latin-1'),  # preserve bytes
            }
        except HTTPError as e:
            body = b''
            try:
                body = e.read()
            except Exception:
                pass
            result = {
                'status': e.code,
                'statusText': str(e.reason),
                'responseHeaders': '\r\n'.join(
                    f'{k}: {v}' for k, v in e.headers.items()
                ),
                'finalUrl': url,
                'response': body.decode('latin-1'),
            }
        except URLError as e:
            result = {
                'error': str(e.reason),
                'status': 0,
                'statusText': str(e.reason),
                'responseHeaders': '',
                'finalUrl': url,
                'response': '',
            }
        except Exception as e:
            result = {
                'error': str(e),
                'status': 0,
                'statusText': str(e),
                'responseHeaders': '',
                'finalUrl': url,
                'response': '',
            }

        self.finished.emit(result)


class GmSchemeHandler(QWebEngineUrlSchemeHandler):
    """Handle qute-gm://* proxy requests for Greasemonkey scripts."""

    def __init__(self, parent=None):
        super().__init__(parent)
        # prevent GC of in-flight workers/threads
        self._pending = []

    def install(self, profile):
        """Install the handler on the given QWebEngineProfile."""
        profile.installUrlSchemeHandler(_GM, self)

    def requestStarted(self, job: QWebEngineUrlRequestJob):
        url = job.requestUrl()
        path = url.path()

        if path != '/fetch':
            job.fail(QWebEngineUrlRequestJob.Error.UrlNotFound)
            return

        # Try POST body first, fall back to query parameter
        spec = None
        body_dev = job.requestBody()
        if body_dev is not None:
            raw = bytes(body_dev.readAll())
            if raw:
                try:
                    spec = json.loads(raw)
                except (json.JSONDecodeError, UnicodeDecodeError):
                    pass

        if spec is None:
            # Fall back to ?spec= query parameter (for GET requests).
            # We must NOT use QUrlQuery because it treats ';' as a separator,
            # which breaks URLs containing semicolons (e.g. Google Fonts).
            # Instead, parse manually from the raw query string.
            from qutebrowser.qt.core import QUrl
            raw_query = url.query(QUrl.ComponentFormattingOption.FullyDecoded)
            prefix = 'spec='
            spec_str = ''
            if raw_query.startswith(prefix):
                spec_str = raw_query[len(prefix):]
            if spec_str:
                try:
                    spec = json.loads(spec_str)
                except (json.JSONDecodeError, UnicodeDecodeError):
                    pass

        if spec is None:
            job.fail(QWebEngineUrlRequestJob.Error.RequestFailed)
            return

        # Spin up a worker thread for the blocking HTTP fetch
        thread = QThread(self)
        worker = _FetchWorker(spec)
        worker.moveToThread(thread)

        entry = (thread, worker, job)
        self._pending.append(entry)

        def on_finished(result):
            try:
                # Job may have been deleted if the tab was closed
                # Check if the job's parent is still alive
                data = json.dumps(result).encode('utf-8')
                buf = QBuffer(parent=self)
                buf.open(QIODevice.OpenModeFlag.WriteOnly)
                buf.write(QByteArray(data))
                buf.seek(0)
                buf.close()
                job.reply(b'application/json', buf)
            except RuntimeError:
                # Job was deleted (tab closed, navigation, etc.)
                pass
            finally:
                thread.quit()
                thread.wait()
                try:
                    self._pending.remove(entry)
                except ValueError:
                    pass

        worker.finished.connect(on_finished)
        thread.started.connect(worker.run)
        thread.start()


def init():
    """Register the qute-gm:// scheme.

    Must be called early, before constructing any QtWebEngine classes.
    """
    if QWebEngineUrlScheme is not None:
        assert not QWebEngineUrlScheme.schemeByName(_GM).name()
        scheme = QWebEngineUrlScheme(_GM)
        scheme.setFlags(
            QWebEngineUrlScheme.Flag.SecureScheme |
            QWebEngineUrlScheme.Flag.ContentSecurityPolicyIgnored |
            QWebEngineUrlScheme.Flag.CorsEnabled |
            QWebEngineUrlScheme.Flag.FetchApiAllowed)
        scheme.setSyntax(QWebEngineUrlScheme.Syntax.Path)
        QWebEngineUrlScheme.registerScheme(scheme)
