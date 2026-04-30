// Copyright (C) 2021 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:critical reason:data-parser

#include "qapplication.h"
#include "qwebenginenotificationpresenter_p.h"
#include "qwebengineview.h"
#include "qwebengineview_p.h"
#include "render_widget_host_view_qt_delegate_client.h"
#include "render_widget_host_view_qt_delegate_item.h"
#include "ui/autofillpopupwidget_p.h"
#include "touchhandlewidget_p.h"
#include "touchselectionmenuwidget_p.h"

#include <QtWebEngineCore/private/qwebenginepage_p.h>
#include <QtWebEngineCore/qwebenginecontextmenurequest.h>
#include <QtWebEngineCore/qwebenginefindtextresult.h>
#include <QtWebEngineCore/qwebenginehistory.h>
#include <QtWebEngineCore/qwebenginehttprequest.h>
#include <QtWebEngineCore/qwebengineprofile.h>

#include "autofill_popup_controller.h"
#include "color_chooser_controller.h"
#include "touch_selection_menu_controller.h"
#include "web_contents_adapter.h"

#include <QContextMenuEvent>
#include <QToolTip>
#include <QBoxLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QKeySequence>
#include <QKeyEvent>
#include <QHash>
#include <QIcon>
#include <QLabel>
#include <QPixmap>
#include <QPointer>
#include <QLineEdit>
#include <QList>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStyle>
#include <QStringList>
#include <QTabBar>
#include <QTabWidget>
#include <QGuiApplication>
#include <QQuickWidget>
#include <QtWidgets/private/qapplication_p.h>

#if QT_CONFIG(accessibility)
#include "qwebengine_accessible_p.h"
#endif

#if QT_CONFIG(action)
#include <QAction>
#endif

#if QT_CONFIG(colordialog)
#include <QColorDialog>
#endif

#if QT_CONFIG(filedialog)
#include <QFileDialog>
#include <QStandardPaths>
#include "file_picker_controller.h"
#endif

#if QT_CONFIG(inputdialog)
#include <QInputDialog>
#endif

#if QT_CONFIG(menu)
#include <QMenu>
#endif

#if QT_CONFIG(messagebox)
#include <QMessageBox>
#endif

#if QT_CONFIG(webengine_printing_and_pdf)
#include "printing/printer_worker.h"

#include <QPrintEngine>
#include <QPrinter>
#include <QThread>
#endif

QT_BEGIN_NAMESPACE
class QSpontaneKeyEvent
{
public:
    static inline void makeSpontaneous(QEvent *ev) { ev->setSpontaneous(); }
};
QT_END_NAMESPACE

namespace QtWebEngineCore {
class WebEngineQuickWidget : public QQuickWidget, public WidgetDelegate
{
public:
    WebEngineQuickWidget(RenderWidgetHostViewQtDelegateItem *widget, QWidget *parent)
        : QQuickWidget(parent)
        , m_contentItem(widget)
    {
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        setAttribute(Qt::WA_AcceptTouchEvents);
        setAttribute(Qt::WA_OpaquePaintEvent);
        setAttribute(Qt::WA_AlwaysShowToolTips);

        QQuickItem *root = new QQuickItem(); // Indirection so we don't delete m_contentItem
        setContent(QUrl(), nullptr, root);
        root->setFlags(QQuickItem::ItemHasContents);
        root->setVisible(true);
        m_contentItem->setParentItem(root);

        connectRemoveParentBeforeParentDelete();
    }
    ~WebEngineQuickWidget() override
    {
        if (m_contentItem) {
            m_contentItem->setWidgetDelegate(nullptr);
            m_contentItem->setParentItem(nullptr);
        }
    }

    void InitAsPopup(const QRect &screenRect) override
    {
        setAttribute(Qt::WA_ShowWithoutActivating);
        setFocusPolicy(Qt::NoFocus);
        setWindowFlags(Qt::Popup | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);

        setGeometry(screenRect);
        raise();
        m_contentItem->show();
        show();
    }

    void Bind(WebContentsAdapterClient *client) override
    {
        QWebEnginePagePrivate *page = static_cast<QWebEnginePagePrivate *>(client);
        if (m_pageDestroyedConnection)
            QObject::disconnect(m_pageDestroyedConnection);
        QWebEngineViewPrivate::bindPageAndWidget(page, this);
        m_pageDestroyedConnection = QObject::connect(page->q_ptr, &QObject::destroyed, this, &WebEngineQuickWidget::Unbind);
    }

    void Unbind() override
    {
        if (m_pageDestroyedConnection) {
            QObject::disconnect(m_pageDestroyedConnection);
            m_pageDestroyedConnection = {};
        }
        QWebEngineViewPrivate::bindPageAndWidget(nullptr, this);
    }

    void Destroy() override
    {
        deleteLater();

        // The event loop may be exited at this point.
        // Ensure deferred deletion in this scenario.
        if (QThread::currentThread()->loopLevel() == 0)
            QCoreApplication::sendPostedEvents(this, QEvent::DeferredDelete);
    }

    bool ActiveFocusOnPress() override
    {
        return true;
    }

    void SetInputMethodEnabled(bool enabled) override
    {
        QQuickWidget::setAttribute(Qt::WA_InputMethodEnabled, enabled);
    }
    void SetInputMethodHints(Qt::InputMethodHints hints) override
    {
        QQuickWidget::setInputMethodHints(hints);
    }
    void SetClearColor(const QColor &color) override
    {
        setUpdatesEnabled(false);
        QQuickWidget::setClearColor(color);
        // QQuickWidget is usually blended by punching holes into widgets
        // above it to simulate the visual stacking order. If we want it to be
        // transparent we have to throw away the proper stacking order and always
        // blend the complete normal widgets backing store under it.
        bool isTranslucent = color.alpha() < 255;
        setAttribute(Qt::WA_AlwaysStackOnTop, isTranslucent);
        setAttribute(Qt::WA_OpaquePaintEvent, !isTranslucent);
        setUpdatesEnabled(true);
        window()->update();
    }
    void MoveWindow(const QPoint &screenPos) override
    {
        QQuickWidget::move(screenPos);
    }
    void Resize(int width, int height) override
    {
        QQuickWidget::resize(width, height);
    }
    QWindow *Window() override
    {
        if (const QWidget *root = QQuickWidget::window())
            return root->windowHandle();
        return nullptr;
    }
    void unhandledWheelEvent(QWheelEvent *ev) override
    {
        auto parentWidget = QQuickWidget::parentWidget();
        if (parentWidget) {
            if (QApplicationPrivate::wheel_widget)
                QApplicationPrivate::wheel_widget = nullptr;
            QSpontaneKeyEvent::makeSpontaneous(ev);
            qApp->notify(parentWidget, ev);
        }
    }
    void SetCursor(const QCursor &cursor) override
    {
        if (auto parentWidget = QQuickWidget::parentWidget())
            parentWidget->setCursor(cursor);
    }

protected:
    void closeEvent(QCloseEvent *event) override
    {
        QQuickWidget::closeEvent(event);

        // If a close event was received from the window manager (e.g. when moving the parent window,
        // clicking outside the popup area)
        // make sure to notify the Chromium WebUI popup and its underlying
        // RenderWidgetHostViewQtDelegate instance to be closed.
        if (m_contentItem && m_contentItem->m_isPopup)
            m_contentItem->m_client->closePopup();
    }
    void showEvent(QShowEvent *event) override
    {
        QQuickWidget::showEvent(event);
        // We don't have a way to catch a top-level window change with QWidget
        // but a widget will most likely be shown again if it changes, so do
        // the reconnection at this point.
        for (const QMetaObject::Connection &c : std::as_const(m_windowConnections))
            disconnect(c);
        m_windowConnections.clear();
        if (QWindow *w = Window()) {
            m_windowConnections.append(connect(w, SIGNAL(xChanged(int)), m_contentItem, SLOT(onWindowPosChanged())));
            m_windowConnections.append(connect(w, SIGNAL(yChanged(int)), m_contentItem, SLOT(onWindowPosChanged())));
        }
    }
    void resizeEvent(QResizeEvent *event) override
    {
        QQuickWidget::resizeEvent(event);
        if (m_contentItem) { // FIXME: Not sure why we need to set m_contentItem size manually
            m_contentItem->setSize(event->size());
            m_contentItem->onWindowPosChanged();
        }
    }
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override
    {
        if (m_contentItem)
            return m_contentItem->inputMethodQuery(query);
        return QVariant();
    }
    bool event(QEvent *event) override;

    void connectRemoveParentBeforeParentDelete();
    void removeParentBeforeParentDelete();

private:
    friend QWebEngineViewPrivate;
    QPointer<RenderWidgetHostViewQtDelegateItem> m_contentItem; // deleted by core
    QMetaObject::Connection m_parentDestroyedConnection;
    QMetaObject::Connection m_pageDestroyedConnection;
    QList<QMetaObject::Connection> m_windowConnections;
};

void WebEngineQuickWidget::connectRemoveParentBeforeParentDelete()
{
    disconnect(m_parentDestroyedConnection);

    if (QWidget *parent = parentWidget()) {
        m_parentDestroyedConnection = connect(parent, &QObject::destroyed,
                                              this,
                                              &WebEngineQuickWidget::removeParentBeforeParentDelete);
    } else {
        m_parentDestroyedConnection = QMetaObject::Connection();
    }
}

void WebEngineQuickWidget::removeParentBeforeParentDelete()
{
    // Unset the parent, because parent is being destroyed, but the owner of this
    // WebEngineQuickWidget is actually a RenderWidgetHostViewQt instance.
    setParent(nullptr);

    // If this widget represents a popup window, make sure to close it, so that if the popup was the
    // last visible top level window, the application event loop can quit if it deems it necessarry.
    if (m_contentItem && m_contentItem->m_isPopup)
        close();
}

bool WebEngineQuickWidget::event(QEvent *event)
{
    bool handled = false;

    // Track parent to make sure we don't get deleted.
    if (event->type() == QEvent::ParentChange)
        connectRemoveParentBeforeParentDelete();

    if (!m_contentItem)
        return QQuickWidget::event(event);

    // Mimic QWidget::event() by ignoring mouse, keyboard, touch and tablet events if the widget is
    // disabled.
    if (!isEnabled()) {
        switch (event->type()) {
        case QEvent::TabletPress:
        case QEvent::TabletRelease:
        case QEvent::TabletMove:
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
        case QEvent::MouseMove:
        case QEvent::TouchBegin:
        case QEvent::TouchUpdate:
        case QEvent::TouchEnd:
        case QEvent::TouchCancel:
        case QEvent::ContextMenu:
        case QEvent::KeyPress:
        case QEvent::KeyRelease:
#if QT_CONFIG(wheelevent)
        case QEvent::Wheel:
#endif
            return false;
        default:
            break;
        }
    }

    switch (event->type()) {
    case QEvent::FocusIn:
    case QEvent::FocusOut:
        // We forward focus events later, once they have made it to the content item.
        return QQuickWidget::event(event);
    case QEvent::DragEnter:
    case QEvent::DragLeave:
    case QEvent::DragMove:
    case QEvent::Drop:
    case QEvent::HoverEnter:
    case QEvent::HoverLeave:
    case QEvent::HoverMove:
        // Let the parent handle these events.
        return false;
    default:
        break;
    }

    switch (event->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
        case QEvent::MouseMove:
            // Don't forward mouse events synthesized by the system, which are caused by genuine touch
            // events. Chromium would then process for e.g. a mouse click handler twice, once due to the
            // system synthesized mouse event, and another time due to a touch-to-gesture-to-mouse
            // transformation done by Chromium.
            // Only allow them for popup type, since QWidgetWindow will ignore them for Qt::Popup flag,
            // which is expected to get input through synthesized mouse events (either by system or Qt)
            if (!m_contentItem->m_isPopup &&
                    static_cast<QMouseEvent *>(event)->source() == Qt::MouseEventSynthesizedBySystem) {
                Q_ASSERT(!windowFlags().testFlag(Qt::Popup));
                return true;
            }
            break;
        default:
            break;
    }

    if (event->type() == QEvent::MouseButtonDblClick) {
        // QWidget keeps the Qt4 behavior where the DblClick event would replace the Press event.
        // QtQuick is different by sending both the Press and DblClick events for the second press
        // where we can simply ignore the DblClick event.
        QMouseEvent *dblClick = static_cast<QMouseEvent *>(event);
        QMouseEvent press(QEvent::MouseButtonPress, dblClick->position(), dblClick->scenePosition(),
                          dblClick->globalPosition(), dblClick->button(), dblClick->buttons(),
                          dblClick->modifiers(), dblClick->source());
        press.setTimestamp(dblClick->timestamp());
        handled = m_contentItem->m_client->forwardEvent(&press);
    } else
        handled = m_contentItem->m_client->forwardEvent(event);

    if (!handled)
        return QQuickWidget::event(event);
    event->accept();
    return true;
}

} // namespace QtWebEngineCore

QT_BEGIN_NAMESPACE

