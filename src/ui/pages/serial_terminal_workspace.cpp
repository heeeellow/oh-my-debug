#include "serial_terminal_workspace.h"
#include "ui/icons.h"
#include "ui/theme.h"
#include "ui/widgets/serial_connection_bar.h"
#include "ui/widgets/serial_keyword_highlighter.h"
#include "ui/widgets/serial_receive_view.h"
#include "ui/widgets/serial_shortcut_sidebar.h"
#include "ui/widgets/serial_workspace_helpers.h"

#include "application/serial/serial_transport_service.h"
#include "core/serial/codec.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QEvent>
#include <QtCore/QFile>
#include <QtCore/QMetaObject>
#include <QtCore/QPointer>
#include <QtCore/QSignalBlocker>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtGui/QActionGroup>
#include <QtGui/QClipboard>
#include <QtGui/QInputMethodEvent>
#include <QtGui/QKeyEvent>
#include <QtGui/QPainter>
#include <QtGui/QResizeEvent>
#include <QtGui/QShortcut>
#include <QtGui/QTextBlock>
#include <QtGui/QTextCharFormat>
#include <QtGui/QTextCursor>
#include <QtWidgets/QApplication>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QStyle>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <functional>
#include <utility>

using namespace serialdemo;

using namespace testtools::ui::serial;

