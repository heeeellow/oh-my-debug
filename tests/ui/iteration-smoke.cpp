#include "application/device/file_transfer_service.h"
#include "application/serial/serial_port_claims.h"
#include "application/serial/serial_session.h"
#include "application/serial/serial_terminal.h"
#include "test_paths.h"
#include "ui/widgets/file_pane.h"
#include "ui/widgets/terminal_grid_widget.h"
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>
#include <QtWidgets>
#include <atomic>
#include <iostream>
#include <mutex>
#define private public
#include "ui/main_window.h"
#include "ui/pages/file_transfer_workspace.h"
#include "ui/pages/serial_terminal_workspace.h"
#include "ui/pages/serial_workspace.h"
#undef private

namespace {
void check(bool value, const char* message)
{
    if (!value)
        throw std::runtime_error(message);
}
bool await(const std::function<bool()>& predicate, int timeout = 6000)
{
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < timeout) {
        if (predicate())
            return true;
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
    void close() override
    {
        opened = false;
    }
    bool write(const serialdemo::ByteBuffer&) override
    {
        return opened;
    }
    bool isOpen() const override
    {
        return opened;
    }
    void receive(const std::string& value)
    {
        publishData({value.begin(), value.end()});
    }
};
class Files final : public testtools::IFileTransferBackend {
  public:
    testtools::FileTransferResult change(const testtools::FileChange&,
                                         const std::atomic<bool>&) override
    {
        return {testtools::TransferOutcome::Failed, "fixture change unsupported"};
    }

    testtools::FileListing
    list(const std::string&, const std::string& directory, const std::atomic<bool>&) override
    {
        return {directory, {}, {}};
    }
    testtools::FileTransferResult transfer(testtools::TransferDirection,
                                           const std::string&,
                                           const std::filesystem::path&,
                                           const std::string&,
                                           testtools::FileKind,
                                           bool,
                                           const std::atomic<bool>&,
                                           const testtools::FileProgress&) override
    {
        return {testtools::TransferOutcome::Completed, "fixture"};
    }
};
} // namespace

class MainWindowUiChecks {
  public:
    static int run(int argc, char** argv);
};