void QWebEngineViewPrivate::pageChanged(QWebEnginePage *oldPage, QWebEnginePage *newPage)
{
    Q_Q(QWebEngineView);

    if (oldPage) {
        oldPage->setVisible(false);
        QObject::disconnect(oldPage, &QWebEnginePage::titleChanged, q, &QWebEngineView::titleChanged);
        QObject::disconnect(oldPage, &QWebEnginePage::urlChanged, q, &QWebEngineView::urlChanged);
        QObject::disconnect(oldPage, &QWebEnginePage::iconUrlChanged, q, &QWebEngineView::iconUrlChanged);
        QObject::disconnect(oldPage, &QWebEnginePage::iconChanged, q, &QWebEngineView::iconChanged);
        QObject::disconnect(oldPage, &QWebEnginePage::loadStarted, q, &QWebEngineView::loadStarted);
        QObject::disconnect(oldPage, &QWebEnginePage::loadProgress, q, &QWebEngineView::loadProgress);
        QObject::disconnect(oldPage, &QWebEnginePage::loadFinished, q, &QWebEngineView::loadFinished);
        QObject::disconnect(oldPage, &QWebEnginePage::selectionChanged, q, &QWebEngineView::selectionChanged);
        QObject::disconnect(oldPage, &QWebEnginePage::renderProcessTerminated, q, &QWebEngineView::renderProcessTerminated);
        QObject::disconnect(m_qutebrowserModeConnection);
        QObject::disconnect(m_qutebrowserStatusConnection);
        QObject::disconnect(m_qutebrowserTitleConnection);
        QObject::disconnect(m_qutebrowserIconConnection);
        QObject::disconnect(m_qutebrowserUrlConnection);
        QObject::disconnect(m_qutebrowserLinkConnection);
        QObject::disconnect(m_qutebrowserScrollConnection);
        QObject::disconnect(m_qutebrowserContentsConnection);
        QObject::disconnect(m_qutebrowserLoadStartedConnection);
        QObject::disconnect(m_qutebrowserLoadProgressConnection);
        QObject::disconnect(m_qutebrowserLoadFinishedConnection);
        QObject::disconnect(m_qutebrowserFindConnection);
        m_qutebrowserModeConnection = {};
        m_qutebrowserStatusConnection = {};
        m_qutebrowserTitleConnection = {};
        m_qutebrowserIconConnection = {};
        m_qutebrowserUrlConnection = {};
        m_qutebrowserLinkConnection = {};
        m_qutebrowserScrollConnection = {};
        m_qutebrowserContentsConnection = {};
        m_qutebrowserLoadStartedConnection = {};
        m_qutebrowserLoadProgressConnection = {};
        m_qutebrowserLoadFinishedConnection = {};
        m_qutebrowserFindConnection = {};
    }

    if (newPage) {
        QObject::connect(newPage, &QWebEnginePage::titleChanged, q, &QWebEngineView::titleChanged);
        QObject::connect(newPage, &QWebEnginePage::urlChanged, q, &QWebEngineView::urlChanged);
        QObject::connect(newPage, &QWebEnginePage::iconUrlChanged, q, &QWebEngineView::iconUrlChanged);
        QObject::connect(newPage, &QWebEnginePage::iconChanged, q, &QWebEngineView::iconChanged);
        QObject::connect(newPage, &QWebEnginePage::loadStarted, q, &QWebEngineView::loadStarted);
        QObject::connect(newPage, &QWebEnginePage::loadProgress, q, &QWebEngineView::loadProgress);
        QObject::connect(newPage, &QWebEnginePage::loadFinished, q, &QWebEngineView::loadFinished);
        QObject::connect(newPage, &QWebEnginePage::selectionChanged, q, &QWebEngineView::selectionChanged);
        QObject::connect(newPage, &QWebEnginePage::renderProcessTerminated, q, &QWebEngineView::renderProcessTerminated);
        m_qutebrowserModeConnection = QObject::connect(newPage, &QWebEnginePage::qutebrowserModeChanged,
                                                       q, [this](const QString &oldMode, const QString &newMode) {
            onQutebrowserModeChanged(oldMode, newMode);
        });
        m_qutebrowserStatusConnection = QObject::connect(newPage, &QWebEnginePage::qutebrowserStatusChanged,
                                                         q, [this](const QString &mode, const QString &keychain, const QString &count) {
            onQutebrowserStatusChanged(mode, keychain, count);
        });
        m_qutebrowserTitleConnection = QObject::connect(newPage, &QWebEnginePage::titleChanged,
                                                       q, [this](const QString &) {
            updateQutebrowserTabSidebar();
        });
        m_qutebrowserIconConnection = QObject::connect(newPage, &QWebEnginePage::iconChanged,
                                                      q, [this](const QIcon &) {
            updateQutebrowserTabSidebar();
        });
        m_qutebrowserUrlConnection = QObject::connect(newPage, &QWebEnginePage::urlChanged,
                                                      q, [this](const QUrl &url) {
            m_qutebrowserUrl = url;
            updateQutebrowserTabSidebar();
            updateQutebrowserStatusOverlay();
        });
        m_qutebrowserLinkConnection = QObject::connect(newPage, &QWebEnginePage::linkHovered,
                                                       q, [this](const QString &url) {
            m_qutebrowserHoveredUrl = url;
            updateQutebrowserStatusOverlay();
        });
        m_qutebrowserScrollConnection = QObject::connect(newPage, &QWebEnginePage::scrollPositionChanged,
                                                         q, [this](const QPointF &position) {
            m_qutebrowserScrollPosition = position;
            updateQutebrowserStatusOverlay();
        });
        m_qutebrowserContentsConnection = QObject::connect(newPage, &QWebEnginePage::contentsSizeChanged,
                                                           q, [this](const QSizeF &size) {
            m_qutebrowserContentsSize = size;
            updateQutebrowserStatusOverlay();
        });
        m_qutebrowserLoadStartedConnection = QObject::connect(newPage, &QWebEnginePage::loadStarted,
                                                              q, [this]() {
            m_qutebrowserLoading = true;
            m_qutebrowserLoadProgress = 0;
            updateQutebrowserStatusOverlay();
        });
        m_qutebrowserLoadProgressConnection = QObject::connect(newPage, &QWebEnginePage::loadProgress,
                                                               q, [this](int progress) {
            m_qutebrowserLoading = progress < 100;
            m_qutebrowserLoadProgress = progress;
            updateQutebrowserStatusOverlay();
        });
        m_qutebrowserLoadFinishedConnection = QObject::connect(newPage, &QWebEnginePage::loadFinished,
                                                               q, [this](bool) {
            m_qutebrowserLoading = false;
            m_qutebrowserLoadProgress = 100;
            if (page) {
                m_qutebrowserCanGoBack = page->history()->canGoBack();
                m_qutebrowserCanGoForward = page->history()->canGoForward();
            }
            updateQutebrowserStatusOverlay();
        });
        m_qutebrowserFindConnection = QObject::connect(newPage, &QWebEnginePage::findTextFinished,
                                                       q, [this](const QWebEngineFindTextResult &result) {
            onQutebrowserFindFinished(result);
        });
        m_qutebrowserUrl = newPage->url();
        m_qutebrowserScrollPosition = newPage->scrollPosition();
        m_qutebrowserContentsSize = newPage->contentsSize();
        m_qutebrowserCanGoBack = newPage->history()->canGoBack();
        m_qutebrowserCanGoForward = newPage->history()->canGoForward();
        newPage->setVisible(q->isVisible());
    }

    auto oldUrl = oldPage ? oldPage->url() : QUrl();
    auto newUrl = newPage ? newPage->url() : QUrl();
    if (oldUrl != newUrl)
        Q_EMIT q->urlChanged(newUrl);

    auto oldTitle = oldPage ? oldPage->title() : QString();
    auto newTitle = newPage ? newPage->title() : QString();
    if (oldTitle != newTitle)
        Q_EMIT q->titleChanged(newTitle);

    auto oldIcon = oldPage ? oldPage->iconUrl() : QUrl();
    auto newIcon = newPage ? newPage->iconUrl() : QUrl();
    if (oldIcon != newIcon) {
        Q_EMIT q->iconUrlChanged(newIcon);
        Q_EMIT q->iconChanged(newPage ? newPage->icon() : QIcon());
    }

    if ((oldPage && oldPage->hasSelection()) || (newPage && newPage->hasSelection()))
        Q_EMIT q->selectionChanged();

    m_qutebrowserMode = QStringLiteral("normal");
    m_qutebrowserKeychain.clear();
    m_qutebrowserCount.clear();
    if (newPage) {
        m_qutebrowserUrl = newPage->url();
        m_qutebrowserScrollPosition = newPage->scrollPosition();
        m_qutebrowserContentsSize = newPage->contentsSize();
        m_qutebrowserCanGoBack = newPage->history()->canGoBack();
        m_qutebrowserCanGoForward = newPage->history()->canGoForward();
    } else {
        m_qutebrowserUrl = QUrl();
        m_qutebrowserScrollPosition = QPointF();
        m_qutebrowserContentsSize = QSizeF();
        m_qutebrowserCanGoBack = false;
        m_qutebrowserCanGoForward = false;
    }
    m_qutebrowserHoveredUrl.clear();
    m_qutebrowserLoading = false;
    m_qutebrowserLoadProgress = 100;
    updateQutebrowserTabSidebar();
    updateQutebrowserStatusOverlay();
}

static QString qutebrowserModeDisplayName(QString mode)
{
    mode.replace(QLatin1Char('_'), QLatin1Char('-'));
    return mode.toUpper();
}

static QString qutebrowserCommandName(QString command)
{
    command = command.trimmed();
    const int space = command.indexOf(QLatin1Char(' '));
    if (space >= 0)
        command.truncate(space);
    return command;
}

static QString qutebrowserCommandArgument(QString command)
{
    command = command.trimmed();
    const int space = command.indexOf(QLatin1Char(' '));
    return space < 0 ? QString() : command.mid(space + 1).trimmed();
}

struct QutebrowserCmdSetTextPreset
{
    QString text;
    bool valid = false;
    bool space = false;
    bool append = false;
};

static bool qutebrowserApplyCmdSetTextOption(const QString &option,
                                             QutebrowserCmdSetTextPreset *preset)
{
    if (option == QStringLiteral("--space")) {
        preset->space = true;
        return true;
    }
    if (option == QStringLiteral("--append")) {
        preset->append = true;
        return true;
    }
    if (option == QStringLiteral("--run-on-count"))
        return true;
    if (!option.startsWith(QLatin1Char('-')) || option.startsWith(QStringLiteral("--")))
        return false;

    for (int i = 1; i < option.size(); ++i) {
        const QChar ch = option.at(i);
        if (ch == QLatin1Char('s')) {
            preset->space = true;
        } else if (ch == QLatin1Char('a')) {
            preset->append = true;
        } else if (ch != QLatin1Char('r')) {
            return false;
        }
    }
    return option.size() > 1;
}

static QutebrowserCmdSetTextPreset qutebrowserParseCmdSetTextPreset(const QString &arguments)
{
    QutebrowserCmdSetTextPreset preset;
    const int length = arguments.size();
    int index = 0;

    while (index < length && arguments.at(index).isSpace())
        ++index;

    while (index < length) {
        const int tokenStart = index;
        while (index < length && !arguments.at(index).isSpace())
            ++index;
        const QString token = arguments.mid(tokenStart, index - tokenStart);

        if (token == QStringLiteral("--")) {
            while (index < length && arguments.at(index).isSpace())
                ++index;
            preset.text = arguments.mid(index);
            preset.valid = true;
            return preset;
        }

        if (!token.startsWith(QLatin1Char('-')) || !qutebrowserApplyCmdSetTextOption(token, &preset)) {
            preset.text = arguments.mid(tokenStart);
            preset.valid = true;
            return preset;
        }

        while (index < length && arguments.at(index).isSpace())
            ++index;
    }

    preset.valid = false;
    return preset;
}

static QStringList qutebrowserCommandArgumentTokens(const QString &arguments)
{
    return arguments.split(QLatin1Char(' '), Qt::SkipEmptyParts);
}

static bool qutebrowserParseStrictInteger(const QString &token, int *value)
{
    if (token.isEmpty())
        return false;

    int index = 0;
    if (token.at(index) == QLatin1Char('-')) {
        if (token.size() == 1)
            return false;
        ++index;
    }

    for (; index < token.size(); ++index) {
        if (!token.at(index).isDigit())
            return false;
    }

    bool ok = false;
    const int parsed = token.toInt(&ok);
    if (!ok)
        return false;
    if (value)
        *value = parsed;
    return true;
}

static constexpr int qutebrowserNativeTabSidebarWidth = 175;

static QString qutebrowserTabDisplayUrl(const QUrl &url)
{
    if (url.isEmpty())
        return QStringLiteral("about:blank");
    QString text = url.toDisplayString(QUrl::PreferLocalFile | QUrl::RemovePassword);
    if (url.isLocalFile()) {
        const QString path = url.toLocalFile();
        const int slash = path.lastIndexOf(QLatin1Char('/'));
        return slash >= 0 ? path.mid(slash + 1) : path;
    }
    if (text.startsWith(QStringLiteral("https://")))
        text.remove(0, 8);
    else if (text.startsWith(QStringLiteral("http://")))
        text.remove(0, 7);
    if (text.endsWith(QLatin1Char('/')))
        text.chop(1);
    return text;
}

static QTabBar *qutebrowserShellTabBar(QTabWidget *tabWidget)
{
    if (!tabWidget)
        return nullptr;
    const QList<QTabBar *> directBars = tabWidget->findChildren<QTabBar *>(QString(),
                                                                          Qt::FindDirectChildrenOnly);
    if (directBars.size() == 1)
        return directBars.first();
    if (directBars.size() > 1)
        return nullptr;
    const QList<QTabBar *> bars = tabWidget->findChildren<QTabBar *>();
    return bars.size() == 1 ? bars.first() : nullptr;
}

class QutebrowserChromeShell final : public QObject
{
public:
    explicit QutebrowserChromeShell(QTabWidget *tabWidget)
        : QObject(tabWidget)
        , m_tabWidget(tabWidget)
    {
        qApp->installEventFilter(this);
        ensureSidebar();
        QObject::connect(tabWidget, &QObject::destroyed, this, [this]() {
            qApp->removeEventFilter(this);
        });
        QObject::connect(tabWidget, &QTabWidget::currentChanged, this, [this](int) {
            refresh();
        });
        refresh();
    }

    ~QutebrowserChromeShell() override
    {
        if (qApp)
            qApp->removeEventFilter(this);
    }

    void setModeState(const QString &mode, const QString &keychain, const QString &count)
    {
        m_mode = mode;
        m_keychain = keychain;
        m_count = count;
    }

    void refresh()
    {
        if (!m_tabWidget)
            return;
        ensureSidebar();
        updateTabBarConnection();
        positionSidebar();
        renderSidebar();
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (!m_tabWidget)
            return QObject::eventFilter(watched, event);

        if (watched == m_tabWidget && (event->type() == QEvent::Resize ||
                                       event->type() == QEvent::Show ||
                                       event->type() == QEvent::LayoutRequest)) {
            positionSidebar();
            if (event->type() == QEvent::LayoutRequest)
                refresh();
        }

        if (event->type() != QEvent::KeyPress && event->type() != QEvent::KeyRelease)
            return QObject::eventFilter(watched, event);

        auto *targetWidget = qobject_cast<QWidget *>(watched);
        if (!targetWidget || !isInThisWindow(targetWidget))
            return QObject::eventFilter(watched, event);

        if (handleBrowserTabKey(targetWidget, event))
            return true;

        return QObject::eventFilter(watched, event);
    }

private:
    bool isInThisWindow(QWidget *widget) const
    {
        return widget && m_tabWidget &&
                (widget == m_tabWidget || m_tabWidget->isAncestorOf(widget));
    }

    void ensureSidebar()
    {
        if (!m_tabWidget || m_sidebar)
            return;

        auto *sidebar = new QWidget(m_tabWidget);
        sidebar->setObjectName(QStringLiteral("QutebrowserChromeTabSidebar"));
        sidebar->setAutoFillBackground(true);
        sidebar->setFocusPolicy(Qt::NoFocus);
        sidebar->setAttribute(Qt::WA_TransparentForMouseEvents, true);

        auto *layout = new QVBoxLayout(sidebar);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        layout->addStretch(1);

        sidebar->setStyleSheet(QStringLiteral(
                "QWidget#QutebrowserChromeTabSidebar {"
                "  background: #000a1a; color: #cce7ff;"
                "  border-right: 1px solid #001020;"
                "}"
                "QWidget#QutebrowserChromeTabRow { background: #001020; border: 0px; }"
                "QWidget#QutebrowserChromeTabRow[selected=\"true\"] { background: #1d9bf0; }"
                "QLabel#QutebrowserChromeTabNumber { background: transparent; color: #cce7ff; padding: 0px; border: 0px; }"
                "QLabel#QutebrowserChromeTabText { background: transparent; color: #cce7ff; padding: 0px; border: 0px; }"
                "QWidget#QutebrowserChromeTabRow[selected=\"true\"] QLabel { color: #ffffff; }"
                "QLabel#QutebrowserChromeTabIcon { background: transparent; padding: 0px; border: 0px; }"));

        m_sidebar = sidebar;
        m_sidebarLayout = layout;
        positionSidebar();
        sidebar->show();
        sidebar->raise();
    }

