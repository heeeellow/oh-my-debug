#include "application/device/file_transfer_service.h"
#include "application/serial/serial_port_claims.h"
#include "application/serial/serial_session.h"
#include "application/serial/serial_terminal.h"
#include "test_paths.h"
#include "ui/widgets/file_pane.h"
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>
#include <QtWidgets>
#include <atomic>
#include <iostream>
#include <mutex>
#define private public
#define protected public
#include "ui/main_window.h"
#include "ui/pages/file_transfer_workspace.h"
#include "ui/pages/serial_workspace.h"
#include "ui/pages/serial_terminal_workspace.h"
#undef protected
#undef private

namespace {
void check(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}
bool await(const std::function<bool()>& predicate, int timeout = 6000)
{
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < timeout) {
        if (predicate()) return true;
        QTest::qWait(5);
    }
    return predicate();
}
class FixtureTransport final : public serialdemo::ISerialTransport {
  public:
    std::atomic<bool> opened{false};
    void open(const serialdemo::SerialConfig&) override
    {
        opened = true;
        publishStatus(serialdemo::TransportState::Connected, "fixture connected");
    }
    void close() override { opened = false; }
    bool write(const serialdemo::ByteBuffer&) override { return opened; }
    bool isOpen() const override { return opened; }
    void receive(const std::string& value) { publishData({value.begin(), value.end()}); }
};
class Files final : public testtools::IFileTransferBackend {
  public:
    testtools::FileTransferResult change(const testtools::FileChange&, const std::atomic<bool>&) override
    { return {testtools::TransferOutcome::Failed, "fixture change unsupported"}; }

    testtools::FileListing list(const std::string&, const std::string& directory,
                                const std::atomic<bool>&) override
    { return {directory, {}, {}}; }
    testtools::FileTransferResult transfer(testtools::TransferDirection, const std::string&,
        const std::filesystem::path&, const std::string&, testtools::FileKind, bool,
        const std::atomic<bool>&, const testtools::FileProgress&) override
    { return {testtools::TransferOutcome::Completed, "fixture"}; }
};
int exactLines(const QString& text, const QString& value)
{
    return text.split('\n').count(value);
}
void saveRaw(SerialEndpointWorkspace* workspace, const QString& filename)
{
    QByteArray bytes;
    for (const auto& entry : workspace->channels_[workspace->primaryChannelIndex()].logBuffer.entries())
        if (entry.direction == testtools::SerialLogDirection::Rx)
            bytes.append(reinterpret_cast<const char*>(entry.bytes.data()), qsizetype(entry.bytes.size()));
    QFile output(filename);
    check(output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size(), "save retained RX bytes");
}
} // namespace

