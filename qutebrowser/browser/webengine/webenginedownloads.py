# SPDX-FileCopyrightText: Florian Bruhin (The Compiler) <mail@qutebrowser.org>
#
# SPDX-License-Identifier: GPL-3.0-or-later

"""QtWebEngine specific code for downloads."""

import re
import os.path
import shutil
import uuid
import functools

from qutebrowser.qt import machinery
from qutebrowser.qt.core import pyqtSlot, Qt, QUrl, QObject
from qutebrowser.qt.webenginecore import QWebEngineDownloadRequest

from qutebrowser.browser import crx, downloads, pdfjs
from qutebrowser.utils import (debug, usertypes, message, log, objreg, standarddir,
                               urlutils, utils, version)


def _find_tab_for_page(page):
    """Find the AbstractTab whose QWebEnginePage matches the given page.

    Args:
        page: A QWebEnginePage instance (from QWebEngineDownloadRequest.page()).

    Returns:
        The matching AbstractTab, or None if not found.
    """
    try:
        for win_id in objreg.window_registry:
            tabbed_browser = objreg.get('tabbed-browser', scope='window',
                                        window=win_id)
            for tab in tabbed_browser.widgets():
                try:
                    if tab._widget.page() is page:
                        return tab
                except (RuntimeError, AttributeError):
                    continue
    except (RuntimeError, AttributeError):
        pass
    return None


def _is_tab_alive(tab):
    """Check if a tab reference is still valid and usable.

    Args:
        tab: An AbstractTab instance, or None.

    Returns:
        True if the tab exists, is not deleted, and not pending removal.
    """
    if tab is None:
        return False
    try:
        return not tab.is_deleted() and not tab.pending_removal
    except RuntimeError:
        return False


def _unique_path(tmpdir, basename):
    """Get a unique file path in tmpdir using the given basename.

    If basename already exists, appends -2, -3, etc. before the extension.

    Returns:
        The full path as a string.
    """
    path = os.path.join(tmpdir, basename)
    if not os.path.exists(path):
        return path
    stem, ext = os.path.splitext(basename)
    for i in range(2, 1000):
        path = os.path.join(tmpdir, "{}-{}{}".format(stem, i, ext))
        if not os.path.exists(path):
            return path
    return path


def _is_crx_download(mime_type, filename):
    """Check if a download is a Chrome extension CRX file."""
    if mime_type == 'application/x-chrome-extension':
        return True
    if filename and filename.endswith('.crx'):
        return True
    return False


def _crx_download_finished(tmp_path):
    """Process a completed CRX download: extract and load the extension."""
    extensions_dir = os.path.join(standarddir.config(), 'extensions')
    os.makedirs(extensions_dir, exist_ok=True)

    try:
        ext_dir = crx.extract_crx(tmp_path, extensions_dir)
    except (ValueError, OSError, KeyError) as e:
        message.error(f"Failed to install extension: {e}")
        return
    finally:
        try:
            os.unlink(tmp_path)
        except OSError:
            pass

    ext_name = os.path.basename(ext_dir)
    log.misc.debug(f"CRX extracted to: {ext_dir}")

    # Load the extension into the running profile immediately
    from qutebrowser.browser.webengine import webenginesettings
    webenginesettings.load_extension_now(ext_dir)

    message.info(f"Extension installed: {ext_name}")