    void positionSidebar()
    {
        if (!m_tabWidget || !m_sidebar)
            return;
        m_sidebar->setGeometry(0, 0, qutebrowserNativeTabSidebarWidth, m_tabWidget->height());
        m_sidebar->raise();
    }

    void updateTabBarConnection()
    {
        QTabBar *tabBar = qutebrowserShellTabBar(m_tabWidget);
        if (m_tabBar == tabBar)
            return;
        QObject::disconnect(m_tabBarMovedConnection);
        QObject::disconnect(m_tabBarCurrentConnection);
        m_tabBarMovedConnection = {};
        m_tabBarCurrentConnection = {};
        m_tabBar = tabBar;
        if (tabBar) {
            m_tabBarMovedConnection = QObject::connect(tabBar, &QTabBar::tabMoved,
                                                       this, [this](int, int) { refresh(); });
            m_tabBarCurrentConnection = QObject::connect(tabBar, &QTabBar::currentChanged,
                                                         this, [this](int) { refresh(); });
        }
    }

    QString textForIndex(int index) const
    {
        if (!m_tabWidget || index < 0 || index >= m_tabWidget->count())
            return QStringLiteral("about:blank");

        // Prefer the browser-process QWebEngineView URL cache so the native
        // sidebar keeps qutebrowser's "[num] <favicon> <link>" shape instead
        // of duplicating qutebrowser's configurable tab-title text. This is a
        // UI-side accessor; it does not round-trip through the renderer.
        QWidget *tabWidgetPage = m_tabWidget->widget(index);
        if (tabWidgetPage) {
            if (auto *view = tabWidgetPage->findChild<QWebEngineView *>()) {
                const QString urlText = qutebrowserTabDisplayUrl(view->url());
                if (!urlText.isEmpty())
                    return urlText;
            }
        }

        QTabBar *tabBar = m_tabBar ? m_tabBar.data() : qutebrowserShellTabBar(m_tabWidget);
        QString text = tabBar ? tabBar->tabText(index) : QString();
        if (!text.isEmpty()) {
            const int colon = text.indexOf(QStringLiteral(": "));
            if (colon > 0)
                text = text.mid(colon + 2);
            const int space = text.indexOf(QLatin1Char(' '));
            if (space > 0 && text.left(space).toInt() == index + 1)
                text = text.mid(space + 1);
            return text.trimmed();
        }
        return QStringLiteral("about:blank");
    }

    void renderSidebar()
    {
        if (!m_sidebar || !m_sidebarLayout || !m_tabWidget)
            return;

        while (m_sidebarLayout->count() > 1) {
            QLayoutItem *item = m_sidebarLayout->takeAt(0);
            if (QWidget *widget = item ? item->widget() : nullptr)
                widget->deleteLater();
            delete item;
        }

        QFont font(QStringLiteral("JetBrains Mono"));
        font.setStyleHint(QFont::Monospace);
        font.setPointSize(9);
        const QFontMetrics metrics(font);
        const int iconExtent = qMax(10, metrics.height() - 2);

        QTabBar *tabBar = m_tabBar ? m_tabBar.data() : qutebrowserShellTabBar(m_tabWidget);
        const int tabCount = tabBar ? tabBar->count() : m_tabWidget->count();
        const int selected = tabBar && tabBar->currentIndex() >= 0
                ? tabBar->currentIndex()
                : m_tabWidget->currentIndex();
        const int numberWidth = metrics.horizontalAdvance(QString::number(qMax(1, tabCount))) + 2;
        const int textWidth = qMax(20, qutebrowserNativeTabSidebarWidth - 10 - numberWidth - 4 - (iconExtent + 4) - 4);

        for (int i = 0; i < tabCount; ++i) {
            QString text = metrics.elidedText(textForIndex(i), Qt::ElideRight, textWidth);

            auto *row = new QWidget(m_sidebar);
            row->setObjectName(QStringLiteral("QutebrowserChromeTabRow"));
            row->setFocusPolicy(Qt::NoFocus);
            row->setFixedHeight(qMax(18, metrics.height() + 2));
            row->setProperty("selected", i == selected);

            auto *rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(5, 0, 5, 0);
            rowLayout->setSpacing(0);

            auto *numberLabel = new QLabel(row);
            numberLabel->setObjectName(QStringLiteral("QutebrowserChromeTabNumber"));
            numberLabel->setTextFormat(Qt::PlainText);
            numberLabel->setFont(font);
            numberLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            numberLabel->setFocusPolicy(Qt::NoFocus);
            numberLabel->setFixedWidth(numberWidth);
            numberLabel->setText(QString::number(i + 1));

            auto *iconLabel = new QLabel(row);
            iconLabel->setObjectName(QStringLiteral("QutebrowserChromeTabIcon"));
            iconLabel->setFocusPolicy(Qt::NoFocus);
            iconLabel->setAlignment(Qt::AlignCenter);
            iconLabel->setFixedSize(iconExtent + 4, iconExtent);
            const QIcon icon = tabBar ? tabBar->tabIcon(i) : QIcon();
            if (!icon.isNull())
                iconLabel->setPixmap(icon.pixmap(iconExtent, iconExtent));

            auto *textLabel = new QLabel(row);
            textLabel->setObjectName(QStringLiteral("QutebrowserChromeTabText"));
            textLabel->setTextFormat(Qt::PlainText);
            textLabel->setFont(font);
            textLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            textLabel->setFocusPolicy(Qt::NoFocus);
            textLabel->setWordWrap(false);
            textLabel->setText(text);

            rowLayout->addWidget(numberLabel);
            rowLayout->addSpacing(4);
            rowLayout->addWidget(iconLabel);
            rowLayout->addSpacing(4);
            rowLayout->addWidget(textLabel, 1);
            m_sidebarLayout->insertWidget(i, row);
        }
    }

    bool editableFocused(QWidget *eventTarget) const
    {
        QWidget *focus = QApplication::focusWidget();
        if (!focus)
            focus = eventTarget;
        if (!isInThisWindow(focus))
            return false;
        if (qobject_cast<QLineEdit *>(focus))
            return true;
        return focus->inputMethodQuery(Qt::ImEnabled).toBool();
    }

    bool handleBrowserTabKey(QWidget *targetWidget, QEvent *event)
    {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (event->type() == QEvent::KeyRelease) {
            if (m_suppressedReleaseKey == keyEvent->key()) {
                m_suppressedReleaseKey = 0;
                event->accept();
                return true;
            }
            return false;
        }

        if (editableFocused(targetWidget))
            return false;
        if (!m_mode.isEmpty() && m_mode != QStringLiteral("normal"))
            return false;
        if (!m_keychain.isEmpty() || !m_count.isEmpty())
            return false;

        const Qt::KeyboardModifiers modifiers = keyEvent->modifiers();
        if ((modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) ||
            !(modifiers & Qt::ShiftModifier))
            return false;

        int delta = 0;
        if (keyEvent->key() == Qt::Key_J)
            delta = 1;
        else if (keyEvent->key() == Qt::Key_K)
            delta = -1;
        else
            return false;

        QTabBar *tabBar = m_tabBar ? m_tabBar.data() : qutebrowserShellTabBar(m_tabWidget);
        int currentIndex = tabBar && tabBar->currentIndex() >= 0
                ? tabBar->currentIndex()
                : m_tabWidget->currentIndex();
        const int count = tabBar ? tabBar->count() : m_tabWidget->count();
        if (count <= 1 || currentIndex < 0)
            return false;

        int targetIndex = currentIndex + delta;
        if (targetIndex < 0)
            targetIndex = count - 1;
        else if (targetIndex >= count)
            targetIndex = 0;

        if (tabBar)
            tabBar->setCurrentIndex(targetIndex);
        m_tabWidget->setCurrentIndex(targetIndex);
        m_suppressedReleaseKey = keyEvent->key();
        refresh();
        event->accept();
        return true;
    }

    QPointer<QTabWidget> m_tabWidget;
    QPointer<QTabBar> m_tabBar;
    QWidget *m_sidebar = nullptr;
    QVBoxLayout *m_sidebarLayout = nullptr;
    QMetaObject::Connection m_tabBarMovedConnection;
    QMetaObject::Connection m_tabBarCurrentConnection;
    QString m_mode = QStringLiteral("normal");
    QString m_keychain;
    QString m_count;
    int m_suppressedReleaseKey = 0;
};

static QHash<QTabWidget *, QPointer<QutebrowserChromeShell>> &qutebrowserChromeShellRegistry()
{
    static QHash<QTabWidget *, QPointer<QutebrowserChromeShell>> registry;
    return registry;
}

static QutebrowserChromeShell *qutebrowserChromeShellForTabWidget(QTabWidget *tabWidget)
{
    if (!tabWidget)
        return nullptr;
    auto &registry = qutebrowserChromeShellRegistry();
    QPointer<QutebrowserChromeShell> shell = registry.value(tabWidget);
    if (!shell) {
        shell = new QutebrowserChromeShell(tabWidget);
        registry.insert(tabWidget, shell);
        QObject::connect(tabWidget, &QObject::destroyed, shell, [tabWidget]() {
            qutebrowserChromeShellRegistry().remove(tabWidget);
        });
    }
    return shell;
}

void QWebEngineViewPrivate::ensureQutebrowserTabSidebar()
{
    Q_Q(QWebEngineView);
    if (auto *layout = qobject_cast<QBoxLayout *>(q->layout()))
        layout->setContentsMargins(qutebrowserNativeTabSidebarWidth, 0, 0, 0);

    int viewIndex = -1;
    QTabWidget *tabWidget = qutebrowserAncestorTabWidget(&viewIndex, nullptr);
    if (QutebrowserChromeShell *shell = qutebrowserChromeShellForTabWidget(tabWidget)) {
        shell->setModeState(m_qutebrowserMode, m_qutebrowserKeychain, m_qutebrowserCount);
        shell->refresh();
    }
}

void QWebEngineViewPrivate::updateQutebrowserTabSidebar()
{
    Q_Q(QWebEngineView);
    if (auto *layout = qobject_cast<QBoxLayout *>(q->layout()))
        layout->setContentsMargins(qutebrowserNativeTabSidebarWidth, 0, 0, 0);

    int viewIndex = -1;
    QTabWidget *tabWidget = qutebrowserAncestorTabWidget(&viewIndex, nullptr);
    if (QutebrowserChromeShell *shell = qutebrowserChromeShellForTabWidget(tabWidget)) {
        shell->setModeState(m_qutebrowserMode, m_qutebrowserKeychain, m_qutebrowserCount);
        shell->refresh();
    }
}

int QWebEngineViewPrivate::qutebrowserChromeLeftInset() const
{
    return qutebrowserAncestorTabWidget(nullptr, nullptr) ? qutebrowserNativeTabSidebarWidth : 0;
}

void QWebEngineViewPrivate::ensureQutebrowserStatusOverlay()
{
    Q_Q(QWebEngineView);
    if (m_qutebrowserStatusOverlay)
        return;

    auto *label = new QLabel(q);
    label->setObjectName(QStringLiteral("QutebrowserChromeStatusbar"));
    label->setAttribute(Qt::WA_TransparentForMouseEvents);
    label->setFocusPolicy(Qt::NoFocus);
    label->setTextFormat(Qt::PlainText);
    label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    label->setMargin(0);
    QFont font(QStringLiteral("JetBrains Mono"));
    font.setStyleHint(QFont::Monospace);
    font.setPointSize(10);
    label->setFont(font);
    label->setFixedHeight(22);
    label->hide();
    m_qutebrowserStatusOverlay = label;
    updateQutebrowserStatusOverlay();
}

void QWebEngineViewPrivate::positionQutebrowserStatusOverlay()
{
    ensureQutebrowserStatusOverlay();
    Q_Q(QWebEngineView);
    const int height = m_qutebrowserStatusOverlay->height() > 0 ? m_qutebrowserStatusOverlay->height() : 22;
    const int leftInset = qutebrowserChromeLeftInset();
    m_qutebrowserStatusOverlay->setGeometry(leftInset, qMax(0, q->height() - height),
                                            qMax(0, q->width() - leftInset), height);
    m_qutebrowserStatusOverlay->raise();
}

QString QWebEngineViewPrivate::qutebrowserScrollText() const
{
    if (m_qutebrowserContentsSize.height() <= 0)
        return QStringLiteral("[0/1]");

    Q_Q(const QWebEngineView);
    const double viewportHeight = q ? q->height() : 0;
    const double maxY = std::max(0.0, m_qutebrowserContentsSize.height() - viewportHeight);
    if (maxY <= 1.0)
        return QStringLiteral("[top]");

    const double y = std::clamp(m_qutebrowserScrollPosition.y(), 0.0, maxY);
    if (y <= 1.0)
        return QStringLiteral("[top]");
    if (maxY - y <= 1.0)
        return QStringLiteral("[bot]");
    return QStringLiteral("[%1%]").arg(static_cast<int>(std::round((y / maxY) * 100.0)));
}

QString QWebEngineViewPrivate::qutebrowserUrlText() const
{
    if (!m_qutebrowserHoveredUrl.isEmpty())
        return m_qutebrowserHoveredUrl;
    if (m_qutebrowserUrl.isEmpty())
        return QString();
    return m_qutebrowserUrl.toDisplayString(QUrl::PreferLocalFile | QUrl::RemovePassword);
}