int main(int argc, char** argv)
{
    try {
        qputenv("TESTTOOLS_OFFLINE", "1");
        QApplication app(argc, argv);
        app.setStyle("Fusion");
        QTemporaryDir preferences;
        check(preferences.isValid(), "isolated preferences");
        qputenv("TESTTOOLS_DATA_DIR", preferences.path().toUtf8());
        const bool live = argc == 4 && std::string(argv[1]) == "--live";
        const QString evidence = testOutputPath();
        QDir().mkpath(evidence);
        QTimer::singleShot(live ? 90000 : 45000, [] {
            std::cerr << "iteration fixture timed out\n" << std::flush;
            std::exit(3);
        });
        testtools::SerialWorkspacePreferences previous;
        previous.channels[1].endpoint = live ? argv[2] : "COM_TEST";
        previous.channels[1].baudRate = 115200;
        std::string error;
        check(testtools::SerialPreferencesStore::save(
            std::filesystem::path(preferences.path().toStdWString()) / L"serial-preferences.json", previous, error),
            "write previous preferences");
        MainWindow window;
        window.resize(1366, 768);
        window.show();
        QApplication::setActiveWindow(&window);
        auto* normal = window.serialWorkspace_;
        auto* terminal = window.terminalWorkspace_;
        auto& channel = terminal->channels_[0];
        auto* edit = channel.receiveEdit;
        check(terminal->channels_.size() == 1 && !channel.manualSendArea && !channel.sendEdit && !channel.autoSendButton,
              "terminal constructs one endpoint and no ordinary-send controls");
        check(!normal->scheduleTimer_->isActive(), "debug scheduler idle at startup");
        check(static_cast<SerialEndpointWorkspace*>(normal) != terminal && terminal->currentWorkspaceMode() == "terminal", "separate terminal page");
        check(channel.portCombo->currentData().toString() == QString::fromStdString(previous.channels[1].endpoint) ||
              channel.portCombo->currentText() == QString::fromStdString(previous.channels[1].endpoint), "migrate previous B endpoint");
        check(window.setSerialModeForDiagnostics("terminal"), "show terminal page");
        QTest::qWait(100);
        check(window.focusWidget() == edit, "terminal receives keyboard focus");
        channel.timestampCheck->setChecked(false);
        channel.autoScrollCheck->setChecked(true);
        normal->channels_[1].timestampCheck->setChecked(false);
        check(terminal->preferencesStorePath_ != normal->preferencesStorePath_, "separate preference files");
        if (live) {
            channel.sourceCombo->setCurrentIndex(0);
            channel.portCombo->setEditText(QString::fromUtf8(argv[2]));
            channel.baudCombo->setCurrentText("115200");
            channel.connectButton->click();
            check(await([&] { return channel.session.isOpen(); }), "open real debug serial");
            check(await([&] { return window.focusWidget() == edit; }), "focus after real connection");
            const auto press = [&](Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
                QTest::keyClick(edit, key, modifiers);
                QTest::qWait(60);
            };
            const auto command = [&](const char* value) {
                QTest::keyClicks(edit, value);
                press(Qt::Key_Return);
            };
            press(Qt::Key_U, Qt::ControlModifier);
            press(Qt::Key_Return);
            check(await([&] { return edit->toPlainText().contains(QRegularExpression("[#$] *$")); }), "real shell prompt");
            press(Qt::Key_L);
            check(await([&] { return edit->toPlainText().contains(QRegularExpression("[#$] l *$")); }), "echo l before Enter");
            press(Qt::Key_S);
            check(await([&] { return edit->toPlainText().contains(QRegularExpression("[#$] ls *$")); }), "echo s before Enter");
            press(Qt::Key_U, Qt::ControlModifier);
            command("printf '__TT09_INPUT__\\n'");
            check(await([&] { return exactLines(edit->toPlainText(), "__TT09_INPUT__") == 1; }), "real command output");
            press(Qt::Key_Up);
            press(Qt::Key_Return);
            check(await([&] { return exactLines(edit->toPlainText(), "__TT09_INPUT__") == 2; }), "device command history");
            QTest::keyClicks(edit, "printf");
            press(Qt::Key_Tab);
            command("'__TT09_COMPLETION__\\n'");
            check(await([&] { return exactLines(edit->toPlainText(), "__TT09_COMPLETION__") == 1; }), "device Tab completion");
            command("sleep 5");
            QTest::qWait(150);
            QElapsedTimer interrupt;
            interrupt.start();
            press(Qt::Key_C, Qt::ControlModifier);
            check(await([&] { return edit->toPlainText().contains(QRegularExpression("[#$] *$")); }, 2000), "real Ctrl+C interrupt");
            std::cout << "REAL_SERIAL input/history/completion/interrupt passed; interrupt=" << interrupt.elapsed() << "ms\n" << std::flush;
            saveRaw(terminal, evidence + "/live-interaction-rx.bin");

            // echo is a shell builtin; launching a printf process per line can
            // measure process creation rather than serial/render throughput.
            QElapsedTimer floodClock;
            floodClock.start();
            command("i=0; while [ \"$i\" -lt 10500 ]; do echo TT9-$i; i=$((i+1)); done; echo __TT09_FLOOD_END__");
            const bool floodFinished = await([&] { return exactLines(edit->toPlainText(), "__TT09_FLOOD_END__") == 1; }, 40000);
            if (!floodFinished) {
                saveRaw(terminal, evidence + "/live-flood-timeout-retained-rx.bin");
                QFile rendered(evidence + "/live-flood-timeout-rendered.txt");
                if (rendered.open(QIODevice::WriteOnly)) rendered.write(edit->toPlainText().toUtf8());
                window.grab().save(evidence + "/live-flood-timeout.png");
                press(Qt::Key_C, Qt::ControlModifier);
            }
            check(floodFinished, "bounded real serial output flood");
            std::cout << "REAL_SERIAL 10500-line output received in " << floodClock.elapsed() << "ms\n" << std::flush;
            check(terminal->terminalBuffer_.truncated(), "real stream reached scrollback limit");
            command("printf '__TT09_AFTER_FLOOD__\\n'");
            check(await([&] { return exactLines(edit->toPlainText(), "__TT09_AFTER_FLOOD__") == 1; }), "input after real log eviction");
            window.grab().save(evidence + "/terminal-live-1366.png");
            window.resize(1920, 1080);
            QTest::qWait(100);
            window.grab().save(evidence + "/terminal-live-1920.png");
            window.resize(1366, 768);
            saveRaw(terminal, evidence + "/live-after-flood-retained-rx.bin");

            {
                QSignalBlocker blocker(window.deviceCombo_);
                window.deviceCombo_->clear();
                window.deviceCombo_->addItem(QString::fromUtf8(argv[3]), QString::fromUtf8(argv[3]));
                window.deviceCombo_->setItemData(0, "device", Qt::UserRole + 1);
            }
            window.deviceCombo_->currentIndexChanged(0); // Offline: set context without deployment.
            qunsetenv("TESTTOOLS_OFFLINE");
            terminal->busyChanged(true);
            check(window.topServiceButton_->isEnabled(), "local COM does not block service preparation");
            window.topServiceButton_->click();
            check(await([&] { return bool(window.agentClient_) && !window.agentConnecting_; }, 15000), "connect existing real Agent");
            check(channel.session.isOpen() && window.topServiceButton_->text() == QStringLiteral("服务详情"), "real Agent independent of local COM");
            check(await([&] { return !terminal->scanning_ && !terminal->agentPorts_.empty(); }, 10000), "device port enumeration available while local COM is open");
            window.topServiceButton_->click();
            window.grab().save(evidence + "/service-live-1366.png");
            window.disconnectAgentButton_->click();
            check(await([&] { return !window.agentCleanupPending_; }), "disconnect service cleanup");
            check(channel.session.isOpen(), "service disconnect preserves debug COM");
            window.topServiceButton_->click();
            check(await([&] { return bool(window.agentClient_) && !window.agentConnecting_; }, 15000), "service retry on real Agent");
            window.setSerialModeForDiagnostics("single");
            normal->channels_[1].portCombo->setEditText(QString::fromUtf8(argv[2]));
            check(await([&] { return normal->channels_[1].connectButton->isEnabled(); }), "local monitor available during Agent enumeration");
            normal->channels_[1].connectButton->click();
            check(!normal->channels_[1].session.isActive() &&
                  normal->channels_[1].statusLabel->toolTip().contains(QStringLiteral("串口终端")), "real same-port ownership message");
            check(channel.session.isOpen(), "switching pages retains terminal session");
            channel.connectButton->click();
            check(await([&] { return !channel.disconnectPending; }), "release real terminal port");
            normal->channels_[1].connectButton->click();
            check(await([&] { return normal->channels_[1].session.isOpen(); }), "monitor acquires released real port");
            normal->channels_[1].lineEndingCombo->setCurrentIndex(2);
            normal->channels_[1].sendEdit->setPlainText("printf '__TT09_MONITOR__\\n'");
            normal->channels_[1].sendButton->click();
            check(await([&] {
                QByteArray received;
                for (const auto& entry : normal->channels_[1].logBuffer.entries())
                    if (entry.direction == testtools::SerialLogDirection::Rx)
                        received.append(reinterpret_cast<const char*>(entry.bytes.data()), qsizetype(entry.bytes.size()));
                return received.contains("\n__TT09_MONITOR__\r\n");
            }), "monitor receives real command output after terminal release");
            normal->channels_[1].connectButton->click();
            check(await([&] { return !normal->channels_[1].disconnectPending; }), "close real monitor");
            // Clear real forwards through the ordinary online shutdown path.
            qunsetenv("TESTTOOLS_OFFLINE");
            window.close();
            check(await([&] { return !window.isVisible(); }), "real close");
            std::cout << "REAL_SERIAL flood/independent pages/ownership/service reconnect passed\n" << std::flush;
            return 0;
        }

        const auto refreshPc = [&](SerialEndpointWorkspace* workspace) {
            auto& pc = workspace->channels_[workspace->primaryChannelIndex()];
            pc.sourceCombo->setCurrentIndex(1);
            workspace->scanning_ = true; // An unrelated board enumeration is still pending.
            pc.sourceCombo->setCurrentIndex(0);
            const auto previous = pc.portCombo->currentText();
            pc.portCombo->addItem("TESTTOOLS_REFRESH_SENTINEL");
            pc.portCombo->setEditText("COM_DRAFT");
            check(pc.sourceCombo->currentText() == QStringLiteral("PC端串口"),
                  "PC name is consistent in every serial mode");
            check(pc.connectButton->isEnabled() && pc.refreshPortsButton->isEnabled(),
                  "board enumeration does not disable idle PC connect or refresh");
            const auto generation = workspace->agentPortGeneration_;
            QSignalSpy activity(workspace, &SerialEndpointWorkspace::activityChanged);
            pc.refreshPortsButton->click();
            check(pc.portCombo->findText("TESTTOOLS_REFRESH_SENTINEL") < 0 &&
                      pc.portCombo->currentText() == "COM_DRAFT",
                  "PC refresh actually rebuilds ports and keeps typed draft");
            check(workspace->scanning_ && workspace->agentPortGeneration_ == generation,
                  "PC refresh neither starts nor cancels board enumeration");
            check(!activity.isEmpty() && activity.last().first().toString().startsWith(
                                             QStringLiteral("已刷新 PC端串口")),
                  "PC refresh reports its own result without requiring a service");
            workspace->scanning_ = false;
            pc.portCombo->setEditText(previous);
        };
        window.setSerialModeForDiagnostics("single");
        for (const auto state : {testtools::AgentServiceState::Disconnected,
                                 testtools::AgentServiceState::Connecting,
                                 testtools::AgentServiceState::Failed}) {
            window.serviceState_ = state;
            normal->setServiceStatus(state, "fixture service unavailable");
            refreshPc(normal);
        }
        window.serviceState_ = testtools::AgentServiceState::Disconnected;
        normal->setServiceStatus(window.serviceState_, {});
        normal->setDeviceMaintenance(true);
        refreshPc(normal);
        normal->setDeviceMaintenance(false);
        normal->channels_[0].sourceCombo->setCurrentIndex(1);
        check(!normal->channels_[0].refreshPortsButton->isEnabled(),
              "device refresh still requires its board service");
        refreshPc(normal);
        normal->channels_[0].sourceCombo->setCurrentIndex(0);
        window.grab().save(evidence + "/pc-serial-refresh-no-service.png");
        window.setSerialModeForDiagnostics("dual");
        check(normal->channels_[0].sourceCombo->itemText(0) == QStringLiteral("PC端串口"),
              "dual endpoint A uses the PC name");
        auto other = std::make_unique<FixtureTransport>();
        serialdemo::SerialConfig otherConfig;
        otherConfig.endpoint = "COM_OTHER";
        normal->channels_[0].session.open(std::move(other), otherConfig);
        check(await([&] { return normal->channels_[0].session.isOpen(); }),
              "other PC endpoint active in fixture");
        refreshPc(normal);
        check(normal->channels_[0].session.isOpen() &&
                  !normal->channels_[0].refreshPortsButton->isEnabled(),
              "refreshing an idle PC endpoint preserves the other connection");
        normal->channels_[0].connectButton->click();
        check(await([&] {
                  return !normal->channels_[0].session.isActive() &&
                         !normal->channels_[0].disconnectPending;
              }),
              "other fixture endpoint closes");
        window.setSerialModeForDiagnostics("terminal");
        refreshPc(terminal);
        auto transport = std::make_unique<FixtureTransport>();
        auto* fixture = transport.get();
        serialdemo::SerialConfig config;
        config.endpoint = "COM_TEST";
        channel.portClaim = window.portClaims_->acquire(
            testtools::SerialPortClaims::resourceKey(false, {}, config.endpoint), "串口终端", error);
        channel.session.open(std::move(transport), config);
        fixture->receive("root# ");
        check(await([&] { return edit->toPlainText() == "root# "; }), "terminal RX");
        window.setSerialModeForDiagnostics("single");
        check(channel.session.isOpen() && !normal->channels_[1].session.isActive(), "page switch keeps separate connection state");
        fixture->receive("terminal-only\r\n");
        check(await([&] { return edit->toPlainText().contains("terminal-only"); }), "hidden terminal receives");
        check(!normal->channels_[1].receiveEdit->toPlainText().contains("terminal-only"), "terminal RX never leaks to monitor");
        normal->channels_[1].portCombo->setEditText("COM_TEST");
        normal->channels_[1].connectButton->click();
        check(!normal->channels_[1].session.isActive() &&
              normal->channels_[1].statusLabel->toolTip().contains(QStringLiteral("串口终端")), "same port rejected before opening OS handle");
        window.setSerialModeForDiagnostics("terminal");
        std::string initial;
        for (int i = 0; i < 11000; ++i) initial += "log-" + std::to_string(i) + "\r\n";
        fixture->receive(initial);
        check(await([&] { return edit->toPlainText().contains("log-10999") && !terminal->terminalBuffer_.dirty(); }), "large terminal stream");
        check(terminal->terminalBuffer_.lines().size() == 10000, "10000-line bounded history");
        qint64 maximum = 0;
        for (int i = 0; i < 30; ++i) {
            const auto tail = "eviction-" + std::to_string(i);
            QElapsedTimer clock;
            clock.start();
            fixture->receive(tail + "\r\n");
            check(await([&] { return edit->toPlainText().contains(QString::fromStdString(tail)); }), "incremental terminal eviction");
            maximum = std::max(maximum, clock.elapsed());
            check(edit->toPlainText().toStdString() == terminal->terminalBuffer_.plainText(), "rendered terminal equals buffer after eviction");
        }
        std::cout << "FIXTURE 10000-line history; maximum tail delivery=" << maximum << "ms\n";
        channel.baudCombo->setCurrentText("230400");
        check(normal->channels_[1].baudCombo->currentText() == "115200", "independent parameters");
        window.grab().save(evidence + "/terminal-fixture-1366.png");
        channel.connectButton->click();
        check(await([&] { return !channel.disconnectPending && !channel.portClaim; }), "claim released after async close");
        check(bool(window.portClaims_->acquire(testtools::SerialPortClaims::resourceKey(false, {}, "COM_TEST"), "monitor", error)), "claim available for next page");

        auto* files = window.fileWorkspace_;
        files->service_ = std::make_shared<testtools::FileTransferService>(std::make_shared<Files>());
        files->setDevice({"fixture-A", "device", ""});
        const auto localA = preferences.path() + "/opened", localB = preferences.path() + "/draft";
        QDir().mkpath(localA);
        QDir().mkpath(localB);
        files->localPane_->navigate(localA);
        check(await([&] { return !files->localLoading_; }), "open local fixture directory");
        window.navigateTo("files");
        qunsetenv("TESTTOOLS_OFFLINE");
        check(await([&] { return !files->entryRefreshPending_ && !files->localLoading_ && !files->remoteLoading_; }), "first file entry");
        files->localPath_->setText(QDir::toNativeSeparators(localB));
        const auto generation = files->localGeneration_;
        window.navigateTo("serial");
        window.navigateTo("files");
        check(await([&] { return files->localGeneration_ > generation && !files->localLoading_ && !files->remoteLoading_; }), "file reentry refresh");
        check(QDir::fromNativeSeparators(QString::fromStdString(files->local_.path)) == localA &&
              files->localPath_->text() == QDir::toNativeSeparators(localB), "reentry preserves local address draft without opening it");
        qputenv("TESTTOOLS_OFFLINE", "1");
        files->setDevice({});
        const auto offlineGeneration = files->localGeneration_;
        window.navigateTo("serial");
        window.navigateTo("files");
        check(await([&] { return files->localGeneration_ > offlineGeneration && !files->localLoading_; }), "local refresh independent of ADB");
        check(files->remote_.path.empty() && files->localPath_->text() == QDir::toNativeSeparators(localB), "offline local refresh keeps draft and avoids remote I/O");
        window.close();
        check(await([&] { return !window.isVisible(); }), "fixture close");
        std::cout << "Independent pages, preference migration, port claims and local address draft passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
