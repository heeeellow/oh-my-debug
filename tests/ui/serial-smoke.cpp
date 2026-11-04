#include "application/serial/at_command_library.h"
#include "application/serial/serial_session.h"
#include "application/serial/serial_terminal.h"
#include "application/serial/serial_workspace_state.h"
#include "application/support/background_task_group.h"
#include "core/serial/serial_test_engine.h"
#include <QtCore/QRegularExpression>
#include <QtGui/QWheelEvent>
#include <QtTest/QTest>
#include <QtWidgets>
#define private public
#define protected public
#include "ui/pages/serial_terminal_workspace.h"
#include "ui/pages/serial_workspace.h"
#undef protected
#undef private
#include "ui/main_window.h"
#include "ui/widgets/serial_shortcut_sidebar.h"
#include "ui/widgets/terminal_grid_widget.h"
#include <atomic>
#include <iostream>
#include <mutex>
#include <stdexcept>

struct FixtureState {
    std::mutex mutex;
    std::string writes;
    void record(const serialdemo::ByteBuffer& bytes)
    {
        std::lock_guard<std::mutex> lock(mutex);
        writes.append(bytes.begin(), bytes.end());
    }
    std::string take()
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto result = writes;
        writes.clear();
        return result;
    }
};
class FixtureTransport final : public serialdemo::ISerialTransport {
  public:
    explicit FixtureTransport(std::shared_ptr<FixtureState> value) : state(std::move(value)) {}
    void open(const serialdemo::SerialConfig&) override
    {
        opened = true;
        publishStatus(serialdemo::TransportState::Connected, "已连接（测试夹具）");
    }
    void close() override
    {
        opened = false;
    }
    bool write(const serialdemo::ByteBuffer& bytes) override
    {
        if (!opened)
            return false;
        state->record(bytes);
        return true;
    }
    bool isOpen() const override
    {
        return opened;
    }
    void receive(const std::string& text)
    {
        publishData({text.begin(), text.end()});
    }
    void unplug()
    {
        opened = false;
        publishStatus(serialdemo::TransportState::Error, "fixture disconnected");
    }
    std::shared_ptr<FixtureState> state;
    std::atomic<bool> opened{false};
};
struct ClipboardGuard {
    QMimeData* saved = new QMimeData;
    ClipboardGuard()
    {
        const auto* current = QApplication::clipboard()->mimeData();
        if (current)
            for (const auto& format : current->formats())
                saved->setData(format, current->data(format));
    }
    ~ClipboardGuard()
    {
        QApplication::clipboard()->setMimeData(saved);
    }
};
void require(bool value, const char* message)
{
    if (!value)
        throw std::runtime_error(message);
}
void pump(int ms = 100)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