SerialTerminalWorkspace::SerialTerminalWorkspace(
    QWidget* parent, std::shared_ptr<testtools::SerialPortClaims> claims)
    : SerialEndpointWorkspace(parent, 1, std::move(claims))
{
    const bool first = initializePreferences(L"terminal-preferences.json", true);
    buildPageHeader(QStringLiteral("串口终端"),
                    QStringLiteral("直接输入 · ↑↓ 历史 · Tab 补全 · Ctrl+C 中断"));
    static_cast<QVBoxLayout*>(layout())->addWidget(buildChannelPanel(0), 1);
    applyPreferencesToUi();
    applyChannelLayout(true);
    auto& channel = channels_[0];
    channel.headerLayout->removeWidget(channel.actionsContainer);
    channel.settingsRowLayout->addWidget(channel.actionsContainer);
    channel.headerWidget->hide();
    channel.portCombo->setMaximumWidth(320);
    channel.receiveTitle->setText(QStringLiteral("终端"));
    channel.clearReceiveButton->setText(QStringLiteral("清屏"));
    channel.clearReceiveButton->setToolTip(
        QStringLiteral("清空终端视图，保留字节计数与普通收发记录，不发送数据"));
    channel.searchButton->setToolTip(QStringLiteral("搜索终端历史（Ctrl+Shift+F）"));
    channel.receiveEdit->setLineWrapMode(QPlainTextEdit::NoWrap);
    channel.receiveEdit->setAttribute(Qt::WA_InputMethodEnabled, true);
    channel.receiveEdit->setProperty("terminalInput", true);
    channel.receiveEdit->setPlaceholderText(
        QStringLiteral("连接后直接输入 · Enter 执行 · ↑↓ 历史 · Tab 补全 · Ctrl+C 中断"));
    channel.receiveHexCheck->hide();
    channel.timestampCheck->hide();
    channel.autoScrollCheck->hide();
    channel.counterLabel->hide();
    channel.resetCounterButton->hide();
    channel.filterCheck->hide();
    installSearchShortcuts();
    finishSetup();
    renderTerminal(true);
    focusTerminal();
    if (first)
        savePreferences();
}
SerialTerminalWorkspace::~SerialTerminalWorkspace()
{
    prepareForShutdown();
}
bool SerialTerminalWorkspace::selectWorkspaceMode(const QString& mode)
{
    if (mode.trimmed().toLower() != QStringLiteral("terminal"))
        return false;
    resumeTerminalInput();
    focusTerminal();
    return true;
}
void SerialTerminalWorkspace::setupReceiveInput(int)
{
    auto& channel = channels_[0];

    static_cast<ReceiveConsole*>(channel.receiveEdit)->scrollFollowChanged = [this](bool follow) {
        channels_[0].autoScrollCheck->setChecked(follow);
    };
    connect(channel.receiveEdit->verticalScrollBar(),
            &QScrollBar::sliderMoved,
            this,
            [this](int position) {
                channels_[0].autoScrollCheck->setChecked(
                    position >= channels_[0].receiveEdit->verticalScrollBar()->maximum());
            });
    channel.receiveEdit->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(channel.receiveEdit,
            &QPlainTextEdit::customContextMenuRequested,
            this,
            [this](const QPoint& point) {
                auto* edit = channels_[0].receiveEdit;
                QMenu menu(this);
                auto* copy = menu.addAction(
                    QStringLiteral("复制\tCtrl+Shift+C"), edit, &QPlainTextEdit::copy);
                copy->setEnabled(edit->textCursor().hasSelection());
                auto* paste = menu.addAction(QStringLiteral("粘贴\tCtrl+Shift+V"), this, [this] {
                    const auto bytes = testtools::terminalPasteBytes(
                        toUtf8(QApplication::clipboard()->text()), terminalLineEnding());
                    if (!bytes.empty()) {
                        resumeTerminalInput();
                        sendBytes(0, bytes);
                    }
                });
                paste->setEnabled(channels_[0].session.isOpen());
                menu.addSeparator();
                auto* interrupt =
                    menu.addAction(QStringLiteral("中断当前命令\tCtrl+C"), this, [this] {
                        resumeTerminalInput();
                        sendBytes(0, {0x03});
                    });
                interrupt->setEnabled(channels_[0].session.isOpen());
                menu.exec(edit->mapToGlobal(point));
                focusTerminal();
            });
}
bool SerialTerminalWorkspace::handleReceiveEvent(QObject* watched, QEvent* event)
{
    if (watched == channels_[0].receiveEdit) {
        if (event->type() == QEvent::ShortcutOverride) {
            auto* key = static_cast<QKeyEvent*>(event);
            if (!(key->modifiers().testFlag(Qt::ControlModifier) &&
                  key->modifiers().testFlag(Qt::ShiftModifier) && key->key() == Qt::Key_F) &&
                key->key() != Qt::Key_F3) {
                key->accept();
                return true;
            }
        }
        if (event->type() == QEvent::InputMethod) {
            const auto text = toUtf8(static_cast<QInputMethodEvent*>(event)->commitString());
            if (!text.empty()) {
                resumeTerminalInput();
                sendBytes(0, {text.begin(), text.end()});
            }
            return true;
        }
        if (event->type() == QEvent::InputMethodQuery) {
            auto* query = static_cast<QInputMethodQueryEvent*>(event);
            query->setValue(Qt::ImEnabled, true);
            query->setValue(
                Qt::ImCursorRectangle,
                static_cast<ReceiveConsole*>(channels_[0].receiveEdit)->terminalCursorRect());
            query->setValue(Qt::ImFont, channels_[0].receiveEdit->font());
            query->setValue(Qt::ImSurroundingText, QString{});
            query->setValue(Qt::ImCursorPosition, 0);
            query->setValue(Qt::ImAnchorPosition, 0);
            return true;
        }
        if (event->type() == QEvent::KeyPress) {
            auto* key = static_cast<QKeyEvent*>(event);
            if (!(key->key() == Qt::Key_Escape && channels_[0].searchButton->isChecked()) &&
                handleTerminalKey(key))
                return true;
        }
    }
    return false;
}
void SerialTerminalWorkspace::streamReset(int)
{
    terminalBuffer_.resetStream();
    scheduleTerminalRender();
}
void SerialTerminalWorkspace::clearReceive(int)
{
    terminalBuffer_.clear();
    renderTerminal(true);
}
bool SerialTerminalWorkspace::historyTruncated(int) const
{
    return terminalBuffer_.truncated();
}
void SerialTerminalWorkspace::renderReceiveLog(int)
{
    renderTerminal(true);
}
void SerialTerminalWorkspace::applyLogAppendResult(int index,
                                                   const testtools::SerialLogAppendResult& result)
{
    if (channels_[index].receivingBatch)
        SerialEndpointWorkspace::applyLogAppendResult(index, result);
    else
        scheduleTerminalRender();
}
void SerialTerminalWorkspace::appendReceived(int index, const ByteBuffer& data)
{
    auto& channel = channels_[index];
    const auto now = QDateTime::currentDateTime();
    const auto timestamp = toUtf8(now.toString(QStringLiteral("HH:mm:ss.zzz")));
    const auto result = channel.logBuffer.appendRx(data, now.toMSecsSinceEpoch(), timestamp);
    terminalBuffer_.push(data, timestamp);
    applyLogAppendResult(index, result);
    updateCounter(index);
}
void SerialTerminalWorkspace::stopPageTasks()
{
    if (terminalRenderTimer_)
        terminalRenderTimer_->stop();
}

