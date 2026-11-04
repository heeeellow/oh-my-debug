#include "application/agent/agent_connection_service.h"
#include "application/agent/agent_service_status.h"
#include "core/protocol/agent_protocol.h"
#include "test_paths.h"
#include "ui/main_window.h"
#include "ui/widgets/file_pane.h"
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>
#include <QtWidgets>
#define private public
#define protected public
#include "ui/pages/file_transfer_workspace.h"
#include "ui/pages/serial_terminal_workspace.h"
#include "ui/pages/serial_workspace.h"
#undef protected
#undef private
#include <asio.hpp>
#include <iostream>
#include <thread>

void check(bool value, const char* message)
{
    if (!value)
        throw std::runtime_error(message);
}
bool await(const std::function<bool()>& predicate, int timeout = 6000)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeout) {
        if (predicate())
            return true;
        QTest::qWait(10);
    }
    return predicate();
}
class LoopbackService {
    asio::io_context context_;
    asio::ip::tcp::acceptor acceptor_{context_, {asio::ip::address_v4::loopback(), 0}};
    std::thread worker_;

  public:
    const unsigned short port = acceptor_.local_endpoint().port();
    std::atomic<bool> done{false};
    LoopbackService()
        : worker_([this] {
              try {
                  asio::ip::tcp::socket socket(context_);
                  acceptor_.accept(socket);
                  for (int i = 0; i < 2; ++i) {
                      std::array<unsigned char, 4> header{};
                      asio::read(socket, asio::buffer(header));
                      const unsigned length = (unsigned(header[0]) << 24) |
                                              (unsigned(header[1]) << 16) |
                                              (unsigned(header[2]) << 8) | header[3];
                      if (!length || length > 1048576)
                          break;
                      std::vector<unsigned char> input(length);
                      asio::read(socket, asio::buffer(input));
                      const std::string response =
                          i == 0
                              ? R"({"protocol":1,"request_id":"health-hello","ok":true,"result":{"agent_version":"0.3.7"}})"
                              : R"({"protocol":1,"request_id":"health-capabilities","ok":true,"result":{"capabilities":[{"id":"serial.stream"}]}})";
                      const auto frame =
                          testtools::encodeAgentFrame(testtools::jsonAgentFrame(response));
                      asio::write(socket, asio::buffer(frame));
                  }
              } catch (const std::exception&) {
              }
              done = true;
          })
    {
    }
    ~LoopbackService()
    {
        if (!done) {
            asio::ip::tcp::socket wake(context_);
            asio::error_code ec;
            wake.connect({asio::ip::address_v4::loopback(), port}, ec);
            wake.close(ec);
        }
        if (worker_.joinable())
            worker_.join();
    }
};
class HeldClient final : public testtools::IAgentConnection {
  public:
    bool connect(const std::string&, std::uint16_t, std::string&) override
    {
        return true;
    }
    void close() override
    {
        closed = true;
    }
    bool request(const std::string&, std::string&, std::string&) override
    {
        return false;
    }
    std::atomic<bool> closed{false};
};
class MainWindowUiChecks {
  public:
    static int run(int argc, char** argv);
};
int MainWindowUiChecks::run(int argc, char** argv)
{
    try {
        QApplication app(argc, argv);
        app.setStyle("Fusion");
        app.setFont(QFont("Microsoft YaHei UI", 10));
        qputenv("TESTTOOLS_OFFLINE", "1");
        QTemporaryDir preferences;
        check(preferences.isValid(), "isolated service UI preferences");
        qputenv("TESTTOOLS_DATA_DIR", preferences.path().toUtf8());
        // Local files retain asynchronous Windows associations; device categories use embedded SVG.
        FileListModel model(nullptr, false);
        model.replace({{"log.txt", testtools::FileKind::Regular},
                       {"bundle.zip", testtools::FileKind::Regular},
                       {"photo.png", testtools::FileKind::Regular},
                       {"audio.wav", testtools::FileKind::Regular},
                       {"video.mp4", testtools::FileKind::Regular},
                       {"tool.sh", testtools::FileKind::Regular},
                       {"lib.so", testtools::FileKind::Regular},
                       {"client.apk", testtools::FileKind::Regular}});
        FileListModel remoteModel(nullptr, true);
        std::vector<testtools::FileEntry> remoteEntries;
        for (int row = 0; row < model.rowCount(); ++row)
            remoteEntries.push_back(model.entry(row));
        remoteModel.replace(std::move(remoteEntries));
        QSignalSpy remoteChanges(&remoteModel, &QAbstractItemModel::dataChanged);
        const auto remoteKey =
            qvariant_cast<QIcon>(remoteModel.data(remoteModel.index(0, 0), Qt::DecorationRole))
                .cacheKey();
        QSignalSpy changes(&model, &QAbstractItemModel::dataChanged);
        const auto placeholder =
            qvariant_cast<QIcon>(model.data(model.index(1, 0), Qt::DecorationRole)).cacheKey();
        for (int i = 0; i < model.rowCount(); ++i)
            check(!qvariant_cast<QIcon>(model.data(model.index(i, 0), Qt::DecorationRole)).isNull(),
                  "nonempty fallback icons");
        check(await([&] { return changes.count() > 0; }), "background system icons delivered");
        check(qvariant_cast<QIcon>(model.data(model.index(1, 0), Qt::DecorationRole)).cacheKey() !=
                  placeholder,
              "native archive icon replaces fallback");
        check(remoteChanges.count() == 0 &&
                  remoteKey == qvariant_cast<QIcon>(
                                   remoteModel.data(remoteModel.index(0, 0), Qt::DecorationRole))
                                   .cacheKey(),
              "device SVG categories stay stable without background Shell replacement");
        QTreeView types;
        types.setModel(&remoteModel);
        types.setRootIsDecorated(false);
        types.resize(750, 360);
        types.show();
        QTest::qWait(300);
        types.grab().save(testOutputPath("remote-file-types.png"));
        types.hide();

        MainWindow window;
        window.resize(1366, 768);
        window.show();
        window.setSerialModeForDiagnostics("terminal");
        auto* serial = window.terminalWorkspace_;
        {
            testtools::DeviceScanResult empty;
            window.applyDeviceScan(empty, true);
            testtools::DeviceScanResult initial;
            initial.devices.push_back({"initial-device", "device", "model:fixture"});
            window.applyDeviceScan(initial, true);
            check(window.selectedDevice().serial == "initial-device" &&
                      window.adbStatusLabel_->text().contains(QStringLiteral("已连接")),
                  "first sole online device is selected after an initially empty scan");
            window.applyDeviceScan(empty, true);
            initial.devices[0].serial = "replacement-device";
            window.applyDeviceScan(initial, true);
            check(window.selectedDevice().serial.empty() &&
                      window.adbStatusLabel_->text().contains(QStringLiteral("待选择")),
                  "a previously selected device disappearing never silently binds another");
            window.applyDeviceScan(initial, false);
            check(window.selectedDevice().serial == "replacement-device",
                  "manual refresh resolves unchanged list with an unselected placeholder");
        }
        {
            QSignalBlocker block(window.deviceCombo_);
            window.deviceCombo_->clear();
            window.deviceCombo_->addItem("loopback fixture", "device-A");
            window.deviceCombo_->setItemData(0, "device", Qt::UserRole + 1);
        }
        window.agentConnecting_ = true;
        window.deviceCombo_->currentIndexChanged(0);
        window.agentConnecting_ = false;
        serial->channels_[0].sourceCombo->setCurrentIndex(0);
        check(await([&] { return !serial->localScanning_; }), "initial PC enumeration complete");
        serial->enumerateLocalPorts_ = [] {
            return std::vector<serialdemo::PortInfo>{{"COM_UI_FIXTURE", "fixture", ""}};
        };
        serial->channels_[0].refreshPortsButton->click();
        check(await([&] { return !serial->localScanning_; }) &&
                  !serial->channels_[0].feedbackPanel->isVisible(),
              "valid listed PC target has no unrelated warning row");
        QTest::qWait(30);
        const int logTop = serial->channels_[0].receiveWidget->mapTo(serial, QPoint()).y();
        serial->channels_[0].sourceCombo->setCurrentIndex(1);
        serial->channels_[0].portCombo->setEditText("/dev/ttyTEST");
        QTest::qWait(30);
        check(serial->channels_[0].receiveWidget->mapTo(serial, QPoint()).y() == logTop,
              "inline service hint preserves terminal vertical position");
        const int refreshLeft = window.refreshButton_->mapTo(&window, QPoint()).x();
        const int comboRight =
            window.deviceCombo_->mapTo(&window, QPoint()).x() + window.deviceCombo_->width();
        check(refreshLeft >= comboRight && refreshLeft - comboRight <= 8,
              "refresh action is immediately next to device selector");
        check(serial->channels_[0].serviceHint->isVisible() &&
                  !serial->channels_[0].connectButton->isEnabled(),
              "missing board service explained next to connection");
        qunsetenv("TESTTOOLS_OFFLINE");
        window.agentConnecting_ = true;
        window.passiveAgentProbe_ = true;
        window.updateServicePresentation();
        check(window.topServiceButton_->isEnabled() &&
                  window.topServiceButton_->property("needsConnection").toBool() &&
                  window.topServiceButton_->text() == QStringLiteral("连接服务"),
              "background existing-service probe leaves prominent connect action usable");
        window.agentConnecting_ = false;
        window.passiveAgentProbe_ = false;
        qputenv("TESTTOOLS_OFFLINE", "1");
        testtools::DeviceScanResult scan;
        scan.devices.push_back({"device-A", "device", ""});
        window.applyDeviceScan(scan, true);
        QSignalSpy resets(window.deviceCombo_->model(), &QAbstractItemModel::modelReset);
        const auto generation = window.agentGeneration_;
        window.applyDeviceScan(scan, true);
        check(resets.count() == 0 && window.selectedDevice().serial == "device-A" &&
                  window.agentGeneration_ == generation,
              "unchanged scan keeps model, selection and service generation stable");
        auto failedScan = scan;
        failedScan.error = "injected discovery failure";
        window.applyDeviceScan(failedScan, true);
        check(resets.count() == 0 && window.selectedDevice().serial == "device-A",
              "failed discovery preserves current device and model");
        window.applyDeviceScan(scan, true);
        check(resets.count() == 0 &&
                  window.adbStatusLabel_->text().contains(QStringLiteral("已连接")),
              "discovery recovery restores status without rebuilding list");
        scan.devices.push_back({"device-B", "device", ""});
        window.applyDeviceScan(scan, true);
        check(window.selectedDevice().serial == "device-A" && window.agentGeneration_ == generation,
              "another device arriving neither switches selection nor reprobes current service");
        qunsetenv("TESTTOOLS_OFFLINE");
        window.scanning_ = true;
        window.automaticScan_ = true;
        window.serialBusy_ = true; // Local COM does not freeze ADB discovery controls.
        window.updateDeviceControls();
        check(window.deviceCombo_->isEnabled() && window.refreshButton_->isEnabled(),
              "silent scan and local COM do not flash global device controls disabled");
        window.scanning_ = false;
        window.serialBusy_ = false;
        qputenv("TESTTOOLS_OFFLINE", "1");
        window.navigateTo("adb");
        check(window.pages_->currentIndex() == MainWindow::MirrorPage,
              "ADB group first opens remote debugging above file transfer");
        window.navigateTo("terminal");
        window.grab().save(testOutputPath("service-disconnected.png"));
        auto held = std::make_shared<HeldClient>();
        window.agentClient_ = held;
        window.agentAdbSerial_ = "device-A";
        unsigned short expiredPort = 0;
        {
            LoopbackService server;
            expiredPort = server.port;
            window.agentForwardPort_ = server.port;
            window.serviceState_ = testtools::AgentServiceState::Ready;
            serial->agentAdbSerial_ = "device-A";
            serial->agentControlPort_ = server.port;
            serial->setWorkspaceModeForDiagnostics("terminal");
            serial->busyChanged(false); // Publish the injected ready fixture to both pages.
            check(!serial->channels_[0].serviceHint->isVisible() &&
                      window.topServiceStatus_->text().contains(QStringLiteral("已就绪")) &&
                      window.topServiceButton_->text() == QStringLiteral("服务详情"),
                  "service readiness visible separately from ADB");
            qunsetenv("TESTTOOLS_OFFLINE");
            QMetaObject::invokeMethod(window.serviceHealthTimer_, "timeout", Qt::DirectConnection);
            qputenv("TESTTOOLS_OFFLINE", "1");
            check(await([&] { return !window.serviceProbeActive_ && server.done; }),
                  "real framed health exchange succeeds");
            check(window.agentClient_ != nullptr, "healthy service retained");
        }
        window.agentForwardPort_ = expiredPort;
        qunsetenv("TESTTOOLS_OFFLINE");
        QMetaObject::invokeMethod(window.serviceHealthTimer_, "timeout", Qt::DirectConnection);
        qputenv("TESTTOOLS_OFFLINE", "1");
        check(await([&] { return !window.serviceProbeActive_; }) && window.agentClient_ &&
                  window.serviceHealthFailures_ == 1,
              "one failed health probe does not flicker ready service to disconnected");
        qunsetenv("TESTTOOLS_OFFLINE");
        QMetaObject::invokeMethod(window.serviceHealthTimer_, "timeout", Qt::DirectConnection);
        qputenv("TESTTOOLS_OFFLINE", "1");
        check(await([&] {
                  return !window.serviceProbeActive_ && !window.agentClient_ &&
                         !window.agentCleanupPending_;
              }),
              "lost service invalidated and cleanup asynchronous");
        check(held->closed && serial->agentControlPort_ == 0 &&
                  serial->channels_[0].serviceHint->isVisible() &&
                  !serial->channels_[0].connectButton->isEnabled(),
              "stale control port cannot reopen a board serial");
        check(window.topServiceStatus_->text().contains(QStringLiteral("连接失败")) &&
                  window.topServiceButton_->text() == QStringLiteral("重试连接"),
              "loss reflected in top bar");
        window.grab().save(testOutputPath("service-failed.png"));
        serial->channels_[0].sourceCombo->setCurrentIndex(0);
        check(!serial->channels_[0].serviceHint->isVisible(),
              "local COM independent of service failure");
        window.close();
        check(await([&] { return !window.isVisible(); }), "window closes after health operations");
        std::cout << "Native icons and real loopback service checks passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
int main(int argc, char** argv)
{
    return MainWindowUiChecks::run(argc, argv);
}