void QWebEngineViewPrivate::updateQutebrowserStatusOverlay()
{
    ensureQutebrowserStatusOverlay();

    const QString keyText = m_qutebrowserCount + m_qutebrowserKeychain;
    const bool hasMode = m_qutebrowserMode != QStringLiteral("normal") && !m_qutebrowserMode.isEmpty();
    const bool hasKeyText = !keyText.isEmpty();
    const QString urlText = qutebrowserUrlText();
    const QString scrollText = qutebrowserScrollText();

    QString leftText;
    if (hasMode)
        leftText = QStringLiteral("-- %1 MODE --").arg(qutebrowserModeDisplayName(m_qutebrowserMode));
    if (hasKeyText)
        leftText += (leftText.isEmpty() ? QString() : QStringLiteral("  ")) + keyText;

    QString rightText = urlText;
    if (m_qutebrowserLoading && m_qutebrowserLoadProgress < 100)
        rightText += (rightText.isEmpty() ? QString() : QStringLiteral(" ")) + QStringLiteral("[%1%]").arg(m_qutebrowserLoadProgress);
    if (!scrollText.isEmpty())
        rightText += (rightText.isEmpty() ? QString() : QStringLiteral(" ")) + scrollText;
    QString historyText;
    if (m_qutebrowserCanGoBack)
        historyText += QChar(0x25C0);
    if (m_qutebrowserCanGoForward)
        historyText += QChar(0x25B6);
    if (!historyText.isEmpty())
        rightText += (rightText.isEmpty() ? QString() : QStringLiteral(" ")) + historyText;

    if (leftText.isEmpty() && rightText.isEmpty()) {
        m_qutebrowserStatusOverlay->hide();
        return;
    }

    Q_Q(QWebEngineView);
    const int availableWidth = qMax(20, q->width() - qutebrowserChromeLeftInset() - 12);
    const QFontMetrics metrics(m_qutebrowserStatusOverlay->font());
    QString text;
    if (leftText.isEmpty()) {
        text = metrics.elidedText(rightText, Qt::ElideMiddle, availableWidth);
    } else if (rightText.isEmpty()) {
        text = metrics.elidedText(leftText, Qt::ElideRight, availableWidth);
    } else {
        const int minGap = metrics.horizontalAdvance(QStringLiteral("  "));
        QString left = leftText;
        QString right = rightText;
        int leftWidth = metrics.horizontalAdvance(left);
        int rightWidth = metrics.horizontalAdvance(right);
        if (leftWidth + minGap + rightWidth > availableWidth) {
            const int rightBudget = qMax(80, availableWidth - minGap - leftWidth);
            right = metrics.elidedText(right, Qt::ElideMiddle, rightBudget);
            rightWidth = metrics.horizontalAdvance(right);
        }
        if (leftWidth + minGap + rightWidth > availableWidth) {
            const int leftBudget = qMax(80, availableWidth - minGap - rightWidth);
            left = metrics.elidedText(left, Qt::ElideRight, leftBudget);
            leftWidth = metrics.horizontalAdvance(left);
        }
        const int spaces = qMax(2, (availableWidth - leftWidth - rightWidth) / qMax(1, metrics.horizontalAdvance(QLatin1Char(' '))));
        text = left + QString(spaces, QLatin1Char(' ')) + right;
    }

    QString fg = QStringLiteral("#ffffff");
    QString bg = QStringLiteral("#00050f");
    if (m_qutebrowserMode == QStringLiteral("insert")) {
        fg = QStringLiteral("#00050f");
        bg = QStringLiteral("#1d9bf0");
    } else if (m_qutebrowserMode == QStringLiteral("passthrough")) {
        fg = QStringLiteral("#eaf7ff");
        bg = QStringLiteral("#0070b8");
    } else if (m_qutebrowserMode == QStringLiteral("command") ||
               m_qutebrowserMode == QStringLiteral("hint")) {
        fg = QStringLiteral("#ffffff");
        bg = QStringLiteral("#000a1a");
    } else if (m_qutebrowserMode == QStringLiteral("prompt") ||
               m_qutebrowserMode == QStringLiteral("yesno")) {
        fg = QStringLiteral("#ffffff");
        bg = QStringLiteral("#001020");
    }

    m_qutebrowserStatusOverlay->setStyleSheet(QStringLiteral(
            "QLabel#QutebrowserChromeStatusbar {"
            "background: %1; color: %2; padding-left: 6px; padding-right: 6px;"
            "border: 0px; }"
    ).arg(bg, fg));
    m_qutebrowserStatusOverlay->setText(text);
    positionQutebrowserStatusOverlay();
    m_qutebrowserStatusOverlay->show();
    if (m_qutebrowserFindOverlay && m_qutebrowserFindActive) {
        positionQutebrowserFindOverlay();
        m_qutebrowserFindOverlay->show();
        m_qutebrowserFindOverlay->raise();
    }
    if (m_qutebrowserCommandLineOverlay && m_qutebrowserCommandLineActive) {
        positionQutebrowserCommandLineOverlay();
        m_qutebrowserCommandLineOverlay->show();
        m_qutebrowserCommandLineOverlay->raise();
    }
}

void QWebEngineViewPrivate::onQutebrowserModeChanged(const QString &oldMode, const QString &newMode)
{
    Q_UNUSED(oldMode);
    m_qutebrowserMode = newMode;
    m_qutebrowserKeychain.clear();
    m_qutebrowserCount.clear();
    if (m_webEngineWidget) {
        m_webEngineWidget->setProperty("qutebrowserMode", m_qutebrowserMode);
        m_webEngineWidget->setProperty("qutebrowserKeychain", m_qutebrowserKeychain);
        m_webEngineWidget->setProperty("qutebrowserCount", m_qutebrowserCount);
    }
    updateQutebrowserTabSidebar();
    updateQutebrowserStatusOverlay();
}

void QWebEngineViewPrivate::onQutebrowserStatusChanged(const QString &mode, const QString &keychain, const QString &count)
{
    if (!mode.isEmpty())
        m_qutebrowserMode = mode;
    m_qutebrowserKeychain = keychain;
    m_qutebrowserCount = count;
    if (m_webEngineWidget) {
        m_webEngineWidget->setProperty("qutebrowserMode", m_qutebrowserMode);
        m_webEngineWidget->setProperty("qutebrowserKeychain", m_qutebrowserKeychain);
        m_webEngineWidget->setProperty("qutebrowserCount", m_qutebrowserCount);
    }
    updateQutebrowserTabSidebar();
    updateQutebrowserStatusOverlay();
}

QTabWidget *QWebEngineViewPrivate::qutebrowserAncestorTabWidget(int *viewIndex,
                                                               int *currentIndex) const
{
    if (viewIndex)
        *viewIndex = -1;
    if (currentIndex)
        *currentIndex = -1;

    const QWidget *view = q_ptr;
    if (!view)
        return nullptr;

    for (QWidget *candidate = q_ptr; candidate; candidate = candidate->parentWidget()) {
        auto *tabWidget = qobject_cast<QTabWidget *>(candidate);
        if (!tabWidget)
            continue;

        for (int index = 0; index < tabWidget->count(); ++index) {
            QWidget *pageWidget = tabWidget->widget(index);
            if (!pageWidget)
                continue;
            if (pageWidget == view || pageWidget->isAncestorOf(view)) {
                if (viewIndex)
                    *viewIndex = index;
                if (currentIndex)
                    *currentIndex = tabWidget->currentIndex();
                return tabWidget;
            }
        }
    }

    return nullptr;
}

QTabBar *QWebEngineViewPrivate::qutebrowserAncestorTabBar(QTabWidget *tabWidget) const
{
    if (!tabWidget)
        return nullptr;

    const QList<QTabBar *> directBars = tabWidget->findChildren<QTabBar *>(QString(),
                                                                          Qt::FindDirectChildrenOnly);
    if (directBars.size() == 1)
        return directBars.first();
    if (directBars.size() > 1)
        return nullptr;

    const QList<QTabBar *> bars = tabWidget->findChildren<QTabBar *>();
    return bars.size() == 1 ? bars.first() : nullptr;
}

bool QWebEngineViewPrivate::qutebrowserSetCurrentTabIndex(int targetIndex)
{
    int viewIndex = -1;
    int currentIndex = -1;
    QTabWidget *tabWidget = qutebrowserAncestorTabWidget(&viewIndex, &currentIndex);
    if (!tabWidget || viewIndex < 0 || currentIndex < 0 || viewIndex != tabWidget->currentIndex())
        return false;

    QTabBar *tabBar = qutebrowserAncestorTabBar(tabWidget);
    if (tabBar && tabBar->currentIndex() >= 0)
        currentIndex = tabBar->currentIndex();

    const int count = tabBar ? tabBar->count() : tabWidget->count();
    if (targetIndex < 0 || targetIndex >= count || targetIndex == currentIndex)
        return false;

    if (tabBar)
        tabBar->setCurrentIndex(targetIndex);
    tabWidget->setCurrentIndex(targetIndex);
    return true;
}

bool QWebEngineViewPrivate::qutebrowserMoveCurrentTab(int targetIndex)
{
    int viewIndex = -1;
    int currentIndex = -1;
    QTabWidget *tabWidget = qutebrowserAncestorTabWidget(&viewIndex, &currentIndex);
    if (!tabWidget || viewIndex < 0 || currentIndex < 0 || viewIndex != tabWidget->currentIndex())
        return false;

    QTabBar *tabBar = qutebrowserAncestorTabBar(tabWidget);
    if (!tabBar || tabBar->count() != tabWidget->count())
        return false;
    if (tabBar->currentIndex() >= 0)
        currentIndex = tabBar->currentIndex();

    if (targetIndex < 0 || targetIndex >= tabBar->count())
        return false;

    if (targetIndex != currentIndex)
        tabBar->moveTab(currentIndex, targetIndex);
    return true;
}

bool QWebEngineViewPrivate::qutebrowserHandleTabCommand(const QString &name,
                                                        const QString &arguments)
{
    if (name != QStringLiteral("tab-next") && name != QStringLiteral("tab-prev") &&
        name != QStringLiteral("tab-focus") && name != QStringLiteral("tab-move"))
        return false;

    int viewIndex = -1;
    int currentIndex = -1;
    QTabWidget *tabWidget = qutebrowserAncestorTabWidget(&viewIndex, &currentIndex);
    if (!tabWidget || viewIndex < 0 || currentIndex < 0 || viewIndex != tabWidget->currentIndex())
        return false;

    QTabBar *tabBar = qutebrowserAncestorTabBar(tabWidget);
    if (tabBar && tabBar->currentIndex() >= 0)
        currentIndex = tabBar->currentIndex();

    const int count = tabBar ? tabBar->count() : tabWidget->count();
    const QStringList tokens = qutebrowserCommandArgumentTokens(arguments);

    if (name == QStringLiteral("tab-next") || name == QStringLiteral("tab-prev")) {
        if (!tokens.isEmpty())
            return false;
        const int delta = name == QStringLiteral("tab-next") ? 1 : -1;
        const int targetIndex = currentIndex + delta;
        if (targetIndex < 0 || targetIndex >= count)
            return false;
        return qutebrowserSetCurrentTabIndex(targetIndex);
    }

    if (name == QStringLiteral("tab-focus")) {
        if (tokens.size() != 1)
            return false;

        int index = 0;
        if (!qutebrowserParseStrictInteger(tokens.first(), &index))
            return false;

        int targetIndex = -1;
        if (index == -1) {
            targetIndex = count - 1;
        } else if (index > 0) {
            targetIndex = index - 1;
        } else {
            return false;
        }

        if (targetIndex < 0 || targetIndex >= count || targetIndex == currentIndex)
            return false;
        return qutebrowserSetCurrentTabIndex(targetIndex);
    }

    if (name == QStringLiteral("tab-move")) {
        if (tokens.size() > 1)
            return false;

        int targetIndex = 0;
        if (tokens.isEmpty() || tokens.first() == QStringLiteral("start")) {
            targetIndex = 0;
        } else if (tokens.first() == QStringLiteral("end")) {
            targetIndex = count - 1;
        } else if (tokens.first() == QStringLiteral("+")) {
            targetIndex = currentIndex + 1;
        } else if (tokens.first() == QStringLiteral("-")) {
            targetIndex = currentIndex - 1;
        } else {
            int index = 0;
            if (!qutebrowserParseStrictInteger(tokens.first(), &index) || index == 0)
                return false;
            targetIndex = index > 0 ? index - 1 : count + index;
        }

        if (targetIndex < 0 || targetIndex >= count)
            return false;
        return qutebrowserMoveCurrentTab(targetIndex);
    }

    return false;
}

bool QWebEngineViewPrivate::qutebrowserHandleCommand(const QString &command)
{
    const QString name = qutebrowserCommandName(command);
    if (qutebrowserHandleTabCommand(name, qutebrowserCommandArgument(command)))
        return true;

    if (name == QStringLiteral("cmd-set-text")) {
        const QutebrowserCmdSetTextPreset preset =
                qutebrowserParseCmdSetTextPreset(qutebrowserCommandArgument(command));
        if (!preset.valid)
            return false;

        QString text = preset.text;
        if (preset.space)
            text += QLatin1Char(' ');
        if (preset.append) {
            if (!m_qutebrowserCommandLineActive)
                return false;
            text = qutebrowserCommandLineText() + text;
        }
        text = expandQutebrowserCommandLinePlaceholders(text);

        if (text == QStringLiteral("/")) {
            startQutebrowserFind(false);
            return true;
        }
        if (text == QStringLiteral("?")) {
            startQutebrowserFind(true);
            return true;
        }
        if (text.startsWith(QLatin1Char(':'))) {
            startQutebrowserCommandLine(text);
            return true;
        }
        return false;
    }

    if (name == QStringLiteral("search-next")) {
        if (m_qutebrowserFindText.isEmpty() && !m_qutebrowserFindActive)
            return false;
        navigateQutebrowserFind(false);
        return true;
    }
    if (name == QStringLiteral("search-prev")) {
        if (m_qutebrowserFindText.isEmpty() && !m_qutebrowserFindActive)
            return false;
        navigateQutebrowserFind(true);
        return true;
    }
    if (name == QStringLiteral("search") && qutebrowserCommandArgument(command).isEmpty()) {
        if (m_qutebrowserFindText.isEmpty() && !m_qutebrowserFindActive)
            return false;
        clearQutebrowserFind();
        return true;
    }

    return false;
}

void QWebEngineViewPrivate::ensureQutebrowserCommandLineOverlay()
{
    Q_Q(QWebEngineView);
    if (m_qutebrowserCommandLineOverlay)
        return;

    auto *overlay = new QWidget(q);
    overlay->setObjectName(QStringLiteral("QutebrowserChromeCommandLine"));
    overlay->setAutoFillBackground(true);
    overlay->setFixedHeight(24);
    overlay->hide();

    QFont font(QStringLiteral("JetBrains Mono"));
    font.setStyleHint(QFont::Monospace);
    font.setPointSize(10);

    auto *layout = new QHBoxLayout(overlay);
    layout->setContentsMargins(6, 0, 6, 0);
    layout->setSpacing(4);

    auto *prefix = new QLabel(QStringLiteral(":"), overlay);
    prefix->setObjectName(QStringLiteral("QutebrowserChromeCommandPrefix"));
    prefix->setFont(font);
    prefix->setFixedWidth(QFontMetrics(font).horizontalAdvance(QStringLiteral(":")) + 4);
    prefix->setAlignment(Qt::AlignCenter);

    auto *lineEdit = new QLineEdit(overlay);
    lineEdit->setObjectName(QStringLiteral("QutebrowserChromeCommandInput"));
    lineEdit->setFont(font);
    lineEdit->setFrame(false);
    lineEdit->setClearButtonEnabled(false);
    lineEdit->setPlaceholderText(QStringLiteral("command"));

    layout->addWidget(prefix);
    layout->addWidget(lineEdit, 1);

    overlay->setStyleSheet(QStringLiteral(
            "QWidget#QutebrowserChromeCommandLine {"
            "  background: #000a1a; color: #ffffff;"
            "  border-top: 1px solid #1d9bf0;"
            "}"
            "QLabel#QutebrowserChromeCommandPrefix { color: #4fd0ff; }"
            "QLineEdit#QutebrowserChromeCommandInput {"
            "  background: #000a1a; color: #ffffff; border: 0;"
            "  selection-background-color: #1d9bf0;"
            "  selection-color: #00050f;"
            "  placeholder-text-color: #cce7ff;"
            "}"
            "QLineEdit#QutebrowserChromeCommandInput:!focus { color: #cce7ff; }"));

    QObject::connect(lineEdit, &QLineEdit::returnPressed, q, [this]() {
        acceptQutebrowserCommandLine();
    });

    auto *escapeShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), overlay);
    escapeShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    QObject::connect(escapeShortcut, &QShortcut::activated, q, [this]() {
        cancelQutebrowserCommandLine();
    });

    m_qutebrowserCommandLineOverlay = overlay;
    m_qutebrowserCommandLinePrefixLabel = prefix;
    m_qutebrowserCommandLineEdit = lineEdit;
}

