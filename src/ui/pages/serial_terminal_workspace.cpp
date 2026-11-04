#include "serial_terminal_workspace.h"
#include "application/serial/serial_terminal.h"
#include "ui/widgets/serial_workspace_helpers.h"
#include "ui/widgets/terminal_grid_widget.h"
#include <QtCore/QMetaObject>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtGui/QActionGroup>
#include <QtGui/QClipboard>
#include <QtGui/QKeyEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFontComboBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>
using namespace testtools::ui::serial;
namespace {
int modifiers(Qt::KeyboardModifiers m)
{
    return (m.testFlag(Qt::ShiftModifier) ? 1 : 0) | (m.testFlag(Qt::AltModifier) ? 2 : 0) |
           (m.testFlag(Qt::ControlModifier) ? 4 : 0);
}
} // namespace
SerialTerminalWorkspace::SerialTerminalWorkspace(
    QWidget* parent, std::shared_ptr<testtools::SerialPortClaims> claims)
    : SerialEndpointWorkspace(parent, 1, std::move(claims))
{
    const bool first = initializePreferences(L"terminal-preferences.json", true);
    QPointer<SerialTerminalWorkspace> guard(this);
    const auto notice = frameNotice_;
    terminalEngine_ = std::make_shared<testtools::TerminalEngine>(
        [guard, notice] {
            if (!notice->exchange(true))
                QMetaObject::invokeMethod(
                    guard,
                    [guard] {
                        if (guard && !guard->shuttingDown_)
                            guard->renderTerminal();
                    },
                    Qt::QueuedConnection);
        },
        [guard](std::uint64_t generation, const std::vector<std::uint8_t>& bytes) {
            QMetaObject::invokeMethod(
                guard,
                [guard, generation, bytes] {
                    if (guard && !guard->shuttingDown_ && guard->channels_[0].session.isOpen() &&
                        guard->channels_[0].session.generation() == generation)
                        guard->sendBytes(0, bytes, false);
                },
                Qt::QueuedConnection);
        });
    buildPageHeader(QStringLiteral("串口终端"),
                    QStringLiteral("直接输入 · ↑↓ 历史 · Tab 补全 · Ctrl+C 复制 / 中断"));
    static_cast<QVBoxLayout*>(layout())->addWidget(buildChannelPanel(0), 1);
    applyPreferencesToUi();
    applyChannelLayout(true);
    auto& c = channels_[0];
    c.headerLayout->removeWidget(c.actionsContainer);
    c.settingsRowLayout->addWidget(c.actionsContainer);
    c.headerWidget->hide();
    c.portCombo->setMaximumWidth(320);
    c.receiveTitle->setText(QStringLiteral("终端"));
    c.clearReceiveButton->setText(QStringLiteral("清屏"));
    c.clearReceiveButton->setToolTip(QStringLiteral("清空本地终端，不发送数据，不影响持续录制"));
    c.searchButton->setToolTip(QStringLiteral("搜索终端历史（Ctrl+Shift+F）"));
    for (auto* w : {c.receiveHexCheck, c.timestampCheck, c.autoScrollCheck, c.filterCheck})
        w->hide();
    c.counterLabel->hide();
    c.resetCounterButton->hide();
    installSearchShortcuts();
    finishSetup();
    // SerialSession records original bytes before this callback, independently of display loss.
    const auto engine = terminalEngine_;
    c.session.setDataCallback(
        [engine](std::uint64_t generation, const testtools::ByteBuffer& bytes) {
            engine->receive(generation, bytes);
        });
    applyAppearance();
    renderTerminal();
    focusTerminal();
    if (first)
        savePreferences();
}
SerialTerminalWorkspace::~SerialTerminalWorkspace()
{
    prepareForShutdown();
    channels_[0].session.setDataCallback({});
    terminalEngine_.reset();
}
bool SerialTerminalWorkspace::selectWorkspaceMode(const QString& mode)
{
    if (mode.trimmed().toLower() != QStringLiteral("terminal"))
        return false;
    resumeTerminalInput();
    focusTerminal();
    return true;
}
QWidget* SerialTerminalWorkspace::createReceiveView(int, QWidget* parent)
{
    terminalGrid_ = new TerminalGridWidget(parent);
    return terminalGrid_;
}
void SerialTerminalWorkspace::setupReceiveInput(int)
{
    terminalGrid_->keyInput = [this](QKeyEvent* e) { handleTerminalKey(e); };
    terminalGrid_->textInput = [this](const QString& t) {
        if (channels_[0].session.isOpen()) {
            resumeTerminalInput();
            noteTerminalInput();
            terminalEngine_->text(channels_[0].session.generation(), t.toStdU32String());
        }
    };
    terminalGrid_->pasteRequested = [this] { pasteClipboard(); };
    terminalGrid_->followChanged = [this](bool follow) {
        channels_[0].autoScrollCheck->setChecked(follow);
    };
    terminalGrid_->gridResized = [this](int rows, int cols) {
        terminalEngine_->resize(rows, cols);
        if (dimensionsLabel_)
            dimensionsLabel_->setText(QStringLiteral("%1 × %2").arg(cols).arg(rows));
    };
    terminalGrid_->mouseInput = [this](int r, int c, int b, bool pressed, Qt::KeyboardModifiers m) {
        if (channels_[0].session.isOpen())
            terminalEngine_->mouse(
                channels_[0].session.generation(), r, c, b, pressed, modifiers(m));
    };
}
void SerialTerminalWorkspace::streamReset(int)
{
    if (terminalEngine_)
        terminalEngine_->resetStream();
}
void SerialTerminalWorkspace::clearReceive(int)
{
    terminalGrid_->clearSelection();
    terminalEngine_->clear();
}
bool SerialTerminalWorkspace::historyTruncated(int) const
{
    return terminalEngine_->snapshot()->trimmedLines != 0;
}
void SerialTerminalWorkspace::renderReceiveLog(int)
{
    if (terminalGrid_)
        terminalGrid_->setTimestampVisible(channels_[0].timestampCheck->isChecked());
    renderTerminal();
    if (terminalGrid_ && channels_[0].searchEdit->text().isEmpty())
        updateSearchMatches(0);
}
void SerialTerminalWorkspace::applyLogAppendResult(int, const testtools::SerialLogAppendResult&) {}
void SerialTerminalWorkspace::appendReceived(int, const testtools::ByteBuffer& b)
{
    terminalEngine_->receive(channels_[0].session.generation(), b);
}
void SerialTerminalWorkspace::focusTerminal()
{
    if (terminalGrid_)
        terminalGrid_->setFocus(Qt::OtherFocusReason);
}
QString SerialTerminalWorkspace::receiveText(int) const
{
    return terminalGrid_->toPlainText();
}
void SerialTerminalWorkspace::followOutput(int)
{
    if (terminalGrid_)
        terminalGrid_->followOutput();
}
void SerialTerminalWorkspace::setHighlightEnabled(int, bool e)
{
    if (terminalGrid_)
        terminalGrid_->setKeywordHighlight(e);
}
void SerialTerminalWorkspace::noteTerminalInput()
{
    inputEchoParsedBytes_ = terminalEngine_->snapshot()->parsedBytes;
    inputEchoClock_.start();
    awaitingInputEcho_ = true;
}
void SerialTerminalWorkspace::renderTerminal()
{
    if (!terminalGrid_ || !terminalEngine_)
        return;
    frameNotice_->store(false);
    const auto frame = terminalEngine_->snapshot();
    terminalGrid_->setFrame(frame);
    if (awaitingInputEcho_ && frame->parsedBytes > inputEchoParsedBytes_) {
        awaitingInputEcho_ = false;
        // Paint the first received update after an explicit input without another
        // deferred viewport cycle. Continuous output keeps normal dirty-region merging.
        if (inputEchoClock_.elapsed() <= 150)
            terminalGrid_->viewport()->repaint();
    }
    channels_[0].displayDroppedBytes = frame->droppedBytes;
    if (frame->droppedBytes > announcedDroppedBytes_) {
        announcedDroppedBytes_ = frame->droppedBytes;
        emit activityChanged(QStringLiteral("终端显示积压，已跳过 %1 字节；独立录制请查看录制状态")
                                 .arg(frame->droppedBytes));
    }
    if (frame->rejectedInputs > announcedRejectedInputs_) {
        announcedRejectedInputs_ = frame->rejectedInputs;
        emit activityChanged(
            QStringLiteral("终端输入队列已满，%1 次输入未发送").arg(frame->rejectedInputs));
    }
    updateCounter(0);
    updateTruncation(0);
    if (!channels_[0].searchEdit->text().isEmpty())
        scheduleSearchRefresh(0);
}
void SerialTerminalWorkspace::updateSearchMatches(int)
{
    const int n = terminalGrid_->setSearch(channels_[0].searchEdit->text());
    channels_[0].currentMatch = terminalGrid_->currentMatch();
    channels_[0].matchCountLabel->setText(QStringLiteral("%1 处").arg(n));
    channels_[0].previousMatchButton->setEnabled(n > 0);
    channels_[0].nextMatchButton->setEnabled(n > 0);
}
void SerialTerminalWorkspace::findLogMatch(int, int step)
{
    const int n = terminalGrid_->setSearch(channels_[0].searchEdit->text(), true, step);
    channels_[0].currentMatch = terminalGrid_->currentMatch();
    channels_[0].matchCountLabel->setText(
        QStringLiteral("%1 / %2").arg(terminalGrid_->currentMatch() + 1).arg(n));
}
void SerialTerminalWorkspace::resumeTerminalInput()
{
    channels_[0].searchButton->setChecked(false);
    terminalGrid_->clearSelection();
    terminalGrid_->followOutput();
}
void SerialTerminalWorkspace::handleTerminalKey(QKeyEvent* e)
{
    const int key = e->key();
    if (key == Qt::Key_Control || key == Qt::Key_Shift || key == Qt::Key_Alt ||
        key == Qt::Key_Meta || key == Qt::Key_AltGr)
        return;
    const auto mods = e->modifiers();
    const bool ctrl = mods.testFlag(Qt::ControlModifier), shift = mods.testFlag(Qt::ShiftModifier);
    if (ctrl && key == Qt::Key_C &&
        (shift || (preferences_.terminalSmartKeys && terminalGrid_->hasSelection()))) {
        terminalGrid_->copy(!shift);
        return;
    }
    if ((ctrl && key == Qt::Key_V && (shift || preferences_.terminalSmartKeys)) ||
        (shift && key == Qt::Key_Insert)) {
        pasteClipboard();
        return;
    }
    if (!channels_[0].session.isOpen())
        return;
    const auto g = channels_[0].session.generation();
    resumeTerminalInput();
    noteTerminalInput();
    if (ctrl && key >= Qt::Key_A && key <= Qt::Key_Z) {
        terminalEngine_->rawInput(g, {std::uint8_t(key - Qt::Key_A + 1)});
        return;
    }
    if (key == Qt::Key_Enter || key == Qt::Key_Return) {
        terminalEngine_->rawInput(
            g,
            testtools::terminalKeyBytes(testtools::TerminalKey::Enter,
                                        lineEndingFromIndex(preferences_.terminalLineEnding)));
        return;
    }
    if (key == Qt::Key_Backspace) {
        terminalEngine_->rawInput(g, {std::uint8_t(preferences_.terminalBackspace)});
        return;
    }
    using K = testtools::TerminalInputKey;
    const std::pair<int, K> keys[] = {{Qt::Key_Tab, K::Tab},
                                      {Qt::Key_Backtab, K::Tab},
                                      {Qt::Key_Escape, K::Escape},
                                      {Qt::Key_Up, K::Up},
                                      {Qt::Key_Down, K::Down},
                                      {Qt::Key_Left, K::Left},
                                      {Qt::Key_Right, K::Right},
                                      {Qt::Key_Insert, K::Insert},
                                      {Qt::Key_Delete, K::Delete},
                                      {Qt::Key_Home, K::Home},
                                      {Qt::Key_End, K::End},
                                      {Qt::Key_PageUp, K::PageUp},
                                      {Qt::Key_PageDown, K::PageDown}};
    for (const auto& k : keys)
        if (key == k.first) {
            terminalEngine_->key(g, k.second, modifiers(mods) | (key == Qt::Key_Backtab ? 1 : 0));
            return;
        }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F12) {
        terminalEngine_->key(g, K(int(K::F1) + key - Qt::Key_F1), modifiers(mods));
        return;
    }
    if (!e->text().isEmpty())
        terminalEngine_->text(
            g, e->text().toStdU32String(), mods.testFlag(Qt::AltModifier) ? 2 : 0);
}
void SerialTerminalWorkspace::pasteClipboard()
{
    auto& session = channels_[0].session;
    if (!session.isOpen())
        return;
    const auto g = session.generation();
    QString text = QApplication::clipboard()->text();
    if (text.isEmpty())
        return;
    if ((text.contains('\n') || text.contains('\r')) && pasteApprovedGeneration_ != g) {
        QString preview = text;
        preview.replace("\r\n", "\n");
        preview.replace('\r', '\n');
        const auto lines = preview.split('\n');
        QMessageBox box(QMessageBox::Question,
                        QStringLiteral("确认多行粘贴"),
                        QStringLiteral("将向当前串口粘贴 %1 行。换行可能执行命令。\n\n%2")
                            .arg(lines.size())
                            .arg(lines.mid(0, 5).join('\n').left(500)),
                        QMessageBox::Ok | QMessageBox::Cancel,
                        this);
        box.setDefaultButton(QMessageBox::Cancel);
        auto* check = new QCheckBox(QStringLiteral("本次连接不再提示"), &box);
        box.setCheckBox(check);
        if (box.exec() != QMessageBox::Ok)
            return;
        if (!session.isOpen() || session.generation() != g)
            return;
        if (check->isChecked())
            pasteApprovedGeneration_ = g;
    }
    QString clean;
    for (const auto ch : text)
        if (ch.unicode() >= 32 || ch == '\n' || ch == '\r' || ch == '\t')
            clean += ch;
    const auto b = testtools::terminalPasteBytes(
        toUtf8(clean), lineEndingFromIndex(preferences_.terminalLineEnding));
    resumeTerminalInput();
    noteTerminalInput();
    terminalEngine_->paste(g, std::string(b.begin(), b.end()));
}
void SerialTerminalWorkspace::applyAppearance()
{
    terminalGrid_->setAppearance(preferences_.terminalTheme,
                                 fromUtf8(preferences_.terminalFont),
                                 preferences_.terminalFontSize,
                                 preferences_.terminalLineSpacing,
                                 preferences_.terminalCursorShape);
    terminalGrid_->copyOnSelect = preferences_.terminalCopyOnSelect;
    terminalGrid_->rightPaste = preferences_.terminalRightPaste;
    terminalGrid_->setKeywordHighlight(channels_[0].highlightButton->isChecked());
    terminalGrid_->setTimestampVisible(channels_[0].timestampCheck->isChecked());
}
void SerialTerminalWorkspace::editAppearance()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("终端外观"));
    auto* form = new QFormLayout(&dialog);
    auto* theme = new QComboBox(&dialog);
    theme->addItems(
        {QStringLiteral("柔和深色"), QStringLiteral("暖白浅色"), QStringLiteral("高对比")});
    theme->setCurrentIndex(preferences_.terminalTheme);
    form->addRow(QStringLiteral("配色"), theme);
    auto* font = new QFontComboBox(&dialog);
    font->setFontFilters(QFontComboBox::MonospacedFonts);
    font->setCurrentFont(terminalGrid_->font());
    form->addRow(QStringLiteral("字体"), font);
    auto* size = new QSpinBox(&dialog);
    size->setRange(11, 18);
    size->setValue(preferences_.terminalFontSize);
    size->setSuffix(" px");
    form->addRow(QStringLiteral("字号"), size);
    auto* spacing = new QSpinBox(&dialog);
    spacing->setRange(0, 6);
    spacing->setValue(preferences_.terminalLineSpacing);
    form->addRow(QStringLiteral("额外行距"), spacing);
    auto* cursor = new QComboBox(&dialog);
    cursor->addItems({QStringLiteral("跟随设备"),
                      QStringLiteral("块状"),
                      QStringLiteral("下划线"),
                      QStringLiteral("竖线")});
    cursor->setCurrentIndex(preferences_.terminalCursorShape);
    form->addRow(QStringLiteral("光标"), cursor);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    preferences_.terminalTheme = theme->currentIndex();
    preferences_.terminalFont = toUtf8(font->currentFont().family());
    preferences_.terminalFontSize = size->value();
    preferences_.terminalLineSpacing = spacing->value();
    preferences_.terminalCursorShape = cursor->currentIndex();
    applyAppearance();
    savePreferences();
}
void SerialTerminalWorkspace::buildReceiveOptions(QHBoxLayout* header)
{
    channels_[0].timestampCheck->setText(QStringLiteral("接收时间"));
    channels_[0].timestampCheck->setToolTip(
        QStringLiteral("只为收到文本的行显示时间；空白、清屏和窗口变化不会生成时间"));
    dimensionsLabel_ = new QLabel(this);
    dimensionsLabel_->setToolTip(QStringLiteral("列 × 行；窗口缩放不会自动通知串口设备"));
    header->addWidget(dimensionsLabel_);
    terminalOptionsButton_ = new QPushButton(QStringLiteral("终端选项…"), this);
    terminalOptionsButton_->setObjectName(QStringLiteral("clearButton"));
    auto* menu = new QMenu(terminalOptionsButton_);
    menu->addAction(QStringLiteral("终端外观…"), this, [this] { editAppearance(); });
    menu->addAction(QStringLiteral("复制窗口尺寸命令"), this, [this] {
        QApplication::clipboard()->setText(QStringLiteral("stty rows %1 cols %2")
                                               .arg(terminalGrid_->gridRows())
                                               .arg(terminalGrid_->gridColumns()));
        emit activityChanged(QStringLiteral("已复制窗口尺寸命令；由你决定何时粘贴执行"));
    });
    auto* endings = menu->addMenu(QStringLiteral("回车格式"));
    auto* group = new QActionGroup(endings);
    const std::array<QString, 3> names{
        QStringLiteral("CR"), QStringLiteral("LF"), QStringLiteral("CRLF")};
    const std::array<int, 3> values{2, 1, 3};
    for (std::size_t i = 0; i < 3; ++i) {
        auto* a = endings->addAction(names[i]);
        a->setCheckable(true);
        a->setData(values[i]);
        a->setChecked(values[i] == preferences_.terminalLineEnding);
        group->addAction(a);
        terminalEndingActions_[i] = a;
        connect(a, &QAction::triggered, this, [this, value = values[i]] {
            preferences_.terminalLineEnding = value;
            savePreferences();
        });
    }
    auto* backspace = menu->addMenu(QStringLiteral("退格键"));
    auto* keys = new QActionGroup(backspace);
    for (std::size_t i = 0; i < 2; ++i) {
        const int value = i == 0 ? 127 : 8;
        auto* a =
            backspace->addAction(i == 0 ? QStringLiteral("DEL (7F)") : QStringLiteral("BS (08)"));
        a->setCheckable(true);
        a->setData(value);
        a->setChecked(value == preferences_.terminalBackspace);
        keys->addAction(a);
        terminalBackspaceActions_[i] = a;
        connect(a, &QAction::triggered, this, [this, value] {
            preferences_.terminalBackspace = value;
            savePreferences();
        });
    }
    menu->addSeparator();
    const auto option = [this, menu](const QString& label, bool* value) {
        auto* a = menu->addAction(label);
        a->setCheckable(true);
        a->setChecked(*value);
        connect(a, &QAction::toggled, this, [this, value](bool checked) {
            *value = checked;
            applyAppearance();
            savePreferences();
        });
    };
    option(QStringLiteral("智能 Ctrl+C / Ctrl+V"), &preferences_.terminalSmartKeys);
    option(QStringLiteral("选中即复制"), &preferences_.terminalCopyOnSelect);
    option(QStringLiteral("无选区时右键粘贴"), &preferences_.terminalRightPaste);
    for (auto* check : {channels_[0].timestampCheck, channels_[0].autoScrollCheck}) {
        auto* a = menu->addAction(check->text());
        a->setCheckable(true);
        a->setChecked(check->isChecked());
        connect(a, &QAction::toggled, check, &QCheckBox::setChecked);
        connect(check, &QCheckBox::toggled, a, &QAction::setChecked);
    }
    menu->addAction(QStringLiteral("发送中断 (Ctrl+C)"), this, [this] {
        if (channels_[0].session.isOpen())
            terminalEngine_->rawInput(channels_[0].session.generation(), {3});
    });
    menu->addAction(QStringLiteral("计数清零"), this, [this] { resetChannelCounters(0); });
    menu->addAction(QStringLiteral("键盘说明…"), this, [this] {
        QMessageBox::information(
            this,
            QStringLiteral("串口终端键盘"),
            QStringLiteral(
                "智能模式：Ctrl+C 有选区复制，无选区中断；Ctrl+V 粘贴。\n关闭智能模式后，Ctrl+C / "
                "Ctrl+V 始终发送 03 / 16。\nCtrl+Shift+C / V 始终复制 / 粘贴；Ctrl+Shift+F / F3 "
                "搜索。\nShift+PageUp / Down 回看；Alt+拖动矩形选择；鼠标报告开启时按住 Shift "
                "选择。\n串口没有窗口尺寸通知，请按需复制并执行 stty 命令。"));
    });
    connect(menu, &QMenu::aboutToHide, this, [this] {
        QTimer::singleShot(0, this, &SerialTerminalWorkspace::focusTerminal);
    });
    terminalOptionsButton_->setMenu(menu);
    header->addWidget(terminalOptionsButton_);
}
