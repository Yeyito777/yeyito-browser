// Copyright (C) 2021 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:critical reason:data-parser

#ifndef QWEBENGINEVIEW_P_H
#define QWEBENGINEVIEW_P_H

//
//  W A R N I N G
//  -------------
//
// This file is not part of the Qt API.  It exists purely as an
// implementation detail.  This header file may change from version to
// version without notice, or even be removed.
//
// We mean it.
//

#include <QtWebEngineCore/private/qwebenginepage_p.h> // PageView

#include "render_view_context_menu_qt.h"

#include <QtCore/qpointer.h>
#include <QtCore/qsize.h>
#include <QtCore/qpoint.h>
#include <QtCore/qurl.h>

namespace QtWebEngineCore {
class AutofillPopupController;
class QWebEngineContextMenuRequest;
class WebEngineQuickWidget;
class RenderWidgetHostViewQtDelegate;
class RenderWidgetHostViewQtDelegateClient;
class TouchSelectionMenuController;
}

namespace QtWebEngineWidgetUI {
class AutofillPopupWidget;
class TouchHandleDrawableDelegate;
class TouchSelectionMenuWidget;
}

QT_BEGIN_NAMESPACE

class QLabel;
class QLineEdit;
class QMenu;
class QPrinter;
class QTabBar;
class QTabWidget;
class QWebEngineFindTextResult;
class QWebEngineView;
class QWidget;

class QWebEngineViewPrivate : public PageView
{
public:
    Q_DECLARE_PUBLIC(QWebEngineView)
    QWebEngineView *q_ptr;

    void pageChanged(QWebEnginePage *oldPage, QWebEnginePage *newPage);
    void widgetChanged(QtWebEngineCore::WebEngineQuickWidget *oldWidget,
                       QtWebEngineCore::WebEngineQuickWidget *newWidget);

    void contextMenuRequested(QWebEngineContextMenuRequest *request) override;
    QStringList chooseFiles(QWebEnginePage::FileSelectionMode mode, const QStringList &oldFiles,
                            const QStringList &acceptedMimeTypes) override;
    void
    showColorDialog(QSharedPointer<QtWebEngineCore::ColorChooserController> controller) override;
    bool showAuthorizationDialog(const QString &title, const QString &message) override;
    void javaScriptAlert(const QUrl &url, const QString &msg) override;
    bool javaScriptConfirm(const QUrl &url, const QString &msg) override;
    bool javaScriptPrompt(const QUrl &url, const QString &msg, const QString &defaultValue,
                          QString *result) override;
    void setToolTip(const QString &toolTipText) override;
    QtWebEngineCore::RenderWidgetHostViewQtDelegate *CreateRenderWidgetHostViewQtDelegate(
            QtWebEngineCore::RenderWidgetHostViewQtDelegateClient *client) override;
    QtWebEngineCore::RenderWidgetHostViewQtDelegate *CreateRenderWidgetHostViewQtDelegateForPopup(
            QtWebEngineCore::RenderWidgetHostViewQtDelegateClient *client) override;
    QWebEngineContextMenuRequest *lastContextMenuRequest() const override;
    QWebEnginePage *createPageForWindow(QWebEnginePage::WebWindowType type) override;
    QObject *accessibilityParentObject() override;
    void didPrintPage(QPrinter *&printer, QSharedPointer<QByteArray> result) override;
    void didPrintPageToPdf(const QString &filePath, bool success) override;
    void printRequested() override;
    void printRequestedByFrame(QWebEngineFrame frame) override;
    void showAutofillPopup(QtWebEngineCore::AutofillPopupController *controller,
                           const QRect &bounds, bool autoselectFirstSuggestion) override;
    void hideAutofillPopup() override;
    QtWebEngineCore::TouchHandleDrawableDelegate *
    createTouchHandleDelegate(const QMap<int, QImage> &images) override;