void QWebEngineViewPrivate::positionQutebrowserCommandLineOverlay()
{
    ensureQutebrowserCommandLineOverlay();
    Q_Q(QWebEngineView);
    const int height = m_qutebrowserCommandLineOverlay->height() > 0
            ? m_qutebrowserCommandLineOverlay->height() : 24;
    const int leftInset = qutebrowserChromeLeftInset();
    m_qutebrowserCommandLineOverlay->setGeometry(leftInset, qMax(0, q->height() - height),
                                                 qMax(0, q->width() - leftInset), height);
    m_qutebrowserCommandLineOverlay->raise();
}

void QWebEngineViewPrivate::startQutebrowserCommandLine(const QString &text)
{
    ensureQutebrowserCommandLineOverlay();

    if (m_qutebrowserFindOverlay && m_qutebrowserFindActive) {
        m_qutebrowserFindActive = false;
        m_qutebrowserFindOverlay->hide();
    }

    m_qutebrowserCommandLineActive = true;
    if (m_qutebrowserCommandLinePrefixLabel)
        m_qutebrowserCommandLinePrefixLabel->setText(QStringLiteral(":"));
    if (m_qutebrowserCommandLineEdit) {
        QSignalBlocker blocker(m_qutebrowserCommandLineEdit);
        const QString contents = text.startsWith(QLatin1Char(':')) ? text.mid(1) : text;
        m_qutebrowserCommandLineEdit->setText(contents);
        m_qutebrowserCommandLineEdit->setCursorPosition(contents.size());
    }

    positionQutebrowserCommandLineOverlay();
    m_qutebrowserCommandLineOverlay->show();
    m_qutebrowserCommandLineOverlay->raise();
    if (m_qutebrowserCommandLineEdit)
        m_qutebrowserCommandLineEdit->setFocus(Qt::ShortcutFocusReason);
}

void QWebEngineViewPrivate::acceptQutebrowserCommandLine()
{
    const QString command = m_qutebrowserCommandLineEdit
            ? m_qutebrowserCommandLineEdit->text() : QString();
    hideQutebrowserCommandLine();
    focusContainer();
    executeQutebrowserCommandLineCommand(command);
}

void QWebEngineViewPrivate::cancelQutebrowserCommandLine()
{
    hideQutebrowserCommandLine();
    focusContainer();
}

void QWebEngineViewPrivate::hideQutebrowserCommandLine()
{
    m_qutebrowserCommandLineActive = false;
    if (m_qutebrowserCommandLineEdit) {
        QSignalBlocker blocker(m_qutebrowserCommandLineEdit);
        m_qutebrowserCommandLineEdit->clear();
    }
    if (m_qutebrowserCommandLineOverlay)
        m_qutebrowserCommandLineOverlay->hide();
}

void QWebEngineViewPrivate::executeQutebrowserCommandLineCommand(const QString &command)
{
    if (qutebrowserHandleCommand(command))
        return;
    if (page)
        Q_EMIT page->qutebrowserCommandRequested(command);
}

QString QWebEngineViewPrivate::qutebrowserCommandLineText() const
{
    return QStringLiteral(":") + (m_qutebrowserCommandLineEdit
            ? m_qutebrowserCommandLineEdit->text() : QString());
}

QString QWebEngineViewPrivate::qutebrowserCurrentUrlText() const
{
    const QUrl currentUrl = page ? page->url() : m_qutebrowserUrl;
    if (currentUrl.isEmpty())
        return QString();
    return currentUrl.toDisplayString(QUrl::PreferLocalFile | QUrl::RemovePassword);
}

QString QWebEngineViewPrivate::expandQutebrowserCommandLinePlaceholders(QString text) const
{
    const QString urlText = qutebrowserCurrentUrlText();
    text.replace(QStringLiteral("{url:pretty}"), urlText);
    text.replace(QStringLiteral("{url:yank}"), urlText);
    text.replace(QStringLiteral("{url}"), urlText);
    return text;
}

void QWebEngineViewPrivate::ensureQutebrowserFindOverlay()
{
    Q_Q(QWebEngineView);
    if (m_qutebrowserFindOverlay)
        return;

    auto *overlay = new QWidget(q);
    overlay->setObjectName(QStringLiteral("QutebrowserChromeFindbar"));
    overlay->setAutoFillBackground(true);
    overlay->setFixedHeight(24);
    overlay->hide();

    QFont font(QStringLiteral("JetBrains Mono"));
    font.setStyleHint(QFont::Monospace);
    font.setPointSize(10);

    auto *layout = new QHBoxLayout(overlay);
    layout->setContentsMargins(6, 0, 6, 0);
    layout->setSpacing(6);

    auto *prefix = new QLabel(QStringLiteral("/"), overlay);
    prefix->setObjectName(QStringLiteral("QutebrowserChromeFindPrefix"));
    prefix->setFont(font);
    prefix->setFixedWidth(QFontMetrics(font).horizontalAdvance(QStringLiteral("?")) + 2);
    prefix->setAlignment(Qt::AlignCenter);

    auto *lineEdit = new QLineEdit(overlay);
    lineEdit->setObjectName(QStringLiteral("QutebrowserChromeFindInput"));
    lineEdit->setFont(font);
    lineEdit->setFrame(false);
    lineEdit->setClearButtonEnabled(false);
    lineEdit->setPlaceholderText(QStringLiteral("search"));

    auto *count = new QLabel(overlay);
    count->setObjectName(QStringLiteral("QutebrowserChromeFindCount"));
    count->setFont(font);
    count->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    count->setMinimumWidth(QFontMetrics(font).horizontalAdvance(QStringLiteral("000/000")));

    layout->addWidget(prefix);
    layout->addWidget(lineEdit, 1);
    layout->addWidget(count);

    overlay->setStyleSheet(QStringLiteral(
            "QWidget#QutebrowserChromeFindbar {"
            "  background: #000a1a; color: #ffffff;"
            "  border-top: 1px solid #1d9bf0;"
            "}"
            "QLabel#QutebrowserChromeFindPrefix { color: #4fd0ff; }"
            "QLabel#QutebrowserChromeFindCount { color: #cce7ff; }"
            "QLineEdit#QutebrowserChromeFindInput {"
            "  background: #000a1a; color: #ffffff; border: 0;"
            "  selection-background-color: #1d9bf0;"
            "  selection-color: #00050f;"
            "}"
            "QLineEdit#QutebrowserChromeFindInput:!focus { color: #cce7ff; }"));

    QObject::connect(lineEdit, &QLineEdit::textChanged, q, [this]() {
        updateQutebrowserFindFromInput();
    });
    QObject::connect(lineEdit, &QLineEdit::returnPressed, q, [this]() {
        acceptQutebrowserFind();
    });

    auto *escapeShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), overlay);
    escapeShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    QObject::connect(escapeShortcut, &QShortcut::activated, q, [this]() {
        cancelQutebrowserFind();
    });

    m_qutebrowserFindOverlay = overlay;
    m_qutebrowserFindPrefixLabel = prefix;
    m_qutebrowserFindLineEdit = lineEdit;
    m_qutebrowserFindCountLabel = count;
    updateQutebrowserFindOverlay();
}

void QWebEngineViewPrivate::positionQutebrowserFindOverlay()
{
    ensureQutebrowserFindOverlay();
    Q_Q(QWebEngineView);
    const int height = m_qutebrowserFindOverlay->height() > 0 ? m_qutebrowserFindOverlay->height() : 24;
    const int leftInset = qutebrowserChromeLeftInset();
    m_qutebrowserFindOverlay->setGeometry(leftInset, qMax(0, q->height() - height),
                                          qMax(0, q->width() - leftInset), height);
    m_qutebrowserFindOverlay->raise();
}

void QWebEngineViewPrivate::startQutebrowserFind(bool reverse)
{
    ensureQutebrowserFindOverlay();
    if (m_qutebrowserCommandLineActive)
        hideQutebrowserCommandLine();
    m_qutebrowserFindActive = true;
    m_qutebrowserFindReverse = reverse;
    m_qutebrowserFindActiveMatch = 0;
    m_qutebrowserFindTotalMatches = 0;

    if (m_qutebrowserFindPrefixLabel)
        m_qutebrowserFindPrefixLabel->setText(reverse ? QStringLiteral("?") : QStringLiteral("/"));
    if (m_qutebrowserFindLineEdit) {
        QSignalBlocker blocker(m_qutebrowserFindLineEdit);
        m_qutebrowserFindLineEdit->setText(m_qutebrowserFindText);
        m_qutebrowserFindLineEdit->selectAll();
    }
    updateQutebrowserFindOverlay();
    positionQutebrowserFindOverlay();
    m_qutebrowserFindOverlay->show();
    m_qutebrowserFindOverlay->raise();
    if (m_qutebrowserFindLineEdit)
        m_qutebrowserFindLineEdit->setFocus(Qt::ShortcutFocusReason);
}

void QWebEngineViewPrivate::acceptQutebrowserFind()
{
    if (m_qutebrowserFindLineEdit)
        m_qutebrowserFindText = m_qutebrowserFindLineEdit->text();
    m_qutebrowserFindActive = false;
    if (m_qutebrowserFindOverlay)
        m_qutebrowserFindOverlay->hide();
    focusContainer();
}

void QWebEngineViewPrivate::cancelQutebrowserFind()
{
    clearQutebrowserFind();
    focusContainer();
}

void QWebEngineViewPrivate::clearQutebrowserFind()
{
    m_qutebrowserFindActive = false;
    m_qutebrowserFindReverse = false;
    m_qutebrowserFindText.clear();
    m_qutebrowserFindActiveMatch = 0;
    m_qutebrowserFindTotalMatches = 0;
    if (m_qutebrowserFindLineEdit) {
        QSignalBlocker blocker(m_qutebrowserFindLineEdit);
        m_qutebrowserFindLineEdit->clear();
    }
    if (m_qutebrowserFindOverlay)
        m_qutebrowserFindOverlay->hide();
    if (page)
        page->findText(QString());
}

void QWebEngineViewPrivate::updateQutebrowserFindFromInput()
{
    if (!m_qutebrowserFindLineEdit)
        return;
    m_qutebrowserFindText = m_qutebrowserFindLineEdit->text();
    if (!page)
        return;

    if (m_qutebrowserFindText.isEmpty()) {
        m_qutebrowserFindActiveMatch = 0;
        m_qutebrowserFindTotalMatches = 0;
        page->findText(QString());
        updateQutebrowserFindOverlay();
        return;
    }

    QWebEnginePage::FindFlags flags;
    if (m_qutebrowserFindReverse)
        flags |= QWebEnginePage::FindBackward;
    page->findText(m_qutebrowserFindText, flags);
    updateQutebrowserFindOverlay();
}

void QWebEngineViewPrivate::navigateQutebrowserFind(bool reverse)
{
    const QString text = m_qutebrowserFindLineEdit && m_qutebrowserFindActive
            ? m_qutebrowserFindLineEdit->text()
            : m_qutebrowserFindText;
    if (text.isEmpty() || !page)
        return;

    m_qutebrowserFindText = text;
    const bool backward = reverse ? !m_qutebrowserFindReverse : m_qutebrowserFindReverse;
    QWebEnginePage::FindFlags flags;
    if (backward)
        flags |= QWebEnginePage::FindBackward;
    page->findText(text, flags);
}

void QWebEngineViewPrivate::onQutebrowserFindFinished(const QWebEngineFindTextResult &result)
{
    m_qutebrowserFindActiveMatch = result.activeMatch();
    m_qutebrowserFindTotalMatches = result.numberOfMatches();
    if (m_qutebrowserFindActive)
        updateQutebrowserFindOverlay();
}

void QWebEngineViewPrivate::updateQutebrowserFindOverlay()
{
    if (!m_qutebrowserFindOverlay || !m_qutebrowserFindLineEdit || !m_qutebrowserFindCountLabel)
        return;

    if (m_qutebrowserFindPrefixLabel)
        m_qutebrowserFindPrefixLabel->setText(m_qutebrowserFindReverse ? QStringLiteral("?") : QStringLiteral("/"));

    QString countText;
    if (!m_qutebrowserFindLineEdit->text().isEmpty()) {
        if (m_qutebrowserFindTotalMatches > 0)
            countText = QStringLiteral("%1/%2").arg(m_qutebrowserFindActiveMatch).arg(m_qutebrowserFindTotalMatches);
        else
            countText = QStringLiteral("0/0");
    }
    m_qutebrowserFindCountLabel->setText(countText);
}

void QWebEngineViewPrivate::widgetChanged(QtWebEngineCore::WebEngineQuickWidget *oldWidget,
                                          QtWebEngineCore::WebEngineQuickWidget *newWidget)
{
    Q_Q(QWebEngineView);

    bool hasFocus = oldWidget ? oldWidget->hasFocus() : false;
    if (oldWidget) {
        if (m_webEngineWidget == oldWidget)
            m_webEngineWidget = nullptr;
        q->layout()->removeWidget(oldWidget);
        oldWidget->hide();
#if QT_CONFIG(accessibility)
        if (!QtWebEngineCore::closingDown())
            QAccessible::deleteAccessibleInterface(
                    QAccessible::uniqueId(QAccessible::queryAccessibleInterface(oldWidget)));
#endif
    }

    if (newWidget) {
        Q_ASSERT(!QtWebEngineCore::closingDown());
#if QT_CONFIG(accessibility)
        QAccessible::deleteAccessibleInterface(QAccessible::uniqueId(QAccessible::queryAccessibleInterface(newWidget)));
        QAccessible::registerAccessibleInterface(new QtWebEngineCore::RenderWidgetHostViewQtDelegateWidgetAccessible(newWidget, q));
#endif
        ensureQutebrowserTabSidebar();
        if (auto *boxLayout = qobject_cast<QBoxLayout *>(q->layout()))
            boxLayout->addWidget(newWidget, 1);
        else
            q->layout()->addWidget(newWidget);
        m_webEngineWidget = newWidget;
        m_webEngineWidget->setProperty("qutebrowserMode", m_qutebrowserMode);
        m_webEngineWidget->setProperty("qutebrowserKeychain", m_qutebrowserKeychain);
        m_webEngineWidget->setProperty("qutebrowserCount", m_qutebrowserCount);
        q->setFocusProxy(newWidget);
        if (hasFocus)
            newWidget->setFocus();
        newWidget->show();
    }

    positionQutebrowserStatusOverlay();
    if (m_qutebrowserFindOverlay)
        positionQutebrowserFindOverlay();
    if (m_qutebrowserCommandLineOverlay)
        positionQutebrowserCommandLineOverlay();
}