class DownloadItem(downloads.AbstractDownloadItem):

    """A wrapper over a QWebEngineDownloadRequest.

    Attributes:
        _qt_item: The wrapped item.
    """

    def __init__(self, qt_item: QWebEngineDownloadRequest,
                 manager: downloads.AbstractDownloadManager,
                 parent: QObject = None) -> None:
        super().__init__(manager=manager, parent=manager)
        self._qt_item = qt_item
        if machinery.IS_QT5:
            qt_item.downloadProgress.connect(self.stats.on_download_progress)
        else:  # Qt 6
            qt_item.receivedBytesChanged.connect(
                lambda: self.stats.on_download_progress(
                    qt_item.receivedBytes(),
                    qt_item.totalBytes(),
                )
            )
            qt_item.totalBytesChanged.connect(
                lambda: self.stats.on_download_progress(
                    qt_item.receivedBytes(),
                    qt_item.totalBytes(),
                )
            )
        qt_item.stateChanged.connect(
            self._on_state_changed)

        # Ensure wrapped qt_item is deleted manually when the wrapper object
        # is deleted. See https://github.com/qutebrowser/qutebrowser/issues/3373
        self.destroyed.connect(self._qt_item.deleteLater)

    def _is_page_download(self):
        """Check if this item is a page (i.e. mhtml) download."""
        return (self._qt_item.savePageFormat() !=
                QWebEngineDownloadRequest.SavePageFormat.UnknownSaveFormat)

    @pyqtSlot(QWebEngineDownloadRequest.DownloadState)
    def _on_state_changed(self, state):
        state_name = debug.qenum_key(QWebEngineDownloadRequest, state)
        log.downloads.debug("State for {!r} changed to {}".format(
            self, state_name))

        if state == QWebEngineDownloadRequest.DownloadState.DownloadRequested:
            pass
        elif state == QWebEngineDownloadRequest.DownloadState.DownloadInProgress:
            pass
        elif state == QWebEngineDownloadRequest.DownloadState.DownloadCompleted:
            log.downloads.debug("Download {} finished".format(self.basename))
            if self._is_page_download():
                # Same logging as QtWebKit mhtml downloads.
                log.downloads.debug("File successfully written.")
            self.successful = True
            self.done = True
            self.finished.emit()
            self.stats.finish()
        elif state == QWebEngineDownloadRequest.DownloadState.DownloadCancelled:
            self.successful = False
            self.done = True
            self.cancelled.emit()
            self.stats.finish()
        elif state == QWebEngineDownloadRequest.DownloadState.DownloadInterrupted:
            self.successful = False
            reason = self._qt_item.interruptReasonString()
            self._die(reason)
        else:
            raise ValueError("_on_state_changed was called with unknown state "
                             "{}".format(state_name))

    def _do_die(self):
        if machinery.IS_QT5:
            self._qt_item.downloadProgress.disconnect()
        else:  # Qt 6
            self._qt_item.receivedBytesChanged.disconnect()
            self._qt_item.totalBytesChanged.disconnect()

        if self._qt_item.state() != QWebEngineDownloadRequest.DownloadState.DownloadInterrupted:
            self._qt_item.cancel()

    def _do_cancel(self):
        state = self._qt_item.state()
        state_name = debug.qenum_key(QWebEngineDownloadRequest, state)
        assert state not in [QWebEngineDownloadRequest.DownloadState.DownloadCompleted,
                             QWebEngineDownloadRequest.DownloadState.DownloadCancelled], state_name
        self._qt_item.cancel()

    def retry(self):
        state = self._qt_item.state()
        if state != QWebEngineDownloadRequest.DownloadState.DownloadInterrupted:
            log.downloads.warning(
                "Refusing to retry download in state {}".format(
                    debug.qenum_key(QWebEngineDownloadRequest, state)))
            return

        self._qt_item.resume()

    def _get_open_filename(self):
        return self._filename

    def url(self) -> QUrl:
        return self._qt_item.url()

    def origin(self) -> QUrl:
        page = self._qt_item.page()
        return page.url() if page else QUrl()

    def _set_fileobj(self, fileobj, *, autoclose=True):
        raise downloads.UnsupportedOperationError

    def _set_tempfile(self, fileobj):
        fileobj.close()
        self._set_filename(fileobj.name, force_overwrite=True,
                           remember_directory=False)

    def _ensure_can_set_filename(self, filename):
        state = self._qt_item.state()
        if state != QWebEngineDownloadRequest.DownloadState.DownloadRequested:
            state_name = debug.qenum_key(QWebEngineDownloadRequest, state)
            raise ValueError("Trying to set filename {} on {!r} which is "
                             "state {} (not in requested state)!".format(
                                 filename, self, state_name))

    def _ask_confirm_question(self, title, msg, *, custom_yes_action=None):
        yes_action = custom_yes_action or self._after_set_filename
        no_action = functools.partial(self.cancel, remove_data=False)
        question = usertypes.Question()
        question.title = title
        question.text = msg
        question.url = 'file://{}'.format(self._filename)
        question.mode = usertypes.PromptMode.yesno
        question.answered_yes.connect(yes_action)
        question.answered_no.connect(no_action)
        question.cancelled.connect(no_action)
        self.cancelled.connect(question.abort)
        self.error.connect(question.abort)
        message.global_bridge.ask(question, blocking=True)

    def _ask_create_parent_question(self, title, msg,
                                    force_overwrite, remember_directory):
        assert self._filename is not None
        no_action = functools.partial(self.cancel, remove_data=False)
        question = usertypes.Question()
        question.title = title
        question.text = msg
        question.url = 'file://{}'.format(os.path.dirname(self._filename))
        question.mode = usertypes.PromptMode.yesno
        question.answered_yes.connect(lambda:
                                      self._after_create_parent_question(
                                          force_overwrite, remember_directory))
        question.answered_no.connect(no_action)
        question.cancelled.connect(no_action)
        self.cancelled.connect(question.abort)
        self.error.connect(question.abort)
        message.global_bridge.ask(question, blocking=True)

    def _after_set_filename(self):
        assert self._filename is not None

        dirname, basename = os.path.split(self._filename)
        self._qt_item.setDownloadDirectory(dirname)
        self._qt_item.setDownloadFileName(basename)

        self._qt_item.accept()

    def _get_conflicting_download(self):
        """Return another potential active download with the same name.

        webenginedownloads.DownloadItem needs to look for downloads both in its
        manager and in qtnetwork-download-manager as both are used
        simultaneously.

        This method can be safely removed once #2328 is fixed.
        """
        conflicting_download = super()._get_conflicting_download()
        if conflicting_download:
            return conflicting_download

        qtnetwork_download_manager = objreg.get(
            'qtnetwork-download-manager')
        for download in qtnetwork_download_manager.downloads:
            if self._conflicts_with(download):
                return download
        return None