int MainWindowUiChecks::run(int argc, char** argv)
{
    try {
        qputenv("TESTTOOLS_OFFLINE", "1");
        QApplication app(argc, argv);
        app.setStyle("Fusion");
        QTemporaryDir preferences;
        check(preferences.isValid(), "isolated preferences");
        qputenv("TESTTOOLS_DATA_DIR", preferences.path().toUtf8());
        const QString evidence = testOutputPath();
        QDir().mkpath(evidence);
        QTimer::singleShot(45000, [] {
            std::cerr << "iteration fixture timed out\n" << std::flush;
            std::exit(3);
        });
        testtools::SerialWorkspacePreferences previous;
        previous.channels[1].endpoint = "COM_TEST";
        previous.channels[1].baudRate = 115200;
        std::string error;
        check(testtools::SerialPreferencesStore::save(
                  std::filesystem::path(preferences.path().toStdWString()) /
                      L"serial-preferences.json",
                  previous,
                  error),
              "write previous preferences");
        MainWindow window;
        window.resize(1366, 768);
        window.show();
        QApplication::setActiveWindow(&window);
        auto* normal = window.serialWorkspace_;
        auto* terminal = window.terminalWorkspace_;
        auto& channel = terminal->channels_[0];
        auto* edit = terminal->terminalGrid_;
        check(terminal->channels_.size() == 1 && !channel.manualSendArea && !channel.sendEdit &&
                  !channel.autoSendButton,
              "terminal constructs one endpoint and no ordinary-send controls");
        check(!normal->scheduleTimer_->isActive(), "debug scheduler idle at startup");
        check(static_cast<SerialEndpointWorkspace*>(normal) != terminal &&
                  terminal->currentWorkspaceMode() == "terminal",
              "separate terminal page");
        check(terminal->preferences_.channels[1].endpoint == previous.channels[1].endpoint &&
                  !channel.endpointManuallyEdited,
              "migrate previous B endpoint without pretending it was manually typed");
        check(window.setSerialModeForDiagnostics("terminal"), "show terminal page");
        QTest::qWait(100);
        check(window.focusWidget() == edit, "terminal receives keyboard focus");
        channel.timestampCheck->setChecked(false);
        {
            check(await([&] { return !terminal->localScanning_ && !normal->localScanning_; }),
                  "initial background enumerations settle");
            const auto originalEnumerator = terminal->enumerateLocalPorts_;
            channel.sourceCombo->setCurrentIndex(0);
            channel.endpointManuallyEdited = false;
            terminal->preferences_.channels[1].manualEndpoint = false;
            terminal->preferences_.channels[1].endpoint = "COM3";
            terminal->localPorts_ = {{"COM1", "built-in", ""}, {"COM3", "old", ""}};
            channel.portCombo->setEditText("COM3");
            terminal->applyPorts(0);
            terminal->updateGlobalControls();
            terminal->enumerateLocalPorts_ = [] {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                return std::vector<serialdemo::PortInfo>{{"COM1", "built-in", ""}, {"COM5", "new", ""}};
            };
            int heartbeats = 0;
            QTimer heartbeat;
            heartbeat.setInterval(10);
            QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++heartbeats; });
            heartbeat.start();
            channel.refreshPortsButton->click();
            check(terminal->localScanning_ && !channel.refreshPortsButton->isEnabled(),
                  "refresh shows pending state without waiting for enumeration");
            check(await([&] { return !terminal->localScanning_; }) && heartbeats >= 5,
                  "GUI keeps processing events during slow PC enumeration");
            heartbeat.stop();
            check(channel.portCombo->currentData().toString() == "COM5" && !channel.session.isActive() &&
                      terminal->modeDescriptionLabel_->toolTip().contains("COM5") &&
                      !channel.endpointManuallyEdited,
                  "a sole new port replaces vanished selection without opening it");
            window.grab().save(evidence + "/port-refresh-new-selection.png");
            terminal->enumerateLocalPorts_ = [] {
                return std::vector<serialdemo::PortInfo>{{"COM1", "", ""}, {"COM6", "", ""}, {"COM7", "", ""}};
            };
            channel.refreshPortsButton->click();
            check(await([&] { return !terminal->localScanning_; }) &&
                      channel.portCombo->currentText().isEmpty() &&
                      !channel.connectButton->isEnabled() && channel.feedbackPanel->isVisible() &&
                      channel.feedbackLabel->text().contains("COM5"),
                  "multiple new ports clear the stale target and explain recovery locally");
            window.grab().save(evidence + "/port-refresh-multiple-candidates.png");
            terminal->setChannelStatus(0, QStringLiteral("端口被占用，请先关闭其他终端"), true);
            emit terminal->activityChanged("unrelated background message");
            terminal->showPageNotice("unrelated refresh result");
            check(channel.feedbackLabel->text().contains(QStringLiteral("端口被占用")),
                  "a background activity cannot erase persistent connection failure feedback");
            const int selected = channel.portCombo->findData("COM6");
            channel.portCombo->setCurrentIndex(selected);
            QMetaObject::invokeMethod(channel.portCombo, "activated", Qt::DirectConnection, Q_ARG(int, selected));
            check(channel.connectionFeedback.isEmpty() && !channel.endpointManuallyEdited,
                  "choosing a new listed target clears the old connection error");
            terminal->enumerateLocalPorts_ = [] {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                return std::vector<serialdemo::PortInfo>{{"COM8", "new", ""}};
            };
            channel.refreshPortsButton->click();
            channel.portCombo->lineEdit()->clear();
            QTest::keyClicks(channel.portCombo->lineEdit(), "COM88");
            channel.portCombo->lineEdit()->setCursorPosition(3);
            check(await([&] { return !terminal->localScanning_; }) &&
                      channel.portCombo->currentText() == "COM88" && channel.endpointManuallyEdited &&
                      channel.portCombo->lineEdit()->cursorPosition() == 3,
                  "late enumeration preserves a newly typed manual endpoint and caret");
            terminal->enumerateLocalPorts_ = []() -> std::vector<serialdemo::PortInfo> {
                throw std::runtime_error("fixture enumeration failure");
            };
            channel.refreshPortsButton->click();
            check(await([&] { return !terminal->localScanning_; }) &&
                      terminal->localPorts_.front().name == "COM8" &&
                      channel.portCombo->currentText() == "COM88" &&
                      channel.feedbackLabel->text().contains("fixture enumeration failure"),
                  "enumeration error retains the list/draft and exposes a retry reason");
            window.grab().save(evidence + "/port-refresh-failure.png");
            terminal->enumerateLocalPorts_ = [] {
                return std::vector<serialdemo::PortInfo>{{"COM9", "new", ""}};
            };
            channel.feedbackRetry->click();
            check(await([&] { return !terminal->localScanning_; }) &&
                      channel.portCombo->currentText() == "COM88",
                  "local retry keeps explicit manual target instead of replacing it");
            terminal->flushPreferences();
            check(await([&] { return !terminal->hasPendingPreferences(); }),
                  "manual endpoint provenance saved in background");
            testtools::SerialWorkspacePreferences persisted;
            check(testtools::SerialPreferencesStore::load(terminal->preferencesStorePath_, persisted, error) &&
                      persisted.channels[1].manualEndpoint && persisted.channels[1].endpoint == "COM88",
                  "manual endpoint intent survives preference roundtrip");
            terminal->enumerateLocalPorts_ = originalEnumerator;
            terminal->refreshLocalPorts();
            check(await([&] { return !terminal->localScanning_; }), "restore actual read-only PC list");
            channel.endpointManuallyEdited = false;
            channel.portFeedback.clear();
            channel.portCombo->setEditText("COM_TEST");
            terminal->updateChannelFeedback(0);
        }
        channel.autoScrollCheck->setChecked(true);
        normal->channels_[1].timestampCheck->setChecked(false);
        check(terminal->preferencesStorePath_ != normal->preferencesStorePath_,
              "separate preference files");
        const auto refreshPc = [&](SerialEndpointWorkspace* workspace) {
            check(await([&] { return !workspace->localScanning_; }), "initial PC enumeration done");
            auto& pc = workspace->channels_[workspace->primaryChannelIndex()];
            pc.sourceCombo->setCurrentIndex(1);
            workspace->scanning_ = true; // An unrelated board enumeration is still pending.
            pc.sourceCombo->setCurrentIndex(0);
            const auto previous = pc.portCombo->currentText();
            pc.portCombo->addItem("TESTTOOLS_REFRESH_SENTINEL");
            pc.portCombo->lineEdit()->clear();
            QTest::keyClicks(pc.portCombo->lineEdit(), "COM_DRAFT");
            check(pc.sourceCombo->currentText() == QStringLiteral("PC端串口"),
                  "PC name is consistent in every serial mode");
            check(pc.connectButton->isEnabled() && pc.refreshPortsButton->isEnabled(),
                  "board enumeration does not disable idle PC connect or refresh");
            const auto generation = workspace->agentPortGeneration_;
            QSignalSpy activity(workspace, &SerialEndpointWorkspace::activityChanged);
            pc.refreshPortsButton->click();
            check(workspace->localScanning_ && !pc.refreshPortsButton->isEnabled(),
                  "PC enumeration runs in background with local progress");
            check(await([&] { return !workspace->localScanning_; }) &&
                      pc.portCombo->findText("TESTTOOLS_REFRESH_SENTINEL") < 0 &&
                      pc.portCombo->currentText() == "COM_DRAFT",
                  "PC refresh actually rebuilds ports and keeps typed draft");
            check(workspace->scanning_ && workspace->agentPortGeneration_ == generation,
                  "PC refresh neither starts nor cancels board enumeration");
            check(!activity.isEmpty() && workspace->modeDescriptionLabel_->property("notice").toBool() &&
                      workspace->modeDescriptionLabel_->toolTip().contains("COM_DRAFT"),
                  "PC refresh result is also visible near the page connection controls");
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
            testtools::SerialPortClaims::resourceKey(false, {}, config.endpoint),
            "串口终端",
            error);
        channel.session.open(std::move(transport), config);
        fixture->receive("root# ");
        check(await([&] { return edit->toPlainText() == "root#"; }), "terminal RX");
        window.setSerialModeForDiagnostics("single");
        check(channel.session.isOpen() && !normal->channels_[1].session.isActive(),
              "page switch keeps separate connection state");
        fixture->receive("terminal-only\r\n");
        check(await([&] { return edit->toPlainText().contains("terminal-only"); }),
              "hidden terminal receives");
        check(!normal->channels_[1].receiveEdit->toPlainText().contains("terminal-only"),
              "terminal RX never leaks to monitor");
        normal->channels_[1].portCombo->setEditText("COM_TEST");
        normal->channels_[1].connectButton->click();
        check(!normal->channels_[1].session.isActive() &&
                  normal->channels_[1].statusLabel->toolTip().contains(QStringLiteral("串口终端")),
              "same port rejected before opening OS handle");
        window.setSerialModeForDiagnostics("terminal");
        std::string initial;
        for (int i = 0; i < 11000; ++i)
            initial += "log-" + std::to_string(i) + "\r\n";
        fixture->receive(initial);
        check(await([&] { return edit->toPlainText().contains("log-10999"); }),
              "large terminal stream");
        check(terminal->terminalEngine_->snapshot()->history->size() <= 20000,
              "10000-line bounded history");
        qint64 maximum = 0;
        for (int i = 0; i < 30; ++i) {
            const auto tail = "eviction-" + std::to_string(i);
            QElapsedTimer clock;
            clock.start();
            fixture->receive(tail + "\r\n");
            check(await([&] { return edit->toPlainText().contains(QString::fromStdString(tail)); }),
                  "incremental terminal eviction");
            maximum = std::max(maximum, clock.elapsed());
            check(terminal->terminalEngine_->snapshot()->droppedBytes == 0,
                  "incremental rendering without dropped bytes");
        }
        std::cout << "FIXTURE 10000-line history; maximum tail delivery=" << maximum << "ms\n";
        channel.baudCombo->setCurrentText("230400");
        check(normal->channels_[1].baudCombo->currentText() == "115200", "independent parameters");
        window.grab().save(evidence + "/terminal-fixture-1366.png");
        channel.connectButton->click();
        check(await([&] { return !channel.disconnectPending && !channel.portClaim; }),
              "claim released after async close");
        check(
            bool(window.portClaims_->acquire(
                testtools::SerialPortClaims::resourceKey(false, {}, "COM_TEST"), "monitor", error)),
            "claim available for next page");

        auto* files = window.fileWorkspace_;
        files->service_ =
            std::make_shared<testtools::FileTransferService>(std::make_shared<Files>());
        files->setDevice({"fixture-A", "device", ""});
        const auto localA = preferences.path() + "/opened", localB = preferences.path() + "/draft";
        QDir().mkpath(localA);
        QDir().mkpath(localB);
        files->localPane_->navigate(localA);
        check(await([&] { return !files->localLoading_; }), "open local fixture directory");
        window.navigateTo("files");
        qunsetenv("TESTTOOLS_OFFLINE");
        check(await([&] {
                  return !files->entryRefreshPending_ && !files->localLoading_ &&
                         !files->remoteLoading_;
              }),
              "first file entry");
        files->localPath_->setText(QDir::toNativeSeparators(localB));
        const auto generation = files->localGeneration_;
        window.navigateTo("serial");
        window.navigateTo("files");
        check(await([&] {
                  return files->localGeneration_ > generation && !files->localLoading_ &&
                         !files->remoteLoading_;
              }),
              "file reentry refresh");
        check(QDir::fromNativeSeparators(QString::fromStdString(files->local_.path)) == localA &&
                  files->localPath_->text() == QDir::toNativeSeparators(localB),
              "reentry preserves local address draft without opening it");
        qputenv("TESTTOOLS_OFFLINE", "1");
        files->setDevice({});
        const auto offlineGeneration = files->localGeneration_;
        window.navigateTo("serial");
        window.navigateTo("files");
        check(await([&] {
                  return files->localGeneration_ > offlineGeneration && !files->localLoading_;
              }),
              "local refresh independent of ADB");
        check(files->remote_.path.empty() &&
                  files->localPath_->text() == QDir::toNativeSeparators(localB),
              "offline local refresh keeps draft and avoids remote I/O");
        window.close();
        check(await([&] { return !window.isVisible(); }), "fixture close");
        std::cout << "Independent pages, preference migration, port claims and local address draft "
                     "passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}

int main(int argc, char** argv)
{
    return MainWindowUiChecks::run(argc, argv);
}