void QWebEngineViewPrivate::contextMenuRequested(QWebEngineContextMenuRequest *request)
{
#if QT_CONFIG(action)
    m_contextRequest = request;
    switch (q_ptr->contextMenuPolicy()) {
    case Qt::DefaultContextMenu: {
        QContextMenuEvent event(QContextMenuEvent::Mouse, request->position(),
                                q_ptr->mapToGlobal(request->position()));
        q_ptr->contextMenuEvent(&event);
        return;
    }
    case Qt::CustomContextMenu:
        Q_EMIT q_ptr->customContextMenuRequested(request->position());
        return;
    case Qt::ActionsContextMenu:
        if (q_ptr->actions().size()) {
            QContextMenuEvent event(QContextMenuEvent::Mouse, request->position(),
                                    q_ptr->mapToGlobal(request->position()));
            QMenu::exec(q_ptr->actions(), event.globalPos(), 0, q_ptr);
        }
        return;
    case Qt::PreventContextMenu:
    case Qt::NoContextMenu:
        return;
    }

    Q_UNREACHABLE();
#else
    Q_UNUSED(request);
#endif // QT_CONFIG(action)
}

QStringList QWebEngineViewPrivate::chooseFiles(QWebEnginePage::FileSelectionMode mode,
                                               const QStringList &oldFiles,
                                               const QStringList &acceptedMimeTypes)
{
#if QT_CONFIG(filedialog)
    Q_Q(QWebEngineView);
    const QStringList &filter =
            QtWebEngineCore::FilePickerController::nameFilters(acceptedMimeTypes);
    QStringList ret;
    QString str;
    switch (static_cast<QtWebEngineCore::FilePickerController::FileChooserMode>(mode)) {
    case QtWebEngineCore::FilePickerController::OpenDirectory:
        Q_FALLTHROUGH();
    case QtWebEngineCore::FilePickerController::OpenMultiple:
        ret = QFileDialog::getOpenFileNames(q, QString(), QString(),
                                            filter.join(QStringLiteral(";;")), nullptr,
                                            QFileDialog::HideNameFilterDetails);
        break;
    case QtWebEngineCore::FilePickerController::UploadFolder:
        str = QFileDialog::getExistingDirectory(q, QWebEngineView::tr("Select folder to upload"));
        if (!str.isNull())
            ret << str;
        break;
    case QtWebEngineCore::FilePickerController::Save:
        str = QFileDialog::getSaveFileName(
                q, QString(),
                (QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)
                 + oldFiles.first()));
        if (!str.isNull())
            ret << str;
        break;
    case QtWebEngineCore::FilePickerController::Open:
        str = QFileDialog::getOpenFileName(q, QString(), oldFiles.first(),
                                           filter.join(QStringLiteral(";;")), nullptr,
                                           QFileDialog::HideNameFilterDetails);
        if (!str.isNull())
            ret << str;
        break;
    }
    return ret;
#else
    Q_UNUSED(mode);
    Q_UNUSED(oldFiles);
    Q_UNUSED(acceptedMimeTypes);

    return QStringList();
#endif // QT_CONFIG(filedialog)
}

void QWebEngineViewPrivate::showColorDialog(
        QSharedPointer<QtWebEngineCore::ColorChooserController> controller)
{
#if QT_CONFIG(colordialog)
    Q_Q(QWebEngineView);
    QColorDialog *dialog = new QColorDialog(controller.data()->initialColor(), q);

    QColorDialog::connect(dialog, SIGNAL(colorSelected(QColor)), controller.data(),
                          SLOT(accept(QColor)));
    QColorDialog::connect(dialog, SIGNAL(rejected()), controller.data(), SLOT(reject()));

    // Delete when done
    QColorDialog::connect(dialog, SIGNAL(colorSelected(QColor)), dialog, SLOT(deleteLater()));
    QColorDialog::connect(dialog, SIGNAL(rejected()), dialog, SLOT(deleteLater()));

#if defined(Q_OS_MACOS)
    dialog->setOption(QColorDialog::DontUseNativeDialog);
#endif

    dialog->open();
#else
    Q_UNUSED(controller);
#endif
}

bool QWebEngineViewPrivate::showAuthorizationDialog(const QString &title, const QString &message)
{
#if QT_CONFIG(messagebox)
    Q_Q(QWebEngineView);
    QMessageBox msgBox(QMessageBox::Question, title, message, QMessageBox::Yes | QMessageBox::No,
                       q, Qt::Dialog | Qt::MSWindowsFixedSizeDialogHint);
    msgBox.setTextFormat(Qt::PlainText);
    return msgBox.exec() == QMessageBox::Yes;
#else
    return false;
#endif // QT_CONFIG(messagebox)
}

void QWebEngineViewPrivate::javaScriptAlert(const QUrl &url, const QString &msg)
{
#if QT_CONFIG(messagebox)
    Q_Q(QWebEngineView);
    QMessageBox msgBox(QMessageBox::Information,
                       QStringLiteral("Javascript Alert - %1").arg(url.toString()),
                       msg, QMessageBox::Ok, q,
                       Qt::Dialog | Qt::MSWindowsFixedSizeDialogHint);
    msgBox.setTextFormat(Qt::PlainText);
    msgBox.exec();
#else
    Q_UNUSED(msg);
#endif // QT_CONFIG(messagebox)
}

bool QWebEngineViewPrivate::javaScriptConfirm(const QUrl &url, const QString &msg)
{
#if QT_CONFIG(messagebox)
    Q_Q(QWebEngineView);
    QMessageBox msgBox(QMessageBox::Information,
                       QStringLiteral("Javascript Confirm - %1").arg(url.toString()),
                       msg, QMessageBox::Ok | QMessageBox::Cancel, q,
                       Qt::Dialog | Qt::MSWindowsFixedSizeDialogHint);
    msgBox.setTextFormat(Qt::PlainText);
    return msgBox.exec() == QMessageBox::Ok;
#else
    Q_UNUSED(msg);
    return false;
#endif // QT_CONFIG(messagebox)
}

bool QWebEngineViewPrivate::javaScriptPrompt(const QUrl &url, const QString &msg,
                                             const QString &defaultValue, QString *result)
{
#if QT_CONFIG(inputdialog)
    Q_Q(QWebEngineView);
    bool ret = false;

    // Workaround: Do not interpret text as Qt::RichText
    // QInputDialog uses Qt::AutoText that interprets the text string as
    // Qt::RichText if Qt::mightBeRichText() returns true, otherwise as Qt::PlainText.
    const QString message = Qt::mightBeRichText(msg) ? msg.toHtmlEscaped() : msg;

    if (result)
        *result = QInputDialog::getText(
                q, QStringLiteral("Javascript Prompt - %1").arg(url.toString()),
                message, QLineEdit::Normal, defaultValue, &ret);
    return ret;
#else
    Q_UNUSED(msg);
    Q_UNUSED(defaultValue);
    Q_UNUSED(result);
    return false;
#endif // QT_CONFIG(inputdialog)
}

void QWebEngineViewPrivate::focusContainer()
{
    Q_Q(QWebEngineView);
    q->activateWindow();
    q->setFocus();
}

void QWebEngineViewPrivate::unhandledKeyEvent(QKeyEvent *event)
{
    Q_Q(QWebEngineView);
    if (q->parentWidget())
        QGuiApplication::sendEvent(q->parentWidget(), event);
}

bool QWebEngineViewPrivate::passOnFocus(bool reverse)
{
    Q_Q(QWebEngineView);
    return q->focusNextPrevChild(!reverse);
}

#if QT_CONFIG(accessibility)
static QAccessibleInterface *webAccessibleFactory(const QString &, QObject *object)
{
    if (QWebEngineView *v = qobject_cast<QWebEngineView*>(object))
        return new QWebEngineViewAccessible(v);
    return nullptr;
}
#endif // QT_CONFIG(accessibility)

QWebEngineViewPrivate::QWebEngineViewPrivate()
    : page(nullptr)
    , m_dragEntered(false)
    , m_ownsPage(false)
    , m_contextRequest(nullptr)
{
#if QT_CONFIG(accessibility)
    QAccessible::installFactory(&webAccessibleFactory);
#endif // QT_CONFIG(accessibility)
}

QWebEngineViewPrivate::~QWebEngineViewPrivate() = default;

// static
void QWebEngineViewPrivate::bindPageAndView(QWebEnginePage *page, QWebEngineView *view)
{
    QWebEngineViewPrivate *v =
            page ? static_cast<QWebEngineViewPrivate *>(page->d_func()->view) : nullptr;
    auto oldView = v ? v->q_func() : nullptr;
    auto oldPage = view ? view->d_func()->page : nullptr;

    bool ownNewPage = false;
    bool deleteOldPage = false;

    // Change pointers first.

    if (page && oldView != view) {
        if (oldView) {
            ownNewPage = oldView->d_func()->m_ownsPage;
            oldView->d_func()->page = nullptr;
            oldView->d_func()->m_ownsPage = false;
        }
        page->d_func()->view = view ? view->d_func() : nullptr;
    }

    if (view && oldPage != page) {
        if (oldPage) {
            if (oldPage->d_func())
                oldPage->d_func()->view = nullptr;
            deleteOldPage = view->d_func()->m_ownsPage;
        }
        view->d_func()->m_ownsPage = ownNewPage;
        view->d_func()->page = page;
    }

    // Then notify.

    auto item = page ? page->d_func()->delegateItem : nullptr;
    auto oldItem = (oldPage && oldPage->d_func()) ? oldPage->d_func()->delegateItem : nullptr;
    auto widget = item ? static_cast<QtWebEngineCore::WebEngineQuickWidget *>(item->m_widgetDelegate) : nullptr;
    auto oldWidget = oldItem ? static_cast<QtWebEngineCore::WebEngineQuickWidget *>(oldItem->m_widgetDelegate) : nullptr;

    // New page/widget moving away from oldView
    if (page && oldView != view && oldView) {
        oldView->d_func()->pageChanged(page, nullptr);
        if (widget)
            oldView->d_func()->widgetChanged(widget, nullptr);
    }

    // New page/widget moving into new view
    if (view && oldPage != page) {
        if (oldPage && oldPage->d_func())
            view->d_func()->pageChanged(oldPage, page);
        else
            view->d_func()->pageChanged(nullptr, page);
        if (!widget && item) {
            widget = new QtWebEngineCore::WebEngineQuickWidget(item, nullptr);
            item->setWidgetDelegate(widget);
        }
        if (oldWidget != widget)
            view->d_func()->widgetChanged(oldWidget, widget);
    }
    if (deleteOldPage)
        delete oldPage;
}

// static
void QWebEngineViewPrivate::bindPageAndWidget(QWebEnginePagePrivate *pagePrivate,
                                              QtWebEngineCore::WebEngineQuickWidget *widget)
{
    auto *oldAdapterClient = (widget && widget->m_contentItem) ? widget->m_contentItem->m_adapterClient : nullptr;
    auto *oldPagePrivate = static_cast<QWebEnginePagePrivate *>(oldAdapterClient);
    auto *oldItem = pagePrivate ? pagePrivate->delegateItem : nullptr;
    auto *oldWidget = oldItem ? static_cast<QtWebEngineCore::WebEngineQuickWidget *>(oldItem->m_widgetDelegate) : nullptr;

    // Change pointers first.

    if (widget && oldPagePrivate != pagePrivate) {
        if (oldPagePrivate)
            oldPagePrivate->delegateItem = nullptr;
        if (widget->m_contentItem)
            widget->m_contentItem->m_adapterClient = pagePrivate;
    }

    if (pagePrivate && oldWidget != widget) {
        if (oldWidget && oldWidget->m_contentItem)
            oldWidget->m_contentItem->m_adapterClient = nullptr;
        if (widget)
            pagePrivate->delegateItem = widget->m_contentItem;
    }

    // Then notify.

    if (oldPagePrivate && oldPagePrivate != pagePrivate) {
        if (auto oldView = oldPagePrivate->view)
            static_cast<QWebEngineViewPrivate *>(oldView)->widgetChanged(widget, nullptr);
    }

    if (pagePrivate && oldWidget != widget) {
        if (auto view = pagePrivate->view)
            static_cast<QWebEngineViewPrivate *>(view)->widgetChanged(oldWidget, widget);
    }
}

QIcon QWebEngineViewPrivate::webActionIcon(QWebEnginePage::WebAction action) const
{
    Q_Q(const QWebEngineView);
    QIcon icon;
    QStyle *style = q->style();

    switch (action) {
    case QWebEnginePage::Back:
        icon = style->standardIcon(QStyle::SP_ArrowBack);
        break;
    case QWebEnginePage::Forward:
        icon = style->standardIcon(QStyle::SP_ArrowForward);
        break;
    case QWebEnginePage::Stop:
        icon = style->standardIcon(QStyle::SP_BrowserStop);
        break;
    case QWebEnginePage::Reload:
        icon = style->standardIcon(QStyle::SP_BrowserReload);
        break;
    case QWebEnginePage::ReloadAndBypassCache:
        icon = style->standardIcon(QStyle::SP_BrowserReload);
        break;
    default:
        break;
    }
    return icon;
}

QWebEnginePage *QWebEngineViewPrivate::createPageForWindow(QWebEnginePage::WebWindowType type)
{
    Q_Q(QWebEngineView);
    QWebEngineView *newView = q->createWindow(type);
    if (newView)
        return newView->page();
    return nullptr;
}

void QWebEngineViewPrivate::setToolTip(const QString &toolTipText)
{
    Q_Q(QWebEngineView);
    if (toolTipText.isEmpty()) {
        // Avoid duplicate events.
        if (!q->toolTip().isEmpty())
            q->setToolTip(QString());
        // Force to hide tooltip because QWidget's default handler
        // doesn't hide on empty text.
        if (!QToolTip::text().isEmpty())
            QToolTip::hideText();
    } else if (toolTipText != q->toolTip()) {
        q->setToolTip(toolTipText);
    }

}

bool QWebEngineViewPrivate::isEnabled() const
{
    Q_Q(const QWebEngineView);
    return q->isEnabled();
}