def _strip_suffix(filename):
    """Convert a path we got from chromium to a suggested filename.

    Chromium thinks we want to download stuff to ~/Download, so even if we
    don't, we get downloads with a suffix like (1) for files existing there.

    We simply strip the suffix off via regex.

    See https://bugreports.qt.io/browse/QTBUG-56978
    """
    suffix_re = re.compile(r"""
      \ ?  # Optional space between filename and suffix
      (
        # Numerical suffix
        \([0-9]+\)
      |
        # ISO-8601 suffix
        # https://cs.chromium.org/chromium/src/base/time/time_to_iso8601.cc
        \ -\ \d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}.\d{3}Z
      )
      (?=\.|$)  # Begin of extension, or filename without extension
    """, re.VERBOSE)

    return suffix_re.sub('', filename)


class DownloadManager(downloads.AbstractDownloadManager):

    """Manager for currently running downloads.

    Attributes:
        _mhtml_target: DownloadTarget for the next MHTML download.
    """

    def __init__(self, parent=None):
        super().__init__(parent)
        self._mhtml_target = None

    def install(self, profile):
        """Set up the download manager on a QWebEngineProfile."""
        profile.downloadRequested.connect(self.handle_download,
                                          Qt.ConnectionType.DirectConnection)

    @pyqtSlot(QWebEngineDownloadRequest)
    def handle_download(self, qt_item):
        """Start a download coming from a QWebEngineProfile."""
        qt_filename = qt_item.downloadFileName()
        mime_type = qt_item.mimeType()
        url = qt_item.url()

        # WORKAROUND for https://bugreports.qt.io/browse/QTBUG-90355
        if version.qtwebengine_versions().webengine >= utils.VersionNumber(5, 15, 3):
            needs_workaround = False
        elif url.scheme().lower() == 'data':
            if '/' in url.path().split(',')[-1]:  # e.g. a slash in base64
                wrong_filename = url.path().split('/')[-1]
            else:
                wrong_filename = mime_type.split('/')[1]

            needs_workaround = qt_filename == wrong_filename
        else:
            needs_workaround = False

        if needs_workaround:
            suggested_filename = urlutils.filename_from_url(
                url, fallback='qutebrowser-download')
        else:
            suggested_filename = _strip_suffix(qt_filename)

        # CRX extension download: download to temp, extract, load
        if _is_crx_download(mime_type, qt_filename):
            tmpdir = downloads.temp_download_manager.get_tmpdir().name
            basename = suggested_filename or 'extension.crx'
            if not basename.endswith('.crx'):
                basename += '.crx'
            tmp_path = _unique_path(tmpdir, basename)

            download = DownloadItem(qt_item, manager=self)
            self._init_item(download, auto_remove=True,
                            suggested_filename=basename)
            download.finished.connect(
                functools.partial(_crx_download_finished, tmp_path))
            target = downloads.FileDownloadTarget(tmp_path,
                                                  force_overwrite=True)
            download.set_target(target)
            return

        use_pdfjs = pdfjs.should_use_pdfjs(mime_type, url)

        # Local file PDF shortcut: skip the download entirely, just copy and
        # navigate the originating tab to the PDF.js viewer.
        if use_pdfjs and url.scheme() == 'file':
            page = qt_item.page()
            origin_tab = _find_tab_for_page(page) if page else None
            qt_item.cancel()

            local_path = url.toLocalFile()
            basename = utils.sanitize_filename(os.path.basename(local_path))
            basename = utils.elide_filename(basename, 50)
            try:
                tmpdir = downloads.temp_download_manager.get_tmpdir().name
                tmp_path = _unique_path(tmpdir, basename)
                shutil.copy2(local_path, tmp_path)
            except OSError as exc:
                message.error("Failed to copy PDF: {}".format(exc))
                return

            viewer_url = pdfjs.get_main_url(
                os.path.basename(tmp_path), url)
            if _is_tab_alive(origin_tab):
                origin_tab.load_url(viewer_url)
            else:
                tabbed_browser = objreg.get('tabbed-browser', scope='window',
                                            window='last-focused')
                tabbed_browser.tabopen(viewer_url, background=False)
            return

        download = DownloadItem(qt_item, manager=self)
        self._init_item(download, auto_remove=use_pdfjs,
                        suggested_filename=suggested_filename)

        if self._mhtml_target is not None:
            download.set_target(self._mhtml_target)
            self._mhtml_target = None
            return
        if use_pdfjs:
            page = qt_item.page()
            download._origin_tab = _find_tab_for_page(page) if page else None
            download._original_basename = download.basename
            download.set_target(downloads.PDFJSDownloadTarget())
            return

        filename = downloads.immediate_download_path()
        if filename is not None:
            # User doesn't want to be asked, so just use the download_dir
            target = downloads.FileDownloadTarget(filename)
            download.set_target(target)
            return

        if download.cancel_for_origin():
            return

        # Ask the user for a filename - needs to be blocking!
        question = downloads.get_filename_question(
            suggested_filename=suggested_filename, url=qt_item.url(),
            parent=self)
        self._init_filename_question(question, download)
        message.global_bridge.ask(question, blocking=True)
        # The filename is set via the question.answered signal, connected in
        # _init_filename_question.

    def get_mhtml(self, tab, target):
        """Download the given tab as mhtml to the given target."""
        assert tab.backend == usertypes.Backend.QtWebEngine
        assert self._mhtml_target is None, self._mhtml_target
        self._mhtml_target = target
        tab.action.save_page()
