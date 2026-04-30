// Copyright (C) 2026 Yeyito. All rights reserved.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qutebrowser_key_dispatcher.h"

#include "native_web_keyboard_event_qt.h"
#include "render_widget_host_view_qt.h"
#include "web_contents_adapter.h"
#include "web_contents_delegate_qt.h"
#include "web_engine_settings.h"

#include "base/functional/bind.h"
#include "base/values.h"
#include "content/public/browser/page_navigator.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/ime/text_input_type.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"
#include "url/gurl.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QRectF>
#include <QUrl>

#include <algorithm>
#include <cmath>

namespace QtWebEngineCore {

namespace {

constexpr double kSmoothScrollFactor = 0.3;
constexpr int kArrowScrollPx = 40;
constexpr int kFallbackPageScrollPx = 1120;

bool HasOnlyModifier(Qt::KeyboardModifiers actual, Qt::KeyboardModifier expected)
{
    return actual == Qt::KeyboardModifiers(expected);
}

bool HasNoOrOnlyShift(Qt::KeyboardModifiers modifiers)
{
    const Qt::KeyboardModifiers disallowed =
            Qt::KeyboardModifier::ControlModifier |
            Qt::KeyboardModifier::AltModifier |
            Qt::KeyboardModifier::MetaModifier;
    return !(modifiers & disallowed);
}

QString normalizedCommandName(QString command)
{
    command = command.trimmed();
    int space = command.indexOf(QLatin1Char(' '));
    if (space >= 0)
        command.truncate(space);
    return command;
}

QString commandArgument(QString command)
{
    command = command.trimmed();
    int space = command.indexOf(QLatin1Char(' '));
    return space < 0 ? QString() : command.mid(space + 1).trimmed();
}

} // namespace

QutebrowserKeyDispatcher::QutebrowserKeyDispatcher(WebContentsDelegateQt *delegate)
    : delegate_(delegate)
{
    loadDefaultBindings();
}

void QutebrowserKeyDispatcher::clearRendererMode()
{
    if (mode_ == Mode::kHint)
        setMode(Mode::kNormal);
    clearKeychain();
}

void QutebrowserKeyDispatcher::loadDefaultBindings()
{
#include "qutebrowser_key_bindings.inc"
}

void QutebrowserKeyDispatcher::addBinding(Mode mode, const QVector<QString> &sequence, const QString &command)
{
    if (sequence.isEmpty())
        return;
    std::unique_ptr<BindingNode> &root = bindings_[mode];
    if (!root)
        root = std::make_unique<BindingNode>();
    BindingNode *node = root.get();
    for (const QString &key : sequence) {
        std::unique_ptr<BindingNode> &child = node->children[key];
        if (!child)
            child = std::make_unique<BindingNode>();
        node = child.get();
    }
    node->command = command;
}

void QutebrowserKeyDispatcher::removeBinding(Mode mode, const QVector<QString> &sequence)
{
    auto root_it = bindings_.find(mode);
    if (root_it == bindings_.end() || !root_it->second)
        return;
    BindingNode *node = root_it->second.get();
    for (const QString &key : sequence) {
        auto child_it = node->children.find(key);
        if (child_it == node->children.end())
            return;
        node = child_it->second.get();
    }
    node->command.clear();
}

QutebrowserKeyDispatcher::MatchResult
QutebrowserKeyDispatcher::match(Mode mode, const QVector<QString> &sequence) const
{
    auto root_it = bindings_.find(mode);
    if (root_it == bindings_.end() || !root_it->second || sequence.isEmpty())
        return {};

    BindingNode *node = root_it->second.get();
    for (const QString &key : sequence) {
        auto child_it = node->children.find(key);
        if (child_it == node->children.end())
            return {};
        node = child_it->second.get();
    }

    if (!node->command.isEmpty())
        return {MatchType::kExactMatch, node->command};
    if (!node->children.empty())
        return {MatchType::kPartialMatch, QString()};
    return {};
}

QString QutebrowserKeyDispatcher::keyStringForEvent(const QKeyEvent *event) const
{
    if (!event)
        return QString();

    const int key = event->key();
    if (key == Qt::Key_Shift || key == Qt::Key_Control || key == Qt::Key_Alt || key == Qt::Key_Meta)
        return QString();

    const Qt::KeyboardModifiers modifiers = event->modifiers();
    const bool has_non_shift_modifier = modifiers &
            (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);

    QString key_name;
    switch (key) {
    case Qt::Key_Escape: key_name = QStringLiteral("Escape"); break;
    case Qt::Key_Return: case Qt::Key_Enter: key_name = QStringLiteral("Return"); break;
    case Qt::Key_Backspace: key_name = QStringLiteral("Backspace"); break;
    case Qt::Key_Back: key_name = QStringLiteral("Back"); break;
    case Qt::Key_Forward: key_name = QStringLiteral("Forward"); break;
    case Qt::Key_PageDown: key_name = QStringLiteral("PgDown"); break;
    case Qt::Key_PageUp: key_name = QStringLiteral("PgUp"); break;
    case Qt::Key_Space: key_name = QStringLiteral("Space"); break;
    case Qt::Key_Tab: key_name = QStringLiteral("Tab"); break;
    case Qt::Key_Backtab: key_name = QStringLiteral("Tab"); break;
    case Qt::Key_Delete: key_name = QStringLiteral("Delete"); break;
    case Qt::Key_Insert: key_name = QStringLiteral("Ins"); break;
    case Qt::Key_Up: key_name = QStringLiteral("Up"); break;
    case Qt::Key_Down: key_name = QStringLiteral("Down"); break;
    case Qt::Key_Left: key_name = QStringLiteral("Left"); break;
    case Qt::Key_Right: key_name = QStringLiteral("Right"); break;
    default:
        if (key >= Qt::Key_F1 && key <= Qt::Key_F35) {
            key_name = QStringLiteral("F%1").arg(key - Qt::Key_F1 + 1);
        } else if (key >= Qt::Key_A && key <= Qt::Key_Z) {
            const bool shifted = modifiers & Qt::ShiftModifier;
            // qutebrowser's binding syntax canonicalizes modified letters as
            // uppercase key names (Ctrl+J, Ctrl+Shift+Y, Alt+M, ...), while
            // plain shifted text is represented by the shifted character (J).
            // Keep those forms aligned with qutebrowser_key_bindings.inc.
            key_name = QChar(QLatin1Char((has_non_shift_modifier || shifted ? 'A' : 'a') + key - Qt::Key_A));
        } else if (!event->text().isEmpty()) {
            key_name = event->text().left(1);
        } else if (key >= Qt::Key_0 && key <= Qt::Key_9) {
            key_name = QChar(QLatin1Char('0' + key - Qt::Key_0));
        } else if (key == Qt::Key_Minus) {
            key_name = QStringLiteral("-");
        } else if (key == Qt::Key_Equal) {
            key_name = QStringLiteral("=");
        } else if (key == Qt::Key_Plus) {
            key_name = QStringLiteral("+");
        } else if (key == Qt::Key_BracketLeft) {
            key_name = QStringLiteral("[");
        } else if (key == Qt::Key_BracketRight) {
            key_name = QStringLiteral("]");
        } else if (key == Qt::Key_BraceLeft) {
            key_name = QStringLiteral("{");
        } else if (key == Qt::Key_BraceRight) {
            key_name = QStringLiteral("}");
        } else if (key == Qt::Key_Apostrophe) {
            key_name = QStringLiteral("'");
        } else if (key == Qt::Key_QuoteDbl) {
            key_name = QStringLiteral("\"");
        } else if (key == Qt::Key_Colon) {
            key_name = QStringLiteral(":");
        } else if (key == Qt::Key_Semicolon) {
            key_name = QStringLiteral(";");
        } else if (key == Qt::Key_Comma) {
            key_name = QStringLiteral(",");
        } else if (key == Qt::Key_Period) {
            key_name = QStringLiteral(".");
        } else if (key == Qt::Key_Slash) {
            key_name = QStringLiteral("/");
        } else if (key == Qt::Key_Question) {
            key_name = QStringLiteral("?");
        } else if (key == Qt::Key_QuoteLeft) {
            key_name = QStringLiteral("`");
        } else if (key == Qt::Key_AsciiTilde) {
            key_name = QStringLiteral("~");
        } else if (key == Qt::Key_Dollar) {
            key_name = QStringLiteral("$");
        } else if (key == Qt::Key_AsciiCircum) {
            key_name = QStringLiteral("^");
        } else {
            key_name = QKeySequence(key).toString();
        }
    }

    if (key_name.isEmpty())
        return QString();

    QStringList parts;
    if (modifiers & Qt::ControlModifier)
        parts << QStringLiteral("Ctrl");
    if (modifiers & Qt::AltModifier)
        parts << QStringLiteral("Alt");
    if (modifiers & Qt::ShiftModifier) {
        const bool text_already_represents_shift =
                key_name.size() == 1 && !has_non_shift_modifier &&
                (!event->text().isEmpty() || (key >= Qt::Key_A && key <= Qt::Key_Z));
        if (!text_already_represents_shift || key_name == QStringLiteral("Tab"))
            parts << QStringLiteral("Shift");
    }
    if (modifiers & Qt::MetaModifier)
        parts << QStringLiteral("Meta");
    parts << key_name;
    return parts.join(QLatin1Char('+'));
}

bool QutebrowserKeyDispatcher::isEditableFocused(content::WebContents *contents) const
{
    if (!contents)
        return false;
    auto *view = contents->GetRenderWidgetHostView();
    if (auto *qt_view = static_cast<RenderWidgetHostViewQt *>(view)) {
        const ui::TextInputType type = qt_view->getTextInputType();
        return type != ui::TEXT_INPUT_TYPE_NONE;
    }
    return false;
}

bool QutebrowserKeyDispatcher::shouldPassThroughMode() const
{
    return mode_ == Mode::kInsert || mode_ == Mode::kPassthrough ||
           mode_ == Mode::kCommand || mode_ == Mode::kPrompt;
}

bool QutebrowserKeyDispatcher::isCountKey(const QString &key) const
{
    return mode_ == Mode::kNormal && key.size() == 1 && key[0].isDigit() &&
           !(count_.isEmpty() && key == QStringLiteral("0"));
}

int QutebrowserKeyDispatcher::currentCount() const
{
    bool ok = false;
    const int count = count_.toInt(&ok);
    return ok && count > 0 ? count : 1;
}

void QutebrowserKeyDispatcher::clearKeychain()
{
    sequence_.clear();
    count_.clear();
    emitStatusChanged();
}

QString QutebrowserKeyDispatcher::keychainString() const
{
    QString out;
    for (const QString &key : sequence_)
        out += key;
    return out;
}

QString QutebrowserKeyDispatcher::modeName(Mode mode) const
{
    switch (mode) {
    case Mode::kNormal: return QStringLiteral("normal");
    case Mode::kInsert: return QStringLiteral("insert");
    case Mode::kHint: return QStringLiteral("hint");
    case Mode::kPassthrough: return QStringLiteral("passthrough");
    case Mode::kCommand: return QStringLiteral("command");
    case Mode::kPrompt: return QStringLiteral("prompt");
    case Mode::kYesno: return QStringLiteral("yesno");
    case Mode::kCaret: return QStringLiteral("caret");
    case Mode::kSetMark: return QStringLiteral("set_mark");
    case Mode::kJumpMark: return QStringLiteral("jump_mark");
    case Mode::kRegister: return QStringLiteral("register");
    }
    return QStringLiteral("normal");
}

void QutebrowserKeyDispatcher::emitModeChanged(Mode oldMode, Mode newMode)
{
    if (delegate_ && delegate_->adapterClient())
        delegate_->adapterClient()->qutebrowserModeChanged(modeName(oldMode), modeName(newMode));
}

void QutebrowserKeyDispatcher::emitStatusChanged()
{
    if (delegate_ && delegate_->adapterClient())
        delegate_->adapterClient()->qutebrowserStatusChanged(modeName(mode_), keychainString(), count_);
}

void QutebrowserKeyDispatcher::setMode(Mode mode)
{
    if (mode_ == mode)
        return;
    const Mode old_mode = mode_;
    mode_ = mode;
    emitModeChanged(old_mode, mode_);
}

void QutebrowserKeyDispatcher::enterMode(Mode mode)
{
    setMode(mode);
    clearKeychain();
}

void QutebrowserKeyDispatcher::leaveMode()
{
    setMode(Mode::kNormal);
    clearKeychain();
}

content::KeyboardEventProcessingResult QutebrowserKeyDispatcher::preHandleKeyboardEvent(
        content::WebContents *contents,
        const input::NativeWebKeyboardEvent &event)
{
    if (!event.os_event)
        return content::KeyboardEventProcessingResult::NOT_HANDLED;

    QKeyEvent *key_event = ToKeyEvent(event.os_event);
    if (!key_event || key_event->type() != QEvent::KeyPress)
        return content::KeyboardEventProcessingResult::NOT_HANDLED;

    const QString key = keyStringForEvent(key_event);
    if (key.isEmpty())
        return content::KeyboardEventProcessingResult::NOT_HANDLED;

    if (mode_ == Mode::kHint) {
        if (key == QStringLiteral("Escape"))
            clearRendererMode();
        return content::KeyboardEventProcessingResult::NOT_HANDLED_IS_SHORTCUT;
    }

    if (mode_ == Mode::kSetMark || mode_ == Mode::kJumpMark) {
        if (key == QStringLiteral("Escape")) {
            leaveMode();
            return content::KeyboardEventProcessingResult::HANDLED;
        }
        const QString command = QStringLiteral("%1 %2").arg(
                mode_ == Mode::kSetMark ? QStringLiteral("set-mark") : QStringLiteral("jump-mark"),
                key);
        emitPythonCommand(command);
        leaveMode();
        return content::KeyboardEventProcessingResult::HANDLED;
    }

    QVector<QString> candidate = sequence_;
    candidate.push_back(key);
    MatchResult result = match(mode_, candidate);

    if (result.type == MatchType::kNoMatch && mode_ == Mode::kNormal &&
        isEditableFocused(contents) &&
        !(key == QStringLiteral("Escape") || key == QStringLiteral("Ctrl+Space"))) {
        enterMode(Mode::kInsert);
        return content::KeyboardEventProcessingResult::NOT_HANDLED;
    }

    if (result.type == MatchType::kNoMatch && shouldPassThroughMode())
        return content::KeyboardEventProcessingResult::NOT_HANDLED;

    if (result.type == MatchType::kNoMatch && isCountKey(key)) {
        count_.append(key);
        emitStatusChanged();
        return content::KeyboardEventProcessingResult::HANDLED;
    }

    if (result.type == MatchType::kPartialMatch) {
        sequence_ = candidate;
        emitStatusChanged();
        return content::KeyboardEventProcessingResult::HANDLED;
    }

    if (result.type == MatchType::kExactMatch) {
        sequence_ = candidate;
        CommandResult command_result = executeCommandList(result.command, contents, key_event);
        clearKeychain();
        if (command_result == CommandResult::kRendererShortcut)
            return content::KeyboardEventProcessingResult::NOT_HANDLED_IS_SHORTCUT;
        if (command_result == CommandResult::kPassThrough)
            return content::KeyboardEventProcessingResult::NOT_HANDLED;
        return content::KeyboardEventProcessingResult::HANDLED;
    }

    clearKeychain();
    if (shouldPassThroughMode())
        return content::KeyboardEventProcessingResult::NOT_HANDLED;
    return content::KeyboardEventProcessingResult::HANDLED;
}

QutebrowserKeyDispatcher::CommandResult QutebrowserKeyDispatcher::executeCommandList(
        const QString &commands, content::WebContents *contents, const QKeyEvent *event)
{
    CommandResult final_result = CommandResult::kHandled;
    const QStringList parts = commands.split(QStringLiteral(";;"));
    for (const QString &part : parts) {
        const CommandResult result = executeCommand(part.trimmed(), contents, event);
        if (result == CommandResult::kRendererShortcut)
            final_result = result;
        else if (result == CommandResult::kPassThrough && final_result != CommandResult::kRendererShortcut)
            final_result = result;
    }
    return final_result;
}

QutebrowserKeyDispatcher::CommandResult QutebrowserKeyDispatcher::executeCommand(
        QString command, content::WebContents *contents, const QKeyEvent *event)
{
    command = command.trimmed();
    if (command.isEmpty())
        return CommandResult::kHandled;

    Mode next_mode = Mode::kNormal;
    if (commandEntersMode(command, &next_mode)) {
        enterMode(next_mode);
        return CommandResult::kHandled;
    }

    if (command == QStringLiteral("mode-leave")) {
        leaveMode();
        return CommandResult::kHandled;
    }

    CommandResult native_result = CommandResult::kHandled;
    if (executeNativeCommand(command, contents, event, &native_result))
        return native_result;

    emitPythonCommand(command);
    return CommandResult::kHandled;
}

bool QutebrowserKeyDispatcher::commandEntersMode(const QString &command, Mode *mode) const
{
    const QString name = normalizedCommandName(command);
    if (name != QStringLiteral("mode-enter"))
        return false;
    const QString arg = commandArgument(command);
    if (arg == QStringLiteral("insert")) *mode = Mode::kInsert;
    else if (arg == QStringLiteral("passthrough")) *mode = Mode::kPassthrough;
    else if (arg == QStringLiteral("caret")) *mode = Mode::kCaret;
    else if (arg == QStringLiteral("set_mark")) *mode = Mode::kSetMark;
    else if (arg == QStringLiteral("jump_mark")) *mode = Mode::kJumpMark;
    else if (arg == QStringLiteral("normal")) *mode = Mode::kNormal;
    else return false;
    return true;
}

bool QutebrowserKeyDispatcher::executeNativeCommand(const QString &command,
                                                    content::WebContents *contents,
                                                    const QKeyEvent *event,
                                                    CommandResult *result)
{
    WebContentsAdapter *adapter = delegate_ ? delegate_->webContentsAdapter() : nullptr;
    const QString name = normalizedCommandName(command);
    const int count = currentCount();

    if (name == QStringLiteral("nop")) {
        *result = CommandResult::kHandled;
        return true;
    }
    if (name == QStringLiteral("fake-key")) {
        *result = CommandResult::kPassThrough;
        return true;
    }
    if (name == QStringLiteral("mode-leave")) {
        leaveMode();
        *result = CommandResult::kHandled;
        return true;
    }
    if (name == QStringLiteral("insert-text")) {
        insertText(commandArgument(command), contents);
        *result = CommandResult::kHandled;
        return true;
    }
    if (name == QStringLiteral("shader-toggle")) {
        toggleElementShader();
        *result = CommandResult::kHandled;
        return true;
    }
    if (name == QStringLiteral("reload") && adapter) {
        command.contains(QStringLiteral("-f")) ? adapter->reloadAndBypassCache() : adapter->reload();
        *result = CommandResult::kHandled;
        return true;
    }
    if (name == QStringLiteral("stop") && adapter) {
        adapter->stop();
        *result = CommandResult::kHandled;
        return true;
    }
    if (name == QStringLiteral("back") && adapter && !command.contains(QStringLiteral("-t")) && !command.contains(QStringLiteral("-w"))) {
        adapter->navigateBack();
        *result = CommandResult::kHandled;
        return true;
    }
    if (name == QStringLiteral("forward") && adapter && !command.contains(QStringLiteral("-t")) && !command.contains(QStringLiteral("-w"))) {
        adapter->navigateForward();
        *result = CommandResult::kHandled;
        return true;
    }
    if (name == QStringLiteral("tab-close") && adapter) {
        if (command.contains(QStringLiteral("-o")))
            return false;
        adapter->requestClose();
        *result = CommandResult::kHandled;
        return true;
    }
    if (name == QStringLiteral("tab-mute") && adapter) {
        adapter->setAudioMuted(!adapter->isAudioMuted());
        *result = CommandResult::kHandled;
        return true;
    }
    if (name == QStringLiteral("download") && adapter) {
        adapter->download(adapter->activeUrl(), QString());
        *result = CommandResult::kHandled;
        return true;
    }
    if (name == QStringLiteral("view-source") && adapter && adapter->canViewSource()) {
        adapter->viewSource();
        *result = CommandResult::kHandled;
        return true;
    }
    if (name == QStringLiteral("open")) {
        openUrlFromCommand(command, contents);
        *result = CommandResult::kHandled;
        return true;
    }
    if (name == QStringLiteral("yank")) {
        yankFromCommand(command);
        *result = CommandResult::kHandled;
        return true;
    }
    if (name == QStringLiteral("scroll-px")) {
        const QStringList args = commandArgument(command).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (args.size() >= 2) {
            smoothScrollBy(args[0].toInt() * count, args[1].toInt() * count, contents);
            *result = CommandResult::kHandled;
            return true;
        }
    }
    if (name == QStringLiteral("scroll")) {
        const QString arg = commandArgument(command);
        if (arg == QStringLiteral("down")) smoothScrollBy(0, kArrowScrollPx * count, contents);
        else if (arg == QStringLiteral("up")) smoothScrollBy(0, -kArrowScrollPx * count, contents);
        else if (arg == QStringLiteral("left")) smoothScrollBy(-kArrowScrollPx * count, 0, contents);
        else if (arg == QStringLiteral("right")) smoothScrollBy(kArrowScrollPx * count, 0, contents);
        else return false;
        *result = CommandResult::kHandled;
        return true;
    }
    if (name == QStringLiteral("scroll-page")) {
        const QStringList args = commandArgument(command).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (args.size() >= 2) {
            const double y = args[1].toDouble();
            int pageHeight = kFallbackPageScrollPx;
            if (delegate_ && delegate_->adapterClient()) {
                const QRectF viewport = delegate_->adapterClient()->viewportRect();
                if (viewport.height() > 0)
                    pageHeight = static_cast<int>(std::round(viewport.height()));
            }
            smoothScrollBy(0, static_cast<int>(std::round(y * pageHeight)) * count, contents);
            *result = CommandResult::kHandled;
            return true;
        }
    }
    if (name == QStringLiteral("scroll-to-perc")) {
        const QStringList args = commandArgument(command).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        const double y = args.isEmpty() ? 100.0 : args.last().toDouble();
        scrollToPercent(0, y, contents);
        *result = CommandResult::kHandled;
        return true;
    }
    if ((name == QStringLiteral("zoom-in") || name == QStringLiteral("zoom-out") || name == QStringLiteral("zoom")) && adapter) {
        if (name == QStringLiteral("zoom-in")) adapter->setZoomFactor(adapter->currentZoomFactor() + 0.1 * count);
        else if (name == QStringLiteral("zoom-out")) adapter->setZoomFactor(adapter->currentZoomFactor() - 0.1 * count);
        else adapter->setZoomFactor(1.0);
        *result = CommandResult::kHandled;
        return true;
    }
    if (isNativeHintEntryCommand(command, event)) {
        enterMode(Mode::kHint);
        *result = CommandResult::kRendererShortcut;
        return true;
    }

    return false;
}

bool QutebrowserKeyDispatcher::isNativeHintEntryCommand(const QString &command, const QKeyEvent *event) const
{
    const QString name = normalizedCommandName(command);
    if (name != QStringLiteral("hint") || !event)
        return false;
    if (event->key() == Qt::Key_F && HasNoOrOnlyShift(event->modifiers()))
        return true;
    if (event->key() == Qt::Key_J && HasOnlyModifier(event->modifiers(), Qt::ControlModifier))
        return true;
    if (event->key() == Qt::Key_K && HasOnlyModifier(event->modifiers(), Qt::ControlModifier))
        return true;
    if (event->key() == Qt::Key_Space && HasOnlyModifier(event->modifiers(), Qt::ControlModifier))
        return true;
    return false;
}

void QutebrowserKeyDispatcher::emitPythonCommand(const QString &command)
{
    if (delegate_ && delegate_->adapterClient())
        delegate_->adapterClient()->qutebrowserCommandRequested(command);
}

QString QutebrowserKeyDispatcher::firstToken(const QString &arguments) const
{
    const QStringList parts = arguments.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        if (part.startsWith(QLatin1Char('-')))
            continue;
        return part;
    }
    return QString();
}

