// Copyright (C) 2026 Yeyito. All rights reserved.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#ifndef QUTEBROWSER_KEY_DISPATCHER_H
#define QUTEBROWSER_KEY_DISPATCHER_H

#include "content/public/browser/keyboard_event_processing_result.h"

#include <QtCore/qstring.h>
#include <QtCore/qvector.h>

#include <map>
#include <memory>

QT_FORWARD_DECLARE_CLASS(QKeyEvent)

namespace content {
class WebContents;
}

namespace input {
struct NativeWebKeyboardEvent;
}

namespace QtWebEngineCore {

class WebContentsDelegateQt;

// Chromium-side qutebrowser mode/keybinding dispatcher.
//
// This intentionally lives in QtWebEngine's browser process instead of in the
// qutebrowser Python event filter.  The browser process can decide whether a
// key is a browser command (consume it before the page), page input (let it
// through), or a trusted renderer-local browser shortcut (e.g. native hints).
class QutebrowserKeyDispatcher
{
public:
    explicit QutebrowserKeyDispatcher(WebContentsDelegateQt *delegate);

    content::KeyboardEventProcessingResult preHandleKeyboardEvent(
            content::WebContents *contents,
            const input::NativeWebKeyboardEvent &event);

    void clearRendererMode();

private:
    enum class Mode {
        kNormal,
        kInsert,
        kHint,
        kPassthrough,
        kCommand,
        kPrompt,
        kYesno,
        kCaret,
        kSetMark,
        kJumpMark,
        kRegister,
    };

    enum class CommandResult {
        kHandled,
        kPassThrough,
        kRendererShortcut,
    };

    struct BindingNode {
        std::map<QString, std::unique_ptr<BindingNode>> children;
        QString command;
    };

    enum class MatchType {
        kNoMatch,
        kPartialMatch,
        kExactMatch,
    };

    struct MatchResult {
        MatchType type = MatchType::kNoMatch;
        QString command;
    };

    void loadDefaultBindings();
    void addBinding(Mode mode, const QVector<QString> &sequence, const QString &command);
    void removeBinding(Mode mode, const QVector<QString> &sequence);
    MatchResult match(Mode mode, const QVector<QString> &sequence) const;

    QString keyStringForEvent(const QKeyEvent *event) const;
    bool isEditableFocused(content::WebContents *contents) const;
    bool shouldPassThroughMode() const;
    bool isCountKey(const QString &key) const;
    int currentCount() const;
    void clearKeychain();
    QString modeName(Mode mode) const;
    void setMode(Mode mode);
    void emitModeChanged(Mode oldMode, Mode newMode);
    void emitStatusChanged();
    QString keychainString() const;
    void enterMode(Mode mode);
    void leaveMode();

    CommandResult executeCommandList(const QString &commands,
                                     content::WebContents *contents,
                                     const QKeyEvent *event);
    CommandResult executeCommand(QString command,
                                 content::WebContents *contents,
                                 const QKeyEvent *event);

    bool executeNativeCommand(const QString &command,
                              content::WebContents *contents,
                              const QKeyEvent *event,
                              CommandResult *result);
    void emitPythonCommand(const QString &command);
    QString firstToken(const QString &arguments) const;
    void openUrlFromCommand(const QString &command, content::WebContents *contents);
    void yankFromCommand(const QString &command);
    void smoothScrollBy(int dx, int dy, content::WebContents *contents);
    void scrollToPercent(double x, double y, content::WebContents *contents);
    void insertText(const QString &text, content::WebContents *contents);
    void toggleElementShader();
    bool isNativeHintEntryCommand(const QString &command, const QKeyEvent *event) const;
    bool commandEntersMode(const QString &command, Mode *mode) const;
    static QString jsStringLiteral(const QString &text);

    WebContentsDelegateQt *delegate_ = nullptr;
    Mode mode_ = Mode::kNormal;
    QVector<QString> sequence_;
    QString count_;
    std::map<Mode, std::unique_ptr<BindingNode>> bindings_;
};

} // namespace QtWebEngineCore

#endif // QUTEBROWSER_KEY_DISPATCHER_H