QObject *QWebEngineViewPrivate::accessibilityParentObject()
{
    Q_Q(QWebEngineView);
    return q;
}

void QWebEngineViewPrivate::didPrintPage(QPrinter *&currentPrinter, QSharedPointer<QByteArray> result)
{
#if QT_CONFIG(webengine_printing_and_pdf)
    Q_Q(QWebEngineView);

    Q_ASSERT(currentPrinter);

    QThread *printerThread = new QThread;
    QObject::connect(printerThread, &QThread::finished, printerThread, &QThread::deleteLater);
    printerThread->start();

    QtWebEngineCore::PrinterWorker *printerWorker = new QtWebEngineCore::PrinterWorker(result, currentPrinter);
    printerWorker->m_deviceResolution = currentPrinter->resolution();
    printerWorker->m_firstPageFirst = currentPrinter->pageOrder() == QPrinter::FirstPageFirst;
    printerWorker->m_documentCopies = currentPrinter->copyCount();
    printerWorker->m_collateCopies = currentPrinter->collateCopies();

    int oldCopyCount = currentPrinter->copyCount();
    currentPrinter->printEngine()->setProperty(QPrintEngine::PPK_CopyCount, 1);

    QObject::connect(printerWorker, &QtWebEngineCore::PrinterWorker::resultReady, q, [q, &currentPrinter, oldCopyCount](bool success) {
        currentPrinter->printEngine()->setProperty(QPrintEngine::PPK_CopyCount, oldCopyCount);
        currentPrinter = nullptr;
        Q_EMIT q->printFinished(success);
    });

    QObject::connect(printerWorker, &QtWebEngineCore::PrinterWorker::resultReady, printerThread, &QThread::quit);
    QObject::connect(printerThread, &QThread::finished, printerWorker, &QtWebEngineCore::PrinterWorker::deleteLater);

    printerWorker->moveToThread(printerThread);
    QMetaObject::invokeMethod(printerWorker, "print");

#else
    Q_UNUSED(currentPrinter);
    Q_UNUSED(result);
#endif
}

void QWebEngineViewPrivate::didPrintPageToPdf(const QString &filePath, bool success)
{
    Q_Q(QWebEngineView);
    Q_EMIT q->pdfPrintingFinished(filePath, success);
}

void QWebEngineViewPrivate::printRequested()
{
    Q_Q(QWebEngineView);
    QTimer::singleShot(0, q, [q]() {
        Q_EMIT q->printRequested();
    });
}

void QWebEngineViewPrivate::printRequestedByFrame(QWebEngineFrame frame)
{
    Q_Q(QWebEngineView);
    QTimer::singleShot(0, q, [q, frame]() { Q_EMIT q->printRequestedByFrame(frame); });
}

bool QWebEngineViewPrivate::isVisible() const
{
    Q_Q(const QWebEngineView);
    return q->isVisible();
}
QRect QWebEngineViewPrivate::viewportRect() const
{
    if (m_webEngineWidget && !m_webEngineWidget->size().isEmpty())
        return QRect(QPoint(), m_webEngineWidget->size());

    Q_Q(const QWebEngineView);
    const int leftInset = qutebrowserChromeLeftInset();
    return QRect(0, 0, qMax(0, q->width() - leftInset), q->height());
}
QtWebEngineCore::RenderWidgetHostViewQtDelegate *
QWebEngineViewPrivate::CreateRenderWidgetHostViewQtDelegate(
        QtWebEngineCore::RenderWidgetHostViewQtDelegateClient *client)
{
    auto *item = new QtWebEngineCore::RenderWidgetHostViewQtDelegateItem(client, false);
    auto *widget = new QtWebEngineCore::WebEngineQuickWidget(item, nullptr);
    item->setWidgetDelegate(widget);
    return item;
}

QtWebEngineCore::RenderWidgetHostViewQtDelegate *
QWebEngineViewPrivate::CreateRenderWidgetHostViewQtDelegateForPopup(
        QtWebEngineCore::RenderWidgetHostViewQtDelegateClient *client)
{
    Q_Q(QWebEngineView);
    auto *item = new QtWebEngineCore::RenderWidgetHostViewQtDelegateItem(client, true);
    auto *widget = new QtWebEngineCore::WebEngineQuickWidget(item, q);
    item->setWidgetDelegate(widget);
    return item;
}

QWebEngineContextMenuRequest *QWebEngineViewPrivate::lastContextMenuRequest() const
{
    return m_contextRequest;
}

void QWebEngineViewPrivate::showAutofillPopup(QtWebEngineCore::AutofillPopupController *controller,
                                              const QRect &bounds, bool autoselectFirstSuggestion)
{
    Q_Q(QWebEngineView);
    if (!m_autofillPopupWidget)
        m_autofillPopupWidget.reset(new QtWebEngineWidgetUI::AutofillPopupWidget(controller, q));
    m_autofillPopupWidget->showPopup(q->mapToGlobal(bounds.bottomLeft()), bounds.width() + 2,
                                     autoselectFirstSuggestion);
    controller->notifyPopupShown();
}

void QWebEngineViewPrivate::hideAutofillPopup()
{
    if (!m_autofillPopupWidget)
        return;

    Q_Q(QWebEngineView);
    QTimer::singleShot(0, q, [this] {
        if (m_autofillPopupWidget) {
            QtWebEngineCore::AutofillPopupController *controller =
                    m_autofillPopupWidget->m_controller;
            m_autofillPopupWidget.reset();
            controller->notifyPopupHidden();
        }
    });
}

/*!
    \fn QWebEngineView::renderProcessTerminated(QWebEnginePage::RenderProcessTerminationStatus terminationStatus, int exitCode)
    \since 5.6

    This signal is emitted when the render process is terminated with a non-zero exit status.
    \a terminationStatus is the termination status of the process and \a exitCode is the status code
    with which the process terminated.
*/

/*!
    \fn void QWebEngineView::iconChanged(const QIcon &icon)
    \since 5.7

    This signal is emitted when the icon ("favicon") associated with the
    view is changed. The new icon is specified by \a icon.

    \sa icon(), iconUrl(), iconUrlChanged()
*/

QWebEngineView::QWebEngineView(QWidget *parent)
    : QWidget(parent)
    , d_ptr(new QWebEngineViewPrivate)
{
    Q_D(QWebEngineView);
    d->q_ptr = this;
    setAcceptDrops(true);

    QHBoxLayout *layout = new QHBoxLayout;
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    setLayout(layout);
}

/*!
    \since 6.4

    Constructs an empty web view using \a profile with the parent \a parent.

    \note The \a profile object ownership is not taken and it should outlive the view.

    \sa load()
*/

QWebEngineView::QWebEngineView(QWebEngineProfile *profile, QWidget *parent)
    : QWebEngineView(parent)
{
    Q_D(QWebEngineView);
    setPage(new QWebEnginePage(profile, this));
    d->m_ownsPage = true;
}

/*!
    \since 6.4

    Constructs a web view containing \a page with the parent \a parent.

    \note Ownership of \a page is not taken, and it is up to the caller to ensure it is deleted.

    \sa load(), setPage()
*/

QWebEngineView::QWebEngineView(QWebEnginePage *page, QWidget *parent)
    : QWebEngineView(parent)
{
    setPage(page);
}

QWebEngineView::~QWebEngineView()
{
    blockSignals(true);
    QWebEngineViewPrivate::bindPageAndView(nullptr, this);
}

/*!
    \since 6.2

    Returns the view if any, associated with the \a page.

    \sa page(), setPage()
*/
QWebEngineView *QWebEngineView::forPage(const QWebEnginePage *page)
{
    if (!page)
        return nullptr;
    return qobject_cast<QWebEngineView *>(page->d_ptr->accessibilityParentObject());
}

QWebEnginePage* QWebEngineView::page() const
{
    Q_D(const QWebEngineView);
    if (!d->page) {
        QWebEngineView *that = const_cast<QWebEngineView*>(this);
        that->setPage(new QWebEnginePage(that));
        d->m_ownsPage = true;
    }
    return d->page;
}

void QWebEngineView::setPage(QWebEnginePage *newPage)
{
    Q_D(QWebEngineView);
    if (d->page) {
        disconnect(d->m_pageConnection);
        d->m_pageConnection = {};
    }

    QWebEngineViewPrivate::bindPageAndView(newPage, this);
    if (!newPage)
        return;
    d->m_pageConnection = connect(newPage, &QWebEnginePage::_q_aboutToDelete, this,
                                  [newPage]() { QWebEngineViewPrivate::bindPageAndView(newPage, nullptr); });
    auto profile = newPage->profile();
    if (!profile->notificationPresenter())
        profile->setNotificationPresenter(&defaultNotificationPresenter);
}

void QWebEngineView::load(const QUrl& url)
{
    page()->load(url);
}

/*!
    \since 5.9
    Issues the specified \a request and loads the response.

    \sa load(), setUrl(), url(), urlChanged(), QUrl::fromUserInput()
*/
void QWebEngineView::load(const QWebEngineHttpRequest &request)
{
    page()->load(request);
}

void QWebEngineView::setHtml(const QString& html, const QUrl& baseUrl)
{
    page()->setHtml(html, baseUrl);
}

void QWebEngineView::setContent(const QByteArray& data, const QString& mimeType, const QUrl& baseUrl)
{
    page()->setContent(data, mimeType, baseUrl);
}

QWebEngineHistory* QWebEngineView::history() const
{
    return page()->history();
}

QString QWebEngineView::title() const
{
    return page()->title();
}

void QWebEngineView::setUrl(const QUrl &url)
{
    page()->setUrl(url);
}

QUrl QWebEngineView::url() const
{
    return page()->url();
}

QUrl QWebEngineView::iconUrl() const
{
    return page()->iconUrl();
}

/*!
    \property QWebEngineView::icon
    \brief The icon associated with the page currently viewed.
    \since 5.7

    By default, this property contains a null icon.

    \sa iconChanged(), iconUrl(), iconUrlChanged()
*/
QIcon QWebEngineView::icon() const
{
    return page()->icon();
}

bool QWebEngineView::hasSelection() const
{
    return page()->hasSelection();
}

QString QWebEngineView::selectedText() const
{
    return page()->selectedText();
}

#if QT_CONFIG(action)
QAction* QWebEngineView::pageAction(QWebEnginePage::WebAction action) const
{
    Q_D(const QWebEngineView);
    QAction *pageAction = page()->action(action);

    if (pageAction->icon().isNull()) {
        auto icon = d->webActionIcon(action);
        if (!icon.isNull())
            pageAction->setIcon(icon);
    }

    return pageAction;
}
#endif

void QWebEngineView::triggerPageAction(QWebEnginePage::WebAction action, bool checked)
{
    page()->triggerAction(action, checked);
}

void QWebEngineView::findText(const QString &subString, QWebEnginePage::FindFlags options, const std::function<void(const QWebEngineFindTextResult &)> &resultCallback)
{
    page()->findText(subString, options, resultCallback);
}

/*!
 * \reimp
 */
QSize QWebEngineView::sizeHint() const
{
    // TODO: Remove this override for Qt 6
    return QWidget::sizeHint();
}

QWebEngineSettings *QWebEngineView::settings() const
{
    return page()->settings();
}

void QWebEngineView::stop()
{
    page()->triggerAction(QWebEnginePage::Stop);
}

void QWebEngineView::back()
{
    page()->triggerAction(QWebEnginePage::Back);
}

void QWebEngineView::forward()
{
    page()->triggerAction(QWebEnginePage::Forward);
}

void QWebEngineView::reload()
{
    page()->triggerAction(QWebEnginePage::Reload);
}

QWebEngineView *QWebEngineView::createWindow(QWebEnginePage::WebWindowType type)
{
    Q_UNUSED(type);
    return nullptr;
}

qreal QWebEngineView::zoomFactor() const
{
    return page()->zoomFactor();
}

void QWebEngineView::setZoomFactor(qreal factor)
{
    page()->setZoomFactor(factor);
}

/*!
 * \reimp
 */
bool QWebEngineView::event(QEvent *ev)
{
    if (ev->type() == QEvent::ContextMenu) {
        if (contextMenuPolicy() == Qt::NoContextMenu) {
            // We forward the contextMenu event to the parent widget
            ev->ignore();
            return false;
        }

        // We swallow spontaneous contextMenu events and synthethize those back later on when we get the
        // HandleContextMenu callback from chromium
        ev->accept();
        return true;
    }

    const bool handled = QWidget::event(ev);
    if (ev->type() == QEvent::Resize) {
        Q_D(QWebEngineView);
        d->positionQutebrowserStatusOverlay();
        if (d->m_qutebrowserFindOverlay)
            d->positionQutebrowserFindOverlay();
        if (d->m_qutebrowserCommandLineOverlay)
            d->positionQutebrowserCommandLineOverlay();
    }
    return handled;
}

/*!
 * \reimp
 */
#if QT_CONFIG(contextmenu)
void QWebEngineView::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu *menu = createStandardContextMenu();
    menu->popup(event->globalPos());
}
#endif // QT_CONFIG(contextmenu)

/*!
 * \reimp
 */
void QWebEngineView::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    page()->setVisible(true);
}

/*!
 * \reimp
 */
void QWebEngineView::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    page()->setVisible(false);
}

/*!
 * \reimp
 */
void QWebEngineView::closeEvent(QCloseEvent *event)
{
    QWidget::closeEvent(event);
    page()->setVisible(false);
    page()->setLifecycleState(QWebEnginePage::LifecycleState::Discarded);
}

#if QT_CONFIG(draganddrop)
/*!
    \reimp
*/
void QWebEngineView::dragEnterEvent(QDragEnterEvent *e)
{
    Q_D(QWebEngineView);
    e->accept();
    if (d->m_dragEntered)
        d->page->d_ptr->adapter->leaveDrag();
    d->page->d_ptr->adapter->enterDrag(e, mapToGlobal(e->position().toPoint()));
    d->m_dragEntered = true;
}

/*!
    \reimp
*/
void QWebEngineView::dragLeaveEvent(QDragLeaveEvent *e)
{
    Q_D(QWebEngineView);
    if (!d->m_dragEntered)
        return;
    e->accept();
    d->page->d_ptr->adapter->leaveDrag();
    d->m_dragEntered = false;
}

/*!
    \reimp
*/
void QWebEngineView::dragMoveEvent(QDragMoveEvent *e)
{
    Q_D(QWebEngineView);
    if (!d->m_dragEntered)
        return;
    QtWebEngineCore::WebContentsAdapter *adapter = d->page->d_ptr->adapter.data();
    Qt::DropAction dropAction =
            adapter->updateDragPosition(e, mapToGlobal(e->position().toPoint()));
    if (Qt::IgnoreAction == dropAction) {
        e->ignore();
    } else {
        e->setDropAction(dropAction);
        e->accept();
    }
}