    void showTouchSelectionMenu(QtWebEngineCore::TouchSelectionMenuController *,
                                const QRect &) override;
    void hideTouchSelectionMenu() override;
    QWebEngineViewPrivate();
    virtual ~QWebEngineViewPrivate();
    static void bindPageAndView(QWebEnginePage *page, QWebEngineView *view);
    static void bindPageAndWidget(QWebEnginePagePrivate *pagePrivate,
                                  QtWebEngineCore::WebEngineQuickWidget *widget);
    QIcon webActionIcon(QWebEnginePage::WebAction action) const;
    void unhandledKeyEvent(QKeyEvent *event) override;
    void focusContainer() override;
    bool passOnFocus(bool reverse) override;
    bool isEnabled() const override;
    bool isVisible() const override;
    QRect viewportRect() const override;
    bool qutebrowserHandleCommand(const QString &command) override;
    QTabWidget *qutebrowserAncestorTabWidget(int *viewIndex = nullptr,
                                             int *currentIndex = nullptr) const;
    QTabBar *qutebrowserAncestorTabBar(QTabWidget *tabWidget) const;
    bool qutebrowserSetCurrentTabIndex(int targetIndex);
    bool qutebrowserMoveCurrentTab(int targetIndex);
    bool qutebrowserHandleTabCommand(const QString &name, const QString &arguments);
    void ensureQutebrowserTabSidebar();
    void updateQutebrowserTabSidebar();
    int qutebrowserChromeLeftInset() const;
    void ensureQutebrowserStatusOverlay();
    void updateQutebrowserStatusOverlay();
    void positionQutebrowserStatusOverlay();
    void ensureQutebrowserFindOverlay();
    void positionQutebrowserFindOverlay();
    void startQutebrowserFind(bool reverse);
    void acceptQutebrowserFind();
    void cancelQutebrowserFind();
    void clearQutebrowserFind();
    void updateQutebrowserFindFromInput();
    void navigateQutebrowserFind(bool reverse);
    void onQutebrowserFindFinished(const QWebEngineFindTextResult &result);
    void updateQutebrowserFindOverlay();
    void ensureQutebrowserCommandLineOverlay();
    void positionQutebrowserCommandLineOverlay();
    void startQutebrowserCommandLine(const QString &text);
    void acceptQutebrowserCommandLine();
    void cancelQutebrowserCommandLine();
    void hideQutebrowserCommandLine();
    void executeQutebrowserCommandLineCommand(const QString &command);
    QString qutebrowserCommandLineText() const;
    QString expandQutebrowserCommandLinePlaceholders(QString text) const;
    QString qutebrowserCurrentUrlText() const;
    void onQutebrowserModeChanged(const QString &oldMode, const QString &newMode);
    void onQutebrowserStatusChanged(const QString &mode, const QString &keychain, const QString &count);
    QString qutebrowserScrollText() const;
    QString qutebrowserUrlText() const;
    QWebEnginePage *page;
    QMetaObject::Connection m_pageConnection;
    QMetaObject::Connection m_qutebrowserModeConnection;
    QMetaObject::Connection m_qutebrowserStatusConnection;
    QMetaObject::Connection m_qutebrowserTitleConnection;
    QMetaObject::Connection m_qutebrowserUrlConnection;
    QMetaObject::Connection m_qutebrowserLinkConnection;
    QMetaObject::Connection m_qutebrowserScrollConnection;
    QMetaObject::Connection m_qutebrowserContentsConnection;
    QMetaObject::Connection m_qutebrowserLoadStartedConnection;
    QMetaObject::Connection m_qutebrowserLoadProgressConnection;
    QMetaObject::Connection m_qutebrowserLoadFinishedConnection;
    QMetaObject::Connection m_qutebrowserFindConnection;
    QPointer<QtWebEngineCore::WebEngineQuickWidget> m_webEngineWidget;
    QWidget *m_qutebrowserTabSidebar = nullptr;
    QVBoxLayout *m_qutebrowserTabListLayout = nullptr;
    QLabel *m_qutebrowserStatusOverlay = nullptr;
    QWidget *m_qutebrowserFindOverlay = nullptr;
    QLabel *m_qutebrowserFindPrefixLabel = nullptr;
    QLineEdit *m_qutebrowserFindLineEdit = nullptr;
    QLabel *m_qutebrowserFindCountLabel = nullptr;
    QWidget *m_qutebrowserCommandLineOverlay = nullptr;
    QLabel *m_qutebrowserCommandLinePrefixLabel = nullptr;
    QLineEdit *m_qutebrowserCommandLineEdit = nullptr;
    QString m_qutebrowserMode = QStringLiteral("normal");
    QString m_qutebrowserKeychain;
    QString m_qutebrowserCount;
    QUrl m_qutebrowserUrl;
    QString m_qutebrowserHoveredUrl;
    QPointF m_qutebrowserScrollPosition;
    QSizeF m_qutebrowserContentsSize;
    int m_qutebrowserLoadProgress = 100;
    bool m_qutebrowserLoading = false;
    bool m_qutebrowserCanGoBack = false;
    bool m_qutebrowserCanGoForward = false;
    bool m_qutebrowserFindActive = false;
    bool m_qutebrowserFindReverse = false;
    bool m_qutebrowserCommandLineActive = false;
    QString m_qutebrowserFindText;
    int m_qutebrowserFindActiveMatch = 0;
    int m_qutebrowserFindTotalMatches = 0;
    bool m_dragEntered;
    mutable bool m_ownsPage;
    QWebEngineContextMenuRequest *m_contextRequest;
    QScopedPointer<QtWebEngineWidgetUI::AutofillPopupWidget> m_autofillPopupWidget;
    QPointer<QtWebEngineWidgetUI::TouchSelectionMenuWidget> m_touchSelectionMenu;
};

class QContextMenuBuilder : public QtWebEngineCore::RenderViewContextMenuQt
{
public:
    QContextMenuBuilder(QWebEngineContextMenuRequest *request, QWebEngineView *view, QMenu *menu);

private:
    virtual bool hasInspector() override;
    virtual bool isFullScreenMode() override;

    virtual void addMenuItem(ContextMenuItem entry) override;
    virtual bool isMenuItemEnabled(ContextMenuItem entry) override;

    QWebEngineView *m_view;
    QMenu *m_menu;
};

QT_END_NAMESPACE

#endif // QWEBENGINEVIEW_P_H