void QutebrowserKeyDispatcher::openUrlFromCommand(const QString &command, content::WebContents *contents)
{
    if (!contents)
        return;

    QString target = firstToken(commandArgument(command));
    if (target == QStringLiteral("{clipboard}")) {
        if (QClipboard *clipboard = QGuiApplication::clipboard())
            target = clipboard->text(QClipboard::Clipboard).trimmed();
    } else if (target == QStringLiteral("{primary}")) {
        if (QClipboard *clipboard = QGuiApplication::clipboard())
            target = clipboard->text(QClipboard::Selection).trimmed();
    }
    if (target.isEmpty())
        return;

    QUrl url = QUrl::fromUserInput(target);
    if (!url.isValid())
        return;

    WindowOpenDisposition disposition = command.contains(QStringLiteral("-t"))
            ? WindowOpenDisposition::NEW_FOREGROUND_TAB
            : WindowOpenDisposition::CURRENT_TAB;
    const GURL gurl(url.toString().toStdString());
    if (!gurl.is_valid())
        return;

    content::OpenURLParams params(gurl,
                                  content::Referrer(),
                                  disposition,
                                  ui::PAGE_TRANSITION_TYPED,
                                  false);
    contents->OpenURL(params, {});
}

void QutebrowserKeyDispatcher::yankFromCommand(const QString &command)
{
    WebContentsAdapter *adapter = delegate_ ? delegate_->webContentsAdapter() : nullptr;
    if (!adapter)
        return;

    const QString args = commandArgument(command);
    QString text;
    const QUrl url = adapter->activeUrl();
    const QString urlText = url.toDisplayString(QUrl::RemovePassword);
    if (args.contains(QStringLiteral("selection"))) {
        text = adapter->selectedText();
    } else if (args.contains(QStringLiteral("title"))) {
        text = adapter->pageTitle();
    } else if (args.contains(QStringLiteral("domain"))) {
        text = url.host();
    } else if (args.contains(QStringLiteral("inline"))) {
        text = QStringLiteral("[%1](%2)").arg(adapter->pageTitle(), urlText);
    } else {
        text = urlText;
    }

    if (text.isEmpty())
        return;

    if (QClipboard *clipboard = QGuiApplication::clipboard()) {
        const bool useSelection = args.contains(QStringLiteral("-s"));
        if (useSelection && clipboard->supportsSelection())
            clipboard->setText(text, QClipboard::Selection);
        else
            clipboard->setText(text, QClipboard::Clipboard);
    }
}