/*!
    \reimp
*/
void QWebEngineView::dropEvent(QDropEvent *e)
{
    Q_D(QWebEngineView);
    if (!d->m_dragEntered)
        return;
    e->accept();
    d->page->d_ptr->adapter->endDragging(e, mapToGlobal(e->position().toPoint()));
    d->m_dragEntered = false;
}
#endif // QT_CONFIG(draganddrop)

#if QT_CONFIG(menu)
/*!
  Creates a standard context menu and returns a pointer to it.
*/
QMenu *QWebEngineView::createStandardContextMenu()
{
    Q_D(QWebEngineView);
    QMenu *menu = new QMenu(this);
    QContextMenuBuilder contextMenuBuilder(d->m_contextRequest, this, menu);

    contextMenuBuilder.initMenu();

    menu->setAttribute(Qt::WA_DeleteOnClose, true);

    return menu;
}
#endif // QT_CONFIG(menu)

/*!
  \since 6.2

  Returns additional data about the current context menu. It is only guaranteed to be valid during
  the call to the contextMenuEvent().

  \sa createStandardContextMenu()
*/
QWebEngineContextMenuRequest *QWebEngineView::lastContextMenuRequest() const
{
    Q_D(const QWebEngineView);
    return d->m_contextRequest;
}

/*!
    \fn void QWebEngineView::pdfPrintingFinished(const QString &filePath, bool success)
    \since 6.2

    This signal is emitted when printing the web page into a PDF file has
    finished.
    \a filePath will contain the path the file was requested to be created
    at, and \a success will be \c true if the file was successfully created and
    \c false otherwise.

    \sa printToPdf()
*/

/*!
    Renders the current content of the page into a PDF document and saves it
    in the location specified in \a filePath.
    The page size and orientation of the produced PDF document are taken from
    the values specified in \a layout, while the range of pages printed is
    taken from \a ranges with the default being printing all pages.

    This method issues an asynchronous request for printing the web page into
    a PDF and returns immediately.
    To be informed about the result of the request, connect to the signal
    pdfPrintingFinished().

    If a file already exists at the provided file path, it will be overwritten.
    \since 6.2
    \sa pdfPrintingFinished()
*/
void QWebEngineView::printToPdf(const QString &filePath, const QPageLayout &layout, const QPageRanges &ranges)
{
    page()->printToPdf(filePath, layout, ranges);
}

/*!
    Renders the current content of the page into a PDF document and returns a byte array containing the PDF data
    as parameter to \a resultCallback.
    The page size and orientation of the produced PDF document are taken from the values specified in \a layout,
    while the range of pages printed is taken from \a ranges with the default being printing all pages.

    The \a resultCallback must take a const reference to a QByteArray as parameter. If printing was successful, this byte array
    will contain the PDF data, otherwise, the byte array will be empty.

    \warning We guarantee that the callback (\a resultCallback) is always called, but it might be done
    during page destruction. When QWebEnginePage is deleted, the callback is triggered with an invalid
    value and it is not safe to use the corresponding QWebEnginePage or QWebEngineView instance inside it.

    \since 6.2
*/
void QWebEngineView::printToPdf(const std::function<void(const QByteArray&)> &resultCallback, const QPageLayout &layout, const QPageRanges &ranges)
{
    page()->printToPdf(resultCallback, layout, ranges);
}

/*!
    \fn void QWebEngineView::printRequested()
    \since 6.2

    This signal is emitted when the JavaScript \c{window.print()} method is called or the user pressed the print
    button of PDF viewer plugin.
    Typically, the signal handler can simply call print().

    Since 6.8, this signal is only emitted for the main frame, instead of being emitted
    for any frame that requests printing.

    \sa printRequestedByFrame(), print()
*/

/*!
    \fn void QWebEngineView::printRequestedByFrame(QWebEngineFrame frame)
    \since 6.8

    This signal is emitted when the JavaScript \c{window.print()} method is called on \a frame.
    If the frame is the main frame, \c{printRequested} is emitted instead.

    \sa printRequested(), print()
*/

/*!
    \fn void QWebEngineView::printFinished(bool success)
    \since 6.2

    This signal is emitted when printing requested with print() has finished.
    The parameter \a success is \c true for success or \c false for failure.

    \sa print()
*/

/*!
    Renders the current content of the page into a temporary PDF document, then prints it using \a printer.

    The settings for creating and printing the PDF document will be retrieved from the \a printer
    object.

    When finished the signal printFinished() is emitted with the \c true for success or \c false for failure.

    It is the user's responsibility to ensure the \a printer remains valid until printFinished()
    has been emitted.

    \note Printing runs on the browser process, which is by default not sandboxed.

    \note The data generation step of printing can be interrupted for a short period of time using
    the \l QWebEnginePage::Stop web action.

    \note This function rasterizes the result when rendering onto \a printer. Please consider raising
    the default resolution of \a printer to at least 300 DPI, or using printToPdf() to produce
    PDF file output more effectively.

    \since 6.2
*/
void QWebEngineView::print(QPrinter *printer)
{
#if QT_CONFIG(webengine_printing_and_pdf)
    auto *dPage = page()->d_ptr.get();
    if (dPage->currentPrinter) {
        qWarning("Cannot print page on printer %ls: Already printing on a device.", qUtf16Printable(printer->printerName()));
        return;
    }

    dPage->currentPrinter = printer;
    dPage->ensureInitialized();
    std::function callback = [dPage](QSharedPointer<QByteArray> result) {
        dPage->didPrintPage(std::move(result));
    };
    dPage->adapter->printToPDFCallbackResult(std::move(callback), printer->pageLayout(),
                                             printer->pageRanges(),
                                             printer->colorMode() == QPrinter::Color,
                                             QtWebEngineCore::WebContentsAdapter::kUseMainFrameId);
#else
    Q_UNUSED(printer);
    Q_EMIT printFinished(false);
#endif
}

#if QT_CONFIG(action)
QContextMenuBuilder::QContextMenuBuilder(QWebEngineContextMenuRequest *request,
                                         QWebEngineView *view, QMenu *menu)
    : QtWebEngineCore::RenderViewContextMenuQt(request), m_view(view), m_menu(menu)
{
    m_view->page()->d_ptr->ensureInitialized();
}

bool QContextMenuBuilder::hasInspector()
{
    return m_view->page()->d_ptr->adapter->hasInspector();
}

bool QContextMenuBuilder::isFullScreenMode()
{
    return m_view->page()->d_ptr->isFullScreenMode();
}

void QContextMenuBuilder::addMenuItem(ContextMenuItem menuItem)
{
    QPointer<QWebEnginePage> thisRef(m_view->page());
    QAction *action = nullptr;

    switch (menuItem) {
    case ContextMenuItem::Back:
        action = m_view->pageAction(QWebEnginePage::Back);
        break;
    case ContextMenuItem::Forward:
        action = m_view->pageAction(QWebEnginePage::Forward);
        break;
    case ContextMenuItem::Reload:
        action = m_view->pageAction(QWebEnginePage::Reload);
        break;
    case ContextMenuItem::Cut:
        action = m_view->pageAction(QWebEnginePage::Cut);
        break;
    case ContextMenuItem::Copy:
        action = m_view->pageAction(QWebEnginePage::Copy);
        break;
    case ContextMenuItem::Paste:
        action = m_view->pageAction(QWebEnginePage::Paste);
        break;
    case ContextMenuItem::Undo:
        action = m_view->pageAction(QWebEnginePage::Undo);
        break;
    case ContextMenuItem::Redo:
        action = m_view->pageAction(QWebEnginePage::Redo);
        break;
    case ContextMenuItem::SelectAll:
        action = m_view->pageAction(QWebEnginePage::SelectAll);
        break;
    case ContextMenuItem::PasteAndMatchStyle:
        action = m_view->pageAction(QWebEnginePage::PasteAndMatchStyle);
        break;
    case ContextMenuItem::OpenLinkInNewWindow:
        action = m_view->pageAction(QWebEnginePage::OpenLinkInNewWindow);
        break;
    case ContextMenuItem::OpenLinkInNewTab:
        action = m_view->pageAction(QWebEnginePage::OpenLinkInNewTab);
        break;
    case ContextMenuItem::CopyLinkToClipboard:
        action = m_view->pageAction(QWebEnginePage::CopyLinkToClipboard);
        break;
    case ContextMenuItem::DownloadLinkToDisk:
        action = m_view->pageAction(QWebEnginePage::DownloadLinkToDisk);
        break;
    case ContextMenuItem::CopyImageToClipboard:
        action = m_view->pageAction(QWebEnginePage::CopyImageToClipboard);
        break;
    case ContextMenuItem::CopyImageUrlToClipboard:
        action = m_view->pageAction(QWebEnginePage::CopyImageUrlToClipboard);
        break;
    case ContextMenuItem::DownloadImageToDisk:
        action = m_view->pageAction(QWebEnginePage::DownloadImageToDisk);
        break;
    case ContextMenuItem::CopyMediaUrlToClipboard:
        action = m_view->pageAction(QWebEnginePage::CopyMediaUrlToClipboard);
        break;
    case ContextMenuItem::ToggleMediaControls:
        action = m_view->pageAction(QWebEnginePage::ToggleMediaControls);
        break;
    case ContextMenuItem::ToggleMediaLoop:
        action = m_view->pageAction(QWebEnginePage::ToggleMediaLoop);
        break;
    case ContextMenuItem::DownloadMediaToDisk:
        action = m_view->pageAction(QWebEnginePage::DownloadMediaToDisk);
        break;
    case ContextMenuItem::InspectElement:
        action = m_view->pageAction(QWebEnginePage::InspectElement);
        break;
    case ContextMenuItem::ExitFullScreen:
        action = m_view->pageAction(QWebEnginePage::ExitFullScreen);
        break;
    case ContextMenuItem::SavePage:
        action = m_view->pageAction(QWebEnginePage::SavePage);
        break;
    case ContextMenuItem::ViewSource:
        action = m_view->pageAction(QWebEnginePage::ViewSource);
        break;
    case ContextMenuItem::SpellingSuggestions:
        for (int i = 0; i < m_contextData->spellCheckerSuggestions().size() && i < 4; i++) {
            action = new QAction(m_menu);
            QString replacement = m_contextData->spellCheckerSuggestions().at(i);
            QObject::connect(action, &QAction::triggered, [thisRef, replacement] {
                if (thisRef)
                    thisRef->replaceMisspelledWord(replacement);
            });
            action->setText(replacement);
            m_menu->addAction(action);
        }
        return;
    case ContextMenuItem::Separator:
        if (!m_menu->isEmpty())
            m_menu->addSeparator();
        return;
    }
    action->setEnabled(isMenuItemEnabled(menuItem));
    m_menu->addAction(action);
}

bool QContextMenuBuilder::isMenuItemEnabled(ContextMenuItem menuItem)
{
    switch (menuItem) {
    case ContextMenuItem::Back:
        return m_view->page()->d_ptr->adapter->canGoBack();
    case ContextMenuItem::Forward:
        return m_view->page()->d_ptr->adapter->canGoForward();
    case ContextMenuItem::Reload:
        return true;
    case ContextMenuItem::Cut:
        return m_contextData->editFlags() & QWebEngineContextMenuRequest::CanCut;
    case ContextMenuItem::Copy:
        return m_contextData->editFlags() & QWebEngineContextMenuRequest::CanCopy;
    case ContextMenuItem::Paste:
        return m_contextData->editFlags() & QWebEngineContextMenuRequest::CanPaste;
    case ContextMenuItem::Undo:
        return m_contextData->editFlags() & QWebEngineContextMenuRequest::CanUndo;
    case ContextMenuItem::Redo:
        return m_contextData->editFlags() & QWebEngineContextMenuRequest::CanRedo;
    case ContextMenuItem::SelectAll:
        return m_contextData->editFlags() & QWebEngineContextMenuRequest::CanSelectAll;
    case ContextMenuItem::PasteAndMatchStyle:
        return m_contextData->editFlags() & QWebEngineContextMenuRequest::CanPaste;
    case ContextMenuItem::OpenLinkInNewWindow:
    case ContextMenuItem::OpenLinkInNewTab:
    case ContextMenuItem::CopyLinkToClipboard:
    case ContextMenuItem::DownloadLinkToDisk:
    case ContextMenuItem::CopyImageToClipboard:
    case ContextMenuItem::CopyImageUrlToClipboard:
    case ContextMenuItem::DownloadImageToDisk:
    case ContextMenuItem::CopyMediaUrlToClipboard:
    case ContextMenuItem::ToggleMediaControls:
    case ContextMenuItem::ToggleMediaLoop:
    case ContextMenuItem::DownloadMediaToDisk:
    case ContextMenuItem::InspectElement:
    case ContextMenuItem::ExitFullScreen:
    case ContextMenuItem::SavePage:
        return true;
    case ContextMenuItem::ViewSource:
        return m_view->page()->d_ptr->adapter->canViewSource();
    case ContextMenuItem::SpellingSuggestions:
    case ContextMenuItem::Separator:
        return true;
    }
    Q_UNREACHABLE();
}
#endif // QT_CONFIG(action)

QtWebEngineCore::TouchHandleDrawableDelegate *
QWebEngineViewPrivate::createTouchHandleDelegate(const QMap<int, QImage> &images)
{
    Q_Q(QWebEngineView);
    return new QtWebEngineWidgetUI::TouchHandleWidget(q, images);
}

void QWebEngineViewPrivate::hideTouchSelectionMenu()
{
    if (m_touchSelectionMenu)
        m_touchSelectionMenu->close();
}

void QWebEngineViewPrivate::showTouchSelectionMenu(
        QtWebEngineCore::TouchSelectionMenuController *controller, const QRect &selectionBounds)
{
    Q_Q(QWebEngineView);

    // Do not show outside of view
    QSize parentSize = q->nativeParentWidget() ? q->nativeParentWidget()->size() : q->size();
    if (selectionBounds.x() < 0 || selectionBounds.x() > parentSize.width()
        || selectionBounds.y() < 0 || selectionBounds.y() > parentSize.height())
        return;

    m_touchSelectionMenu = new QtWebEngineWidgetUI::TouchSelectionMenuWidget(q, controller);

    const int kSpacingBetweenButtons = 2;
    const int kMenuButtonMinWidth = 80;
    const int kMenuButtonMinHeight = 40;

    int buttonCount = controller->buttonCount();
    int width = (kSpacingBetweenButtons * (buttonCount + 1)) + (kMenuButtonMinWidth * buttonCount);
    int height = kMenuButtonMinHeight + kSpacingBetweenButtons;
    int x = (selectionBounds.x() + selectionBounds.x() + selectionBounds.width() - width) / 2;
    int y = selectionBounds.y() - height - 2;

    QPoint pos = q->mapToGlobal(QPoint(x, y));

    m_touchSelectionMenu->setGeometry(pos.x(), pos.y(), width, height);
    m_touchSelectionMenu->show();
}

QT_END_NAMESPACE

#include "moc_qwebengineview.cpp"