void SerialTerminalWorkspace::buildReceiveOptions(QHBoxLayout* header)
{
    auto& channel = channels_[0];
    terminalOptionsButton_ = new QPushButton(QStringLiteral("终端选项…"), this);
    terminalOptionsButton_->setObjectName(QStringLiteral("clearButton"));
    auto* menu = new QMenu(terminalOptionsButton_);
    auto* endings = menu->addMenu(QStringLiteral("回车格式"));
    auto* endingGroup = new QActionGroup(endings);
    const std::array<QString, 3> names{
        QStringLiteral("CR"), QStringLiteral("LF"), QStringLiteral("CRLF")};
    const std::array<int, 3> values{2, 1, 3};
    for (std::size_t i = 0; i < names.size(); ++i) {
        auto* action = endings->addAction(names[i]);
        action->setCheckable(true);
        action->setData(values[i]);
        action->setChecked(values[i] == preferences_.terminalLineEnding);
        endingGroup->addAction(action);
        terminalEndingActions_[i] = action;
        connect(action, &QAction::triggered, this, [this, value = values[i]] {
            preferences_.terminalLineEnding = value;
            savePreferences();
        });
    }
    auto* backspace = menu->addMenu(QStringLiteral("退格键"));
    auto* backspaceGroup = new QActionGroup(backspace);
    for (std::size_t i = 0; i < 2; ++i) {
        const int value = i == 0 ? 127 : 8;
        auto* action =
            backspace->addAction(i == 0 ? QStringLiteral("DEL (7F)") : QStringLiteral("BS (08)"));
        action->setCheckable(true);
        action->setData(value);
        action->setChecked(value == preferences_.terminalBackspace);
        backspaceGroup->addAction(action);
        terminalBackspaceActions_[i] = action;
        connect(action, &QAction::triggered, this, [this, value] {
            preferences_.terminalBackspace = value;
            savePreferences();
        });
    }
    menu->addSeparator();
    for (auto* check : {channel.timestampCheck, channel.autoScrollCheck}) {
        auto* action = menu->addAction(check->text());
        action->setCheckable(true);
        action->setChecked(check->isChecked());
        connect(action, &QAction::toggled, check, &QCheckBox::setChecked);
        connect(check, &QCheckBox::toggled, action, &QAction::setChecked);
        connect(menu, &QMenu::aboutToShow, this, [check, action] {
            const QSignalBlocker blocker(action);
            action->setChecked(check->isChecked());
        });
    }
    menu->addAction(QStringLiteral("计数清零"), this, [this] { resetChannelCounters(0); });
    menu->addSeparator();
    menu->addAction(QStringLiteral("键盘说明…"), this, [this] {
        QMessageBox::information(
            this,
            QStringLiteral("串口终端键盘"),
            QStringLiteral("直接输入，Enter 执行；↑/↓ 历史、Tab 补全由设备处理。\n"
                           "Ctrl+C：中断设备前台命令，例如停止 ping。\n"
                           "Ctrl+Shift+C / V：复制 / 粘贴。\n"
                           "Ctrl+Shift+F：搜索；Shift+PageUp / PageDown：回看输出。\n"
                           "终端只显示设备回显；仅有内核日志不代表设备已开放交互式 Shell。"));
    });
    connect(menu, &QMenu::aboutToHide, this, [this] {
        QTimer::singleShot(0, this, &SerialTerminalWorkspace::focusTerminal);
    });
    terminalOptionsButton_->setMenu(menu);
    header->addWidget(terminalOptionsButton_);
    terminalRenderTimer_ = new QTimer(this);
    terminalRenderTimer_->setSingleShot(true);
    terminalRenderTimer_->setInterval(16);
    connect(terminalRenderTimer_, &QTimer::timeout, this, [this] { renderTerminal(); });
}

LineEnding SerialTerminalWorkspace::terminalLineEnding() const
{
    return lineEndingFromIndex(preferences_.terminalLineEnding);
}

void SerialTerminalWorkspace::focusTerminal()
{
    if (channels_[0].receiveEdit)
        channels_[0].receiveEdit->setFocus(Qt::OtherFocusReason);
}