int main(int argc, char** argv)
{
    qputenv("TESTTOOLS_OFFLINE", "1");
    QApplication app(argc, argv);
    QApplication::setStyle("Fusion");
    ClipboardGuard clipboard;
    QTimer::singleShot(45000, [] {
        std::cerr << "FAIL: fixture timeout\n";
        std::exit(3);
    });
    try {
        const QString work = QString::fromLocal8Bit(argv[1]);
        QTemporaryDir preferences;
        require(preferences.isValid(), "isolated serial preferences");
        qputenv("TESTTOOLS_DATA_DIR", preferences.path().toUtf8());
        testtools::SerialWorkspacePreferences initialPreferences;
        initialPreferences.channels[1].autoScroll = false;
        initialPreferences.channels[1].lineEnding = 3;
        std::string preferencesError;
        require(testtools::SerialPreferencesStore::save(
                    std::filesystem::path(preferences.path().toStdWString()) /
                        L"serial-preferences.json",
                    initialPreferences,
                    preferencesError),
                "seed isolated serial preferences");
        MainWindow window;
        window.resize(1366, 768);
        window.show();
        QApplication::setActiveWindow(&window);
        pump();
        // Key/render regression for the reusable workspace component. Separate
        // application page ownership is covered by iteration-smoke.
        {
            testtools::TerminalEngine selectionEngine;
            selectionEngine.resize(4, 8);
            const std::string text = "abcdefghijk\r\n12中文34\r\nend";
            selectionEngine.receive(1, {text.begin(), text.end()});
            selectionEngine.waitIdle();
            TerminalGridWidget sample;
            sample.setFrame(selectionEngine.snapshot());
            sample.selectRange(0, 0, 1, 3);
            require(sample.selectedText() == "abcdefghijk", "soft wrap copy rejoins logical line");
            sample.selectRange(0, 1, 1, 3, true);
            require(sample.selectedText() == "bc\njk", "rectangular copy keeps columns");
            sample.selectRange(2, 2, 2, 6);
            require(sample.selectedText() == QStringLiteral("中文"),
                    "wide-cell copy has no padding or duplicate glyphs");
        }
        auto* serial = window.findChild<SerialTerminalWorkspace*>("serialTerminalWorkspace");
        require(serial, "workspace");
        auto& channel = serial->channels_[0];
        auto* edit = serial->terminalGrid_;
        require(window.setSerialModeForDiagnostics("terminal"), "terminal page");
        pump();
        require(!channel.receiveEdit && !channel.manualSendArea && !channel.sendEdit &&
                    edit->isVisible(),
                "native grid without text document/composer");
        require(window.focusWidget() == edit, "terminal autofocus");
        require(channel.highlightButton->isChecked(), "automatic keyword highlight defaults on");
        channel.timestampCheck->setChecked(true);
        pump();
        for (const auto& line : edit->frame()->screen)
            require(line->timestampMs == 0, "empty disconnected view has no timestamps");
        window.grab().save(work + "/terminal-empty-timestamps.png");
        channel.clearReceiveButton->click();
        pump();
        for (const auto& line : edit->frame()->screen)
            require(line->timestampMs == 0, "local clear cannot invent timestamps");

        auto state = std::make_shared<FixtureState>();
        auto fixture = std::make_unique<FixtureTransport>(state);
        auto* device = fixture.get();
        serialdemo::SerialConfig config;
        config.endpoint = "COM_TEST";
        channel.portCombo->setEditText("COM_TEST");
        channel.session.open(std::move(fixture), config);
        pump();
        device->receive("\x1b[32mBoot OK\x1b[0m\r\nroot# echo test");
        pump();
        require(edit->frame()->screen[0]->timestampMs > 0 &&
                    edit->frame()->screen[2]->timestampMs == 0,
                "receive timestamps only on text rows");
        window.grab().save(work + "/terminal-received-timestamps.png");
        const int end = edit->frame()->cursorColumn;
        device->receive("\x1b[2D");
        pump();
        require(edit->frame()->cursorColumn == end - 2, "cursor-only CSI");
        QTest::keyClicks(edit, "abc");
        QTest::keyClick(edit, Qt::Key_Tab);
        QTest::keyClick(edit, Qt::Key_Up);
        QTest::keyClick(edit, Qt::Key_Backspace);
        QTest::keyClick(edit, Qt::Key_Return);
        pump();
        require(state->take() == "abc\t\x1b[A\x7f\r", "mode-aware direct keys");
        const int bootRow = int(edit->frame()->history->size());
        edit->selectRange(bootRow, 0, bootRow, 4);
        QTest::keyClick(edit, Qt::Key_C, Qt::ControlModifier);
        pump();
        require(state->take().empty() && QApplication::clipboard()->text() == "Boot" &&
                    !edit->hasSelection(),
                "smart Ctrl+C copies without interrupt");
        QTest::keyClick(edit, Qt::Key_C, Qt::ControlModifier);
        pump();
        require(state->take() == std::string(1, 3), "Ctrl+C interrupts without selection");
        QApplication::clipboard()->setText("paste");
        QTest::keyClick(edit, Qt::Key_V, Qt::ControlModifier);
        pump();
        require(state->take() == "paste", "smart Ctrl+V");
        QAction* smart = nullptr;
        for (auto* action : serial->terminalOptionsButton_->menu()->actions())
            if (action->text().startsWith(QStringLiteral("智能")))
                smart = action;
        require(smart, "traditional key option");
        smart->setChecked(false);
        QTest::keyClick(edit, Qt::Key_V, Qt::ControlModifier);
        pump();
        require(state->take() == std::string(1, 22), "traditional Ctrl+V");
        smart->setChecked(true);
        auto acceptPaste = [] {
            for (auto* w : QApplication::topLevelWidgets())
                if (auto* box = qobject_cast<QMessageBox*>(w))
                    box->done(QMessageBox::Ok);
        };
        QApplication::clipboard()->setText("one\r\ntwo\n");
        device->receive("\x1b[?2004h");
        pump();
        QTimer::singleShot(50, acceptPaste);
        QTest::keyClick(edit, Qt::Key_V, Qt::ControlModifier | Qt::ShiftModifier);
        pump();
        require(state->take() == "\x1b[200~one\rtwo\r\x1b[201~",
                "multiline preview and bracketed paste");
        serial->terminalBackspaceActions_[1]->trigger();
        QTest::keyClick(edit, Qt::Key_Backspace);
        pump();
        require(state->take() == std::string(1, 8), "BS preference");
        serial->terminalEndingActions_[2]->trigger();
        QTest::keyClick(edit, Qt::Key_Return);
        pump();
        require(state->take() == "\r\n", "CRLF preference");
        serial->terminalEndingActions_[0]->trigger();
        QInputMethodEvent preedit(QStringLiteral("zhong"), {});
        QApplication::sendEvent(edit, &preedit);
        pump();
        require(state->take().empty(), "IME preedit stays local");
        QInputMethodEvent commit;
        commit.setCommitString(QStringLiteral("中文"));
        QApplication::sendEvent(edit, &commit);
        pump();
        require(state->take() == QStringLiteral("中文").toUtf8().toStdString(), "IME commit UTF8");
        const auto bytesBefore = channel.session.snapshot().sentBytes;
        const auto resizeAction = serial->terminalOptionsButton_->menu()->actions()[1];
        resizeAction->trigger();
        require(QApplication::clipboard()->text().startsWith("stty rows "),
                "copy dimensions command");
        window.resize(1920, 1080);
        pump();
        require(channel.session.snapshot().sentBytes == bytesBefore, "resize never sends stty");
        window.grab().save(work + "/terminal-grid-1920.png");
        window.resize(1366, 768);
        pump();
        std::string lines = "\r\n";
        for (int i = 0; i < 90; ++i)
            lines += "trace " + std::to_string(i) + "\r\n";
        device->receive(lines);
        pump();
        edit->verticalScrollBar()->setValue(2);
        pump();
        const int top = edit->verticalScrollBar()->value();
        device->receive("tail\r\n");
        pump();
        require(edit->verticalScrollBar()->value() == top, "history anchor preserved");
        QTest::keyClicks(edit, "x");
        pump();
        require(state->take() == "x" && edit->following(), "input resumes prompt");
        QTest::keyClick(edit, Qt::Key_F, Qt::ControlModifier | Qt::ShiftModifier);
        pump();
        require(channel.searchArea->isVisible(), "search shortcut");
        channel.searchEdit->setText("trace");
        pump();
        QTest::keyClick(channel.searchEdit, Qt::Key_Return);
        pump();
        require(edit->currentMatch() >= 0, "search finds grid history");
        QTest::keyClick(channel.searchEdit, Qt::Key_Escape);
        pump();
        require(window.focusWidget() == edit, "search restores focus");
        device->receive("\x1b[?1049h\x1b[2J\x1b[Halternate");
        pump();
        require(edit->frame()->alternateScreen && edit->toPlainText().startsWith("alternate"),
                "alternate screen");
        device->receive(
            "\x1b[3;1H┌──────────────────────────┐\r\n│ 中文 / UTF-8   é         "
            "│\r\n└──────────────────────────┘\r\n\x1b[38;2;95;190;220mTrueColor\x1b[0m  "
            "\x1b[1mBold\x1b[0m  \x1b[3mItalic\x1b[0m  \x1b[4mUnderline\x1b[0m\r\n\x1b[7m Reverse "
            "\x1b[0m   █▉▊▋▌▍▎▏ ░▒▓ ▖▗▘▙▚▛▜▝▞▟");
        pump();
        const auto screen = edit->frame()->screen;
        int corner = -1, side = -1;
        for (int col = 0; col < int(screen[2]->cells.size()); ++col) {
            if (screen[2]->cells[col].characters[0] == 0x2510)
                corner = col;
            if (screen[3]->cells[col].characters[0] == 0x2502)
                side = col;
        }
        require(corner == side && corner > 0,
                "CJK/combining content stays aligned with box border");
        window.grab().save(work + "/terminal-grid-alternate.png");
        edit->setAppearance(1, "Cascadia Mono", 14, 2, 2);
        pump();
        window.grab().save(work + "/terminal-grid-light.png");
        edit->setAppearance(2, "Cascadia Mono", 14, 2, 3);
        pump();
        window.grab().save(work + "/terminal-grid-high-contrast.png");
        edit->setAppearance(0, "Cascadia Mono", 14, 2, 0);
        pump();
        device->receive("\x1b[?1049l");
        pump();
        require(!edit->frame()->alternateScreen && edit->toPlainText().contains("trace"),
                "restore history");
        window.grab().save(work + "/terminal-grid-1366.png");
        if (argc > 2 && std::string(argv[2]) == "--terminal-only") {
            device->unplug();
            pump(200);
            require(!channel.session.isOpen() && !edit->toPlainText().isEmpty(),
                    "terminal unplug preserves output");
            std::cout
                << "PASS terminal-only native grid/input/search/copy/paste/appearance/unplug; DPR="
                << window.devicePixelRatioF() << std::endl;
            window.close();
            return 0;
        }
        {
            auto* serial = window.findChild<SerialWorkspace*>("serialDebugWorkspace");
            auto& channel = serial->channels_[1];
            auto normalFixture = std::make_unique<FixtureTransport>(state);
            auto* device = normalFixture.get();
            channel.portCombo->setEditText("COM_NORMAL");
            auto normalConfig = config;
            normalConfig.endpoint = "COM_NORMAL";
            channel.session.open(std::move(normalFixture), normalConfig);
            require(window.setSerialModeForDiagnostics("single"), "normal page");
            pump();
            if (serial->shortcutSidebar_->isHidden())
                channel.toggleAtBarButton->click();
            pump();
            require(channel.manualSendArea->isVisible() && serial->shortcutSidebar_->isVisible(),
                    "ordinary sender and shortcuts preserved");
            require(!channel.autoScrollCheck->isChecked(),
                    "terminal scrollback does not overwrite normal follow preference");
            channel.lineEndingCombo->setCurrentIndex(3);
            channel.sendHexCheck->setChecked(false);
            channel.sendEdit->setPlainText("hello");
            channel.sendButton->click();
            pump(150);
            require(state->take() == "hello\r\n", "normal text and shared CRLF");
            channel.lineEndingCombo->setCurrentIndex(0);
            channel.sendHexCheck->setChecked(true);
            channel.sendEdit->setPlainText("00 FF 41");
            channel.sendButton->click();
            pump(150);
            require(state->take() == std::string("\0\xff"
                                                 "A",
                                                 3),
                    "HEX bytes unchanged");
            channel.lineEndingCombo->setCurrentIndex(3);
            channel.sendHexCheck->setChecked(false);
            auto* shortcuts = serial->shortcutSidebar_->findChild<QListWidget*>("shortcutList");
            shortcuts->setCurrentRow(1);
            pump();
            require(state->take().empty(), "selecting shortcut does not transmit");
            serial->shortcutSidebar_->findChild<QPushButton*>("shortcutFill-0")->click();
            require(channel.sendEdit->toPlainText() == "AT" && state->take().empty(),
                    "fill is local and preserves no-send behavior");
            serial->shortcutSidebar_->findChild<QPushButton*>("shortcutSend-0")->click();
            pump(150);
            require(state->take() == "AT\r\n", "shortcut uses same line ending");
            const auto originalCommands = serial->atCommandLibrary_.commands();
            std::string commandError;
            require(serial->atCommandLibrary_.add(
                        {"用于检查窄侧栏省略的较长指令名称", "00 FF 41", true}, commandError),
                    "HEX shortcut fixture accepted");
            serial->shortcutSidebar_->setCommands(serial->atCommandLibrary_.commands());
            const auto hexRow = QString::number(static_cast<int>(originalCommands.size()));
            serial->shortcutSidebar_->findChild<QPushButton*>("shortcutFill-" + hexRow)->click();
            require(channel.sendHexCheck->isChecked() &&
                        channel.sendEdit->toPlainText() == "00 FF 41" &&
                        channel.lineEndingCombo->currentIndex() == 3 && state->take().empty(),
                    "HEX fill changes input format without sending or changing shared ending");
            serial->shortcutSidebar_->findChild<QPushButton*>("shortcutSend-" + hexRow)->click();
            pump(150);
            require(state->take() == std::string("\0\xff"
                                                 "A",
                                                 3),
                    "HEX shortcut sends exact bytes without CRLF");
            window.grab().save(work + "/shortcut-long-name.png");
            require(serial->atCommandLibrary_.replace(originalCommands, commandError),
                    "restore shortcut fixtures");
            serial->shortcutSidebar_->setCommands(serial->atCommandLibrary_.commands());

            serial->shortcutSplitter_->setSizes({800, 300});
            serial->shortcutSplitter_->splitterMoved(800, 1);
            pump();
            const int rememberedWidth = serial->preferences_.shortcutSidebarWidth;
            window.resize(1120, 768);
            pump();
            require(serial->shortcutSidebar_->isHidden() &&
                        serial->preferences_.shortcutBarVisible &&
                        serial->preferences_.shortcutSidebarWidth == rememberedWidth,
                    "narrow layout folds without changing saved preference or width");
            window.grab().save(work + "/shortcut-narrow.png");
            window.resize(1920, 1080);
            pump();
            require(serial->shortcutSidebar_->isVisible(),
                    "wide layout restores preferred sidebar");
            const int composerY = channel.manualSendArea->mapTo(serial, QPoint()).y();
            serial->shortcutSidebar_->findChild<QToolButton*>("collapseShortcuts")->click();
            pump();
            require(serial->shortcutSidebar_->isHidden() &&
                        !serial->preferences_.shortcutBarVisible &&
                        channel.manualSendArea->mapTo(serial, QPoint()).y() == composerY,
                    "explicit collapse is remembered without moving the composer vertically");
            testtools::SerialWorkspacePreferences savedPreferences;
            serial->flushPreferences();
            QElapsedTimer preferenceWait;
            preferenceWait.start();
            while (serial->hasPendingPreferences() && preferenceWait.elapsed() < 3000)
                pump(5);
            require(!serial->hasPendingPreferences(), "asynchronous sidebar preferences drained");
            require(testtools::SerialPreferencesStore::load(
                        serial->preferencesStorePath_, savedPreferences, commandError) &&
                        !savedPreferences.shortcutBarVisible &&
                        savedPreferences.shortcutSidebarWidth == rememberedWidth,
                    "sidebar width and explicit collapse persist to isolated preferences");
            window.resize(1366, 768);
            pump();
            require(serial->shortcutSidebar_->isHidden(),
                    "resize preserves explicit collapsed preference");
            channel.toggleAtBarButton->click();
            pump();
            require(serial->shortcutSidebar_->isVisible(), "toolbar reopens sidebar");
            serialdemo::PeriodicSendConfig plan;
            plan.interval = std::chrono::milliseconds(10);
            plan.targetCount = 3;
            require(!serial->scheduleTimer_->isActive(), "idle send scheduler stopped");
            require(channel.session.startScheduledSend({'P'}, plan), "scheduled send starts");
            serial->setServiceStatus(testtools::AgentServiceState::Disconnected,
                                     QStringLiteral("scheduler fixture"));
            require(serial->scheduleTimer_->isActive(), "active plan starts scheduler");
            pump(150);
            require(state->take() == "PPP" && !channel.session.snapshot().scheduledSend.running,
                    "scheduled count and bytes preserved");
            require(!serial->scheduleTimer_->isActive(), "completed plan stops scheduler");
            require(serial->selectWorkspaceMode("dual"), "dual page for reliability check");
            auto& other = serial->channels_[0];
            other.sourceCombo->setCurrentIndex(0);
            other.portCombo->setEditText("COM_OTHER");
            other.baudCombo->setCurrentText("115200");
            auto otherState = std::make_shared<FixtureState>();
            auto otherFixture = std::make_unique<FixtureTransport>(otherState);
            auto otherConfig = config;
            otherConfig.endpoint = "COM_OTHER";
            other.session.open(std::move(otherFixture), otherConfig);
            pump(30);
            QTimer::singleShot(0, [] {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                require(dialog, "reliability parameters dialog");
                dialog->findChild<QComboBox*>()->setCurrentIndex(2);
                const auto spins = dialog->findChildren<QSpinBox*>();
                require(spins.size() == 3, "reliability parameter controls");
                spins[0]->setValue(2);
                spins[1]->setValue(8);
                spins[2]->setValue(10);
                dialog->accept();
            });
            serial->stressTestButton_->click();
            require(serial->linkVerifier_.stressRunning() && serial->scheduleTimer_->isActive(),
                    "reliability starts scheduler");
            pump(120);
            require(serial->linkVerifier_.awaitingReceive() && !serial->scheduleTimer_->isActive(),
                    "tail collection stops scheduler without stopping reliability task");
            serial->stressTestButton_->click();
            require(!serial->linkVerifier_.stressRunning() && !serial->scheduleTimer_->isActive(),
                    "reliability stop stays idle");
            state->take();
            other.connectButton->click();
            pump(100);
            require(serial->selectWorkspaceMode("single"), "single page after reliability check");
            const auto commandCount = serial->atCommandLibrary_.commands().size();
            bool managerEdited = false;
            QTimer::singleShot(0, [&] {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                require(dialog, "command manager dialog");
                const auto buttons = dialog->findChildren<QPushButton*>();
                QPushButton* create = nullptr;
                QPushButton* save = nullptr;
                for (auto* button : buttons) {
                    if (button->text() == QStringLiteral("新建"))
                        create = button;
                    if (button->text() == QStringLiteral("保存指令"))
                        save = button;
                }
                require(create && save, "command manager edit actions");
                create->click();
                dialog->findChild<QLineEdit*>()->setText("p0-manager");
                dialog->findChild<QPlainTextEdit*>()->setPlainText("AT+P0");
                save->click();
                dialog->grab().save(work + "/command-manager.png");
                managerEdited = true;
                dialog->reject();
            });
            QAction* manageAction = nullptr;
            for (auto* button : serial->shortcutSidebar_->findChildren<QToolButton*>())
                if (button->text() == QStringLiteral("指令库") && button->menu())
                    manageAction = button->menu()->actions().front();
            require(manageAction, "command manager entry");
            manageAction->trigger();
            testtools::AtCommandLibrary persistedCommands;
            require(
                managerEdited && serial->atCommandLibrary_.commands().size() == commandCount + 1 &&
                    testtools::AtCommandStore::load(
                        serial->atCommandStorePath_, persistedCommands, commandError) &&
                    persistedCommands.commands().back().command == "AT+P0" && state->take().empty(),
                "extracted command manager edits and persists without transmitting");
            window.grab().save(work + "/single-regression-1366.png");
            window.setSerialModeForDiagnostics("terminal");
            pump();
            const auto generation = channel.session.generation();
            require(channel.session.isOpen() && state->take().empty(),
                    "mode switch preserves connection and sends nothing");
            device->unplug();
            pump(200);
            require(!channel.session.isOpen() && !channel.receiveEdit->toPlainText().isEmpty(),
                    "unplug preserves output");
        }
        std::cout << "PASS: pure terminal view, autofocus, cursor-only echo redraw, exact bytes, "
                     "standard copy/paste/control shortcuts, return and backspace settings, "
                     "scrollback/input resume, search focus, normal-mode regression and unplug\n"
                  << std::flush;
        window.close();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << std::endl;
        return 1;
    }
}