void QutebrowserKeyDispatcher::smoothScrollBy(int dx, int dy, content::WebContents *contents)
{
    WebContentsAdapter *adapter = delegate_ ? delegate_->webContentsAdapter() : nullptr;
    if (!adapter)
        return;

    auto fallbackScroll = [this, dx, dy]() {
        if (WebContentsAdapter *fallbackAdapter = delegate_ ? delegate_->webContentsAdapter() : nullptr)
            fallbackAdapter->smoothScrollBy(dx, dy, kSmoothScrollFactor);
    };

    if (!contents || !contents->GetPrimaryMainFrame()) {
        fallbackScroll();
        return;
    }

    const QString js = QString::fromLatin1(R"JS(
(function(dx, dy) {
  try {
    const scrollableOverflows = new Set(['auto', 'scroll', 'overlay']);
    const isDocumentScroller = (elem) => {
      const doc = elem && elem.ownerDocument;
      return !!doc && (elem === doc.scrollingElement ||
                       elem === doc.documentElement || elem === doc.body);
    };
    const isScrollable = (elem) => {
      if (!elem) return false;
      if (isDocumentScroller(elem)) {
        const style = elem.ownerDocument.defaultView.getComputedStyle(elem);
        const blocksY = style.overflowY === 'hidden' || style.overflowY === 'clip';
        const blocksX = style.overflowX === 'hidden' || style.overflowX === 'clip';
        return (dy !== 0 && elem.scrollHeight > elem.clientHeight && !blocksY) ||
               (dx !== 0 && elem.scrollWidth > elem.clientWidth && !blocksX);
      }
      const style = elem.ownerDocument.defaultView.getComputedStyle(elem);
      return (dy !== 0 && scrollableOverflows.has(style.overflowY) &&
              elem.scrollHeight > elem.clientHeight) ||
             (dx !== 0 && scrollableOverflows.has(style.overflowX) &&
              elem.scrollWidth > elem.clientWidth);
    };
    const findScrollable = (start) => {
      const doc = document.documentElement;
      const body = document.body;
      let current = start;
      while (current && current !== body && current !== doc) {
        if (isScrollable(current)) return current;
        current = current.parentElement;
      }
      return null;
    };
    const deepActiveElement = (root = document) => {
      const active = root.activeElement;
      if (!active) return active;
      if (active.shadowRoot && active.shadowRoot.activeElement)
        return deepActiveElement(active.shadowRoot);
      return active;
    };
    const selectedScrollable = () => {
      const selector = '[data-qutebrowser-scroll-target="1"]';
      const findIn = (root) => {
        if (!root || !root.querySelector) return null;
        const found = root.querySelector(selector);
        if (found) return found;
        const all = root.querySelectorAll ? root.querySelectorAll('*') : [];
        for (const elem of all) {
          if (elem.shadowRoot) {
            const foundInShadow = findIn(elem.shadowRoot);
            if (foundInShadow) return foundInShadow;
          }
        }
        return null;
      };
      return findIn(document);
    };
    const centerFor = (target) => {
      if (!target) return null;
      if (isDocumentScroller(target)) return [-1, -1];
      const scrollTarget = isScrollable(target) ? target : findScrollable(target);
      if (!scrollTarget) return null;
      const rect = scrollTarget.getBoundingClientRect();
      return [Math.round(rect.left + rect.width / 2),
              Math.round(rect.top + rect.height / 2)];
    };
    return centerFor(selectedScrollable()) || centerFor(deepActiveElement());
  } catch (e) {
    return null;
  }
})(%1, %2);
)JS").arg(dx).arg(dy);

    contents->GetPrimaryMainFrame()->ExecuteJavaScript(
            js.toStdU16String(),
            base::BindOnce([](base::WeakPtr<WebContentsDelegateQt> delegate, int scrollDx, int scrollDy, base::Value result) {
        if (!delegate)
            return;
        WebContentsAdapter *callbackAdapter = delegate->webContentsAdapter();
        if (!callbackAdapter)
            return;

        int posX = -1;
        int posY = -1;
        if (const base::Value::List *list = result.GetIfList(); list && list->size() >= 2) {
            auto toInt = [](const base::Value &value, int *out) {
                if (std::optional<int> i = value.GetIfInt()) {
                    *out = *i;
                    return true;
                }
                if (std::optional<double> d = value.GetIfDouble()) {
                    *out = static_cast<int>(std::round(*d));
                    return true;
                }
                return false;
            };
            int x = -1;
            int y = -1;
            if (toInt((*list)[0], &x) && toInt((*list)[1], &y)) {
                posX = x;
                posY = y;
            }
        }
        callbackAdapter->smoothScrollBy(scrollDx, scrollDy, kSmoothScrollFactor, posX, posY);
    }, delegate_->AsWeakPtr(), dx, dy));
}