void SerialTerminalWorkspace::updateTerminalCursor(bool follow)
{
    auto* edit = channels_[0].receiveEdit;
    const auto row = terminalBuffer_.cursorRow();
    const auto& line = terminalBuffer_.lines()[row];
    const auto block = edit->document()->findBlockByNumber(static_cast<int>(row));
    if (!block.isValid())
        return;
    int offset = channels_[0].timestampCheck->isChecked() && !line.timestamp.empty()
                     ? static_cast<int>(fromUtf8(line.timestamp).size()) + 3
                     : 0;
    for (std::size_t i = 0; i < std::min(terminalBuffer_.cursorColumn(), line.cells.size()); ++i)
        offset += line.cells[i].character > 0xffff ? 2 : 1;
    QTextCursor caret(block);
    caret.setPosition(std::min(block.position() + offset, edit->document()->characterCount() - 1));
    static_cast<ReceiveConsole*>(edit)->setTerminalCursor(caret);
    if (follow && !edit->textCursor().hasSelection()) {
        edit->setTextCursor(caret);
        edit->ensureCursorVisible();
    }
}

void SerialTerminalWorkspace::resumeTerminalInput()
{
    auto& channel = channels_[0];
    channel.searchButton->setChecked(false);
    channel.autoScrollCheck->setChecked(true);
    auto cursor = channel.receiveEdit->textCursor();
    cursor.clearSelection();
    channel.receiveEdit->setTextCursor(cursor);
    renderTerminal();
    updateTerminalCursor(true);
}

bool SerialTerminalWorkspace::handleTerminalKey(QKeyEvent* event)
{
    const auto key = event->key();
    const auto modifiers = event->modifiers();
    const bool control = modifiers.testFlag(Qt::ControlModifier);
    const bool shift = modifiers.testFlag(Qt::ShiftModifier);
    auto* edit = channels_[0].receiveEdit;
    if ((control && shift && key == Qt::Key_F) || key == Qt::Key_F3)
        return false;
    if (control && shift && key == Qt::Key_C) {
        edit->copy();
        return true;
    }
    if (shift && (key == Qt::Key_PageUp || key == Qt::Key_PageDown)) {
        channels_[0].autoScrollCheck->setChecked(false);
        auto* scroll = edit->verticalScrollBar();
        scroll->setValue(scroll->value() + (key == Qt::Key_PageUp ? -1 : 1) * scroll->pageStep());
        return true;
    }
    if ((control && shift && key == Qt::Key_V) || (shift && key == Qt::Key_Insert)) {
        const auto bytes = testtools::terminalPasteBytes(toUtf8(QApplication::clipboard()->text()),
                                                         terminalLineEnding());
        if (!bytes.empty()) {
            resumeTerminalInput();
            sendBytes(0, bytes);
        }
        return true;
    }
    if (control && key >= Qt::Key_A && key <= Qt::Key_Z) {
        resumeTerminalInput();
        sendBytes(0, {static_cast<std::uint8_t>(key - Qt::Key_A + 1)});
        return true;
    }
    using Key = testtools::TerminalKey;
    ByteBuffer bytes;
    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        bytes = testtools::terminalKeyBytes(Key::Enter, terminalLineEnding());
        break;
    case Qt::Key_Tab:
        bytes = testtools::terminalKeyBytes(Key::Tab, terminalLineEnding());
        break;
    case Qt::Key_Backspace:
        bytes = {static_cast<std::uint8_t>(preferences_.terminalBackspace)};
        break;
    case Qt::Key_Up:
        bytes = testtools::terminalKeyBytes(Key::Up, terminalLineEnding());
        break;
    case Qt::Key_Down:
        bytes = testtools::terminalKeyBytes(Key::Down, terminalLineEnding());
        break;
    case Qt::Key_Right:
        bytes = testtools::terminalKeyBytes(Key::Right, terminalLineEnding());
        break;
    case Qt::Key_Left:
        bytes = testtools::terminalKeyBytes(Key::Left, terminalLineEnding());
        break;
    case Qt::Key_Home:
        bytes = testtools::terminalKeyBytes(Key::Home, terminalLineEnding());
        break;
    case Qt::Key_End:
        bytes = testtools::terminalKeyBytes(Key::End, terminalLineEnding());
        break;
    case Qt::Key_Delete:
        bytes = testtools::terminalKeyBytes(Key::Delete, terminalLineEnding());
        break;
    case Qt::Key_PageUp:
        bytes = testtools::terminalKeyBytes(Key::PageUp, terminalLineEnding());
        break;
    case Qt::Key_PageDown:
        bytes = testtools::terminalKeyBytes(Key::PageDown, terminalLineEnding());
        break;
    case Qt::Key_Escape:
        bytes = testtools::terminalKeyBytes(Key::Escape, terminalLineEnding());
        break;
    default: {
        if (control || modifiers.testFlag(Qt::MetaModifier))
            return false;
        auto text = toUtf8(event->text());
        if (modifiers.testFlag(Qt::AltModifier) && !text.empty())
            text.insert(text.begin(), '\x1b');
        bytes.assign(text.begin(), text.end());
        break;
    }
    }
    if (bytes.empty())
        return false;
    resumeTerminalInput();
    sendBytes(0, bytes);
    return true;
}

