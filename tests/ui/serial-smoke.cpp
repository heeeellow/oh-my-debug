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
#include "ui/pages/serial_workspace.h"
#undef private
#include "ui/main_window.h"
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
        const bool live = argc > 2 && std::string(argv[2]) == "--com5";
        MainWindow window;
        window.resize(1366, 768);
        window.show();
        QApplication::setActiveWindow(&window);
        pump();
        auto* serial = window.findChild<SerialWorkspace*>();
        require(serial, "workspace");
        auto& channel = serial->channels_[1];
        auto* edit = channel.receiveEdit;
        if (!live)
            channel.autoScrollCheck->setChecked(false);
        require(serial->setWorkspaceModeForDiagnostics("terminal"), "terminal mode");
        pump();
        require(!channel.manualSendArea->isVisible() && serial->terminalOptionsButton_->isVisible(),
                "direct terminal without composer or shortcut strip");
        require(window.focusWidget() == edit, "enter mode focuses terminal");
        channel.sourceCombo->setCurrentIndex(0);
        channel.baudCombo->setCurrentText("115200");
        if (live) {
            channel.portCombo->setCurrentIndex(channel.portCombo->findData("COM5"));
            channel.portCombo->setEditText("COM5");
            channel.connectButton->click();
            pump(350);
            if (!channel.session.isOpen()) {
                window.grab().save(work + "/com5-open-error.png");
                std::cout << "LIVE_COM5: not opened; button_enabled="
                          << channel.connectButton->isEnabled()
                          << " status=" << channel.statusLabel->toolTip().toUtf8().constData()
                          << " no data sent\n";
                return 2;
            }
            require(window.focusWidget() == edit, "real connect automatically focuses terminal");
            QTest::keyClick(edit, Qt::Key_U, Qt::ControlModifier);
            QTest::keyClick(edit, Qt::Key_Return);
            pump(1200);
            if (!edit->toPlainText().contains('#')) {
                serial->terminalEndingActions_[2]->trigger();
                QTest::keyClick(edit, Qt::Key_U, Qt::ControlModifier);
                QTest::keyClick(edit, Qt::Key_Return);
                pump(1400);
            }
            const QString before = edit->toPlainText();
            const QRegularExpression prompt(QStringLiteral("(?:^|\\n)[^\\n]{0,100}[#$] *$"));
            const bool hasPrompt = prompt.match(before).hasMatch();
            bool printfPassed = false, interruptPassed = false;
            if (hasPrompt) {
                QTest::keyClicks(edit, "printf '__TT_COM5_STANDARD__\\n'");
                QTest::keyClick(edit, Qt::Key_Return);
                pump(500);
                printfPassed = QRegularExpression(QStringLiteral("^__TT_COM5_STANDARD__$"),
                                                  QRegularExpression::MultilineOption)
                                   .match(edit->toPlainText())
                                   .hasMatch();
                if (printfPassed) {
                    QTest::keyClicks(edit, "sleep 5");
                    QTest::keyClick(edit, Qt::Key_Return);
                    pump(150);
                    QTest::keyClick(edit, Qt::Key_C, Qt::ControlModifier);
                    pump(450);
                    interruptPassed = prompt.match(edit->toPlainText()).hasMatch();
                }
            }
            QByteArray raw;
            for (const auto& entry : channel.logBuffer.entries())
                if (entry.direction == testtools::SerialLogDirection::Rx)
                    raw.append(reinterpret_cast<const char*>(entry.bytes.data()),
                               static_cast<qsizetype>(entry.bytes.size()));
            QFile output(work + "/com5-product-rx.bin");
            output.open(QIODevice::WriteOnly);
            require(output.write(raw) == raw.size(), "save raw fixture log");
            output.close();
            const auto snapshot = channel.session.snapshot();
            window.grab().save(work + "/com5-product-1366.png");
            std::cout << "LIVE_COM5: open=1 baud=115200 adb=0 auto_focus=1 tx="
                      << snapshot.sentBytes << " rx=" << snapshot.receivedBytes
                      << " shell_prompt=" << hasPrompt << " printf=" << printfPassed
                      << " interrupt=" << interruptPassed << std::endl;
            channel.connectButton->click();
            pump(250);
            window.close();
            return 0;
        }
        auto state = std::make_shared<FixtureState>();
        auto fixture = std::make_unique<FixtureTransport>(state);
        auto* device = fixture.get();
        serialdemo::SerialConfig config;
        config.endpoint = "COM_TEST";
        channel.portCombo->setEditText("COM_TEST");
        channel.session.open(std::move(fixture), config);
        pump();
        require(window.focusWidget() == edit, "connect focuses terminal");
        device->receive("\x1b[32mBoot OK\x1b[0m\r\nroot# echo test");
        pump();
        const auto end = edit->textCursor().position();
        device->receive("\x1b[2D");
        pump();
        require(edit->textCursor().position() == end - 2, "cursor-only CSI moves visible cursor");
        device->receive("\b");
        pump();
        require(edit->textCursor().position() == end - 3,
                "cursor-only backspace moves visible cursor");
        QTest::keyClicks(edit, "abc");
        QTest::keyClick(edit, Qt::Key_Tab);
        QTest::keyClick(edit, Qt::Key_Up);
        QTest::keyClick(edit, Qt::Key_Backspace);
        QTest::keyClick(edit, Qt::Key_Return);
        pump();
        require(state->take() == "abc\t\x1b[A\x7f\r", "direct key bytes");
        auto selection = edit->document()->find("Boot");
        edit->setTextCursor(selection);
        QTest::keyClick(edit, Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
        pump();
        require(state->take().empty(), "copy never sends");
        QTest::keyClick(edit, Qt::Key_C, Qt::ControlModifier);
        pump();
        require(state->take() == std::string(1, 3), "Ctrl+C always interrupts even with selection");
        QTest::keyClick(edit, Qt::Key_F, Qt::ControlModifier);
        QTest::keyClick(edit, Qt::Key_V, Qt::ControlModifier);
        pump();
        require(state->take() == std::string("\x06\x16", 2) && !channel.searchArea->isVisible(),
                "readline Ctrl+F and Ctrl+V reach device");
        QApplication::clipboard()->setText("one\r\ntwo\n");
        QTest::keyClick(edit, Qt::Key_V, Qt::ControlModifier | Qt::ShiftModifier);
        pump();
        require(state->take() == "one\rtwo\r", "standard paste shortcut");
        serial->terminalBackspaceActions_[1]->trigger();
        QTest::keyClick(edit, Qt::Key_Backspace);
        pump();
        require(state->take() == std::string(1, 8), "BS option");
        serial->terminalBackspaceActions_[0]->trigger();
        serial->terminalEndingActions_[2]->trigger();
        QTest::keyClick(edit, Qt::Key_Return);
        pump();
        require(state->take() == "\r\n" && channel.lineEndingCombo->currentIndex() == 3,
                "terminal return option independent of normal sender");
        serial->terminalEndingActions_[0]->trigger();
        std::string lines = "\r\n";
        for (int i = 0; i < 90; ++i)
            lines += "trace " + std::to_string(i) + "\r\n";
        lines += "root@board:~# ";
        device->receive(lines);
        pump();
        const QPoint position(80, 80);
        QWheelEvent wheel(QPointF(position),
                          QPointF(edit->viewport()->mapToGlobal(position)),
                          QPoint(),
                          QPoint(0, 120),
                          Qt::NoButton,
                          Qt::NoModifier,
                          Qt::NoScrollPhase,
                          false);
        QApplication::sendEvent(edit->viewport(), &wheel);
        pump();
        require(!channel.autoScrollCheck->isChecked() &&
                    edit->verticalScrollBar()->value() < edit->verticalScrollBar()->maximum(),
                "wheel pauses output following");
        const int scrollback = edit->verticalScrollBar()->value();
        device->receive(" echoed\r\nroot@board:~# ");
        pump();
        require(edit->verticalScrollBar()->value() == scrollback,
                "incoming output preserves scrollback");
        QTest::keyClick(edit, Qt::Key_PageUp, Qt::ShiftModifier);
        pump();
        require(!channel.autoScrollCheck->isChecked() && state->take().empty(),
                "Shift+PageUp is local scrollback");
        QTest::keyClicks(edit, "x");
        pump();
        require(channel.autoScrollCheck->isChecked() && state->take() == "x",
                "typing returns to live prompt");
        selection = edit->document()->find("trace");
        edit->setTextCursor(selection);
        device->receive("x");
        pump();
        require(edit->textCursor().selectedText() == "trace",
                "incoming output preserves copy selection");
        QTest::keyClick(edit, Qt::Key_F, Qt::ControlModifier | Qt::ShiftModifier);
        pump();
        require(channel.searchArea->isVisible() && !channel.filterCheck->isVisible(),
                "standard search without row filtering");
        channel.searchEdit->setText("trace");
        pump();
        require(edit->toPlainText().contains("root@board"), "search keeps screen rows");
        QTest::keyClick(channel.searchEdit, Qt::Key_Escape);
        pump();
        require(window.focusWidget() == edit, "close search restores input focus");
        QTest::keyClick(edit, Qt::Key_L, Qt::ControlModifier);
        pump();
        require(state->take() == std::string(1, 12) && edit->toPlainText().contains("root@board"),
                "Ctrl+L waits for remote redraw");
        window.grab().save(work + "/terminal-direct-1366.png");
        window.resize(1920, 1080);
        pump();
        window.grab().save(work + "/terminal-direct-1920.png");
        window.resize(1366, 768);
        pump();
        channel.advancedSettingsButton->click();
        channel.searchButton->click();
        pump();
        window.grab().save(work + "/terminal-expanded-1366.png");
        channel.advancedSettingsButton->click();
        channel.searchButton->setChecked(false);
        serial->setWorkspaceModeForDiagnostics("single");
        pump();
        require(channel.manualSendArea->isVisible() && channel.atCommandArea->isVisible(),
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
        channel.atCommandCombo->setCurrentIndex(0);
        channel.sendAtCommandButton->click();
        pump(150);
        require(state->take() == "AT\r\n", "shortcut uses same line ending");
        serialdemo::PeriodicSendConfig plan;
        plan.interval = std::chrono::milliseconds(10);
        plan.targetCount = 3;
        require(channel.session.startScheduledSend({'P'}, plan), "scheduled send starts");
        pump(150);
        require(state->take() == "PPP" && !channel.session.snapshot().scheduledSend.running,
                "scheduled count and bytes preserved");
        window.grab().save(work + "/single-regression-1366.png");
        serial->setWorkspaceModeForDiagnostics("terminal");
        pump();
        const auto generation = channel.session.generation();
        require(channel.session.isOpen() && state->take().empty(),
                "mode switch preserves connection and sends nothing");
        device->unplug();
        pump(200);
        require(!channel.session.isOpen() && !channel.receiveEdit->toPlainText().isEmpty(),
                "unplug preserves output");
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