QString QutebrowserKeyDispatcher::jsStringLiteral(const QString &text)
{
    QString escaped = text;
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    escaped.replace(QLatin1Char('\''), QStringLiteral("\\'"));
    escaped.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    escaped.replace(QLatin1Char('\r'), QStringLiteral("\\r"));
    return QStringLiteral("'") + escaped + QStringLiteral("'");
}

void QutebrowserKeyDispatcher::scrollToPercent(double, double y, content::WebContents *contents)
{
    WebContentsAdapter *adapter = delegate_ ? delegate_->webContentsAdapter() : nullptr;
    if (adapter && delegate_ && delegate_->adapterClient()) {
        const QRectF viewport = delegate_->adapterClient()->viewportRect();
        const QSizeF contentsSize = adapter->lastContentsSize();
        const QPointF position = adapter->lastScrollOffset();
        if (viewport.height() > 0 && contentsSize.height() > 0) {
            const double maxY = std::max(0.0, contentsSize.height() - viewport.height());
            const double clampedY = std::clamp(y, 0.0, 100.0);
            const double targetY = maxY * clampedY / 100.0;
            const int dy = static_cast<int>(std::round(targetY - position.y()));
            if (dy != 0)
                adapter->smoothScrollBy(0, dy, kSmoothScrollFactor);
            return;
        }
    }

    if (!contents)
        return;
    const QString js = QStringLiteral(
            "window.scrollTo({left: window.scrollX, top: "
            "(document.scrollingElement || document.documentElement).scrollHeight * %1 / 100, "
            "behavior: 'smooth'});").arg(y);
    contents->GetPrimaryMainFrame()->ExecuteJavaScript(js.toStdU16String(), content::RenderFrameHost::JavaScriptResultCallback());
}

void QutebrowserKeyDispatcher::insertText(const QString &text, content::WebContents *contents)
{
    if (!contents)
        return;
    const QString js = QStringLiteral(
            "(function(t){var e=document.activeElement;if(!e)return;"
            "if(document.queryCommandSupported&&document.queryCommandSupported('insertText'))"
            "{document.execCommand('insertText',false,t);return;}"
            "var d=new DataTransfer();d.setData('text/plain',t);"
            "e.dispatchEvent(new ClipboardEvent('paste',{clipboardData:d,bubbles:true,cancelable:true}));"
            "})(%1);").arg(jsStringLiteral(text));
    contents->GetPrimaryMainFrame()->ExecuteJavaScript(js.toStdU16String(), content::RenderFrameHost::JavaScriptResultCallback());
}

void QutebrowserKeyDispatcher::toggleElementShader()
{
    if (!delegate_ || !delegate_->webEngineSettings())
        return;
    WebEngineSettings *settings = delegate_->webEngineSettings()->rootSettings();
    const bool enabled = !settings->testAttribute(QWebEngineSettings::ElementShaderEnabled);
    settings->setAttribute(QWebEngineSettings::ElementShaderEnabled, enabled);
}

} // namespace QtWebEngineCore