void SerialTerminalWorkspace::scheduleTerminalRender()
{
    if (!terminalRenderTimer_->isActive())
        terminalRenderTimer_->start();
}

void SerialTerminalWorkspace::renderTerminal(bool fullRedraw)
{
    auto& channel = channels_[0];
    auto* edit = channel.receiveEdit;
    auto* scroll = edit->verticalScrollBar();
    const int previousScroll = scroll->value();
    const bool follow = channel.autoScrollCheck->isChecked() && !edit->textCursor().hasSelection();
    const bool timestamps = channel.timestampCheck->isChecked();
    const auto& lines = terminalBuffer_.lines();
    channel.filteredRenderPending = false;
    updateTruncation(0);
    if (!fullRedraw && !terminalBuffer_.dirty())
        return;

    // 搜索仅高亮终端历史匹配项，不从终端屏幕裁剪未匹配行。
    std::size_t first = terminalBuffer_.firstChangedLine();
    const auto removed = terminalBuffer_.removedLines();
    const bool rebuild = fullRedraw || terminalBuffer_.needsFullRedraw() ||
                         removed >= std::size_t(edit->document()->blockCount());
    QTextCursor cursor(edit->document());
    cursor.beginEditBlock();
    if (!rebuild && removed) {
        cursor.setPosition(edit->document()->findBlockByNumber(int(removed)).position());
        cursor.movePosition(QTextCursor::Start, QTextCursor::KeepAnchor);
        cursor.removeSelectedText();
    }
    const auto block = edit->document()->findBlockByNumber(static_cast<int>(first));
    if (rebuild || !block.isValid()) {
        cursor.select(QTextCursor::Document);
        cursor.removeSelectedText();
        first = 0;
    } else {
        cursor.setPosition(block.position());
        cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
        cursor.removeSelectedText();
    }
    bool insertedLine = false;
    for (std::size_t i = first; i < lines.size(); ++i) {
        const auto& line = lines[i];
        const auto prefix =
            timestamps && !line.timestamp.empty() ? "[" + line.timestamp + "] " : std::string{};
        if (insertedLine)
            cursor.insertText(QStringLiteral("\n"), QTextCharFormat{});
        insertedLine = true;
        if (!prefix.empty())
            cursor.insertText(fromUtf8(prefix),
                              logEntryFormat(testtools::SerialLogDirection::Notice));
        for (std::size_t cell = 0; cell < line.cells.size();) {
            const auto& style = line.cells[cell].style;
            std::u32string text;
            do {
                text += line.cells[cell++].character;
            } while (cell < line.cells.size() && line.cells[cell].style == style);
            QTextCharFormat format;
            auto foreground = style.foreground == testtools::TerminalStyle::DefaultColor
                                  ? 0x302f2du
                                  : style.foreground;
            auto background = style.background == testtools::TerminalStyle::DefaultColor
                                  ? 0xfaf9f7u
                                  : style.background;
            if (style.inverse)
                std::swap(foreground, background);
            format.setForeground(QColor::fromRgb(foreground));
            format.setBackground(QColor::fromRgb(background));
            format.setFontWeight(style.bold ? QFont::Bold : QFont::Normal);
            format.setFontUnderline(style.underline);
            cursor.insertText(QString::fromUcs4(text.data(), static_cast<qsizetype>(text.size())),
                              format);
        }
    }
    cursor.endEditBlock();
    terminalBuffer_.acknowledgeChanges();
    updateTerminalCursor(follow);
    updateSearchMatches(0);
    const int retainedScroll =
        rebuild ? previousScroll : std::max(0, previousScroll - int(removed));
    scroll->setValue(follow ? scroll->maximum() : std::min(retainedScroll, scroll->maximum()));
}
