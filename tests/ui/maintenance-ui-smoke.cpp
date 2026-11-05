#include "application/device/device_maintenance_service.h"
#include "application/device/file_transfer_service.h"
#include "test_paths.h"
#include "ui/theme.h"
#include "ui/widgets/file_pane.h"
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtTest/QTest>
#include <QtWidgets>
#include <atomic>
#include <iostream>
#include <thread>
#define private public
#define protected public
#include "ui/main_window.h"
#include "ui/pages/file_transfer_workspace.h"
#include "ui/pages/serial_workspace.h"
#include "ui/pages/serial_terminal_workspace.h"
#include "ui/screen_mirror_panel.h"
#include "ui/widgets/device_maintenance_panel.h"
#undef protected
#undef private

using namespace testtools;
namespace {
void check(bool value, const char* message)
{
    if (!value)
        throw std::runtime_error(message);
}
bool await(const std::function<bool()>& predicate, int timeout = 6000)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeout) {
        if (predicate())
            return true;
        QTest::qWait(5);
    }
    return predicate();
}
class Backend final : public IDeviceMaintenanceBackend {
  public:
    std::atomic<int> calls{0};
    std::atomic<bool> hold{true};
    std::atomic<bool> probeHold{false}, probeEntered{false};
    std::atomic<LoaderSupportState> support{LoaderSupportState::Unknown};
    LoaderSupport inspectLoaderSupport(const AdbDevice&, const std::atomic<bool>& cancel) override
    {
        probeEntered = true;
        while (probeHold && !cancel) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return {support.load(), "fixture support reason", {}};
    }
    MaintenanceResult answer{MaintenanceOutcome::Completed, "fixture complete"};
    MaintenanceResult
    execute(const MaintenanceRequest& request,
            const std::atomic<bool>& cancel,
            const std::function<void(const MaintenanceProgress&)>& publish) override
    {
        ++calls;
        publish({disruptive(request.action) ? MaintenanceStage::WaitingRoot
                                            : MaintenanceStage::Exporting,
                 "fixture: waiting",
                 512,
                 request.output,
                 disruptive(request.action)});
        while (hold && !cancel)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (cancel)
            return {MaintenanceOutcome::Cancelled, "fixture cancelled"};
        return answer;
    }
};
void capture(QWidget* widget, const QString& name)
{
    if (!qEnvironmentVariableIsSet("TESTTOOLS_MAINTENANCE_SNAPSHOTS"))
        return;
    QTest::qWait(70);
    const auto pixmap = widget->grab();
    check(pixmap.save(testOutputPath(name + ".png")), "save remote page image");
    QFile file(testOutputPath(name + ".json"));
    check(file.open(QIODevice::WriteOnly), "save image metadata");
    file.write(QJsonDocument(QJsonObject{{"fixture", true},
                                         {"logical_width", widget->width()},
                                         {"logical_height", widget->height()},
                                         {"pixel_width", pixmap.width()},
                                         {"pixel_height", pixmap.height()},
                                         {"device_pixel_ratio", pixmap.devicePixelRatio()}})
                   .toJson());
}
} // namespace
int main(int argc, char** argv)
{
    try {
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        QApplication app(argc, argv);
        app.setStyle("Fusion");
        app.setFont(QFont("Microsoft YaHei UI", 10));
        app.setStyleSheet(testtools::ui::applicationStyleSheet());
        app.setQuitOnLastWindowClosed(false);
        qputenv("TESTTOOLS_OFFLINE", "1");
        auto backend = std::make_shared<Backend>();
        auto service = std::make_shared<DeviceMaintenanceService>(backend);
        MainWindow window(nullptr, service);
        window.resize(1366, 768);
        window.show();
        auto* panel = window.maintenancePanel_;
        check(!panel->root_->isEnabled() && !panel->logcat_->isEnabled() &&
                  !panel->loader_->isHidden() && !panel->loader_->isEnabled(),
              "offline actions disabled and Loader remains discoverable");
        const testtools::AdbDevice device{"device-A", "device", "maintenance fixture"};
        {
            QSignalBlocker block(window.deviceCombo_);
            window.deviceCombo_->clear();
            window.deviceCombo_->addItem("fixture A", "device-A");
            window.deviceCombo_->setItemData(0, "device", Qt::UserRole + 1);
        }
        window.deviceCombo_->currentIndexChanged(0);
        window.navigateTo("mirror");
        QTest::qWait(100);
        auto* columns = window.findChild<QWidget*>("remoteDebugColumns");
        check(columns, "responsive columns exist");
        check(columns->property("columnCount").toInt() == 2, "standard window uses two columns");
        if (argc > 1 && std::string(argv[1]) == "--high-dpi")
            capture(&window, "remote-highdpi");
        else {
            capture(&window, "remote-1366");
            window.resize(1920, 1080);
            QTest::qWait(60);
            capture(&window, "remote-1920");
            window.resize(1120, 768);
            QTest::qWait(100);
            check(columns->property("columnCount").toInt() == 1, "narrow window stacks columns");
            capture(&window, "remote-narrow");
            window.resize(1366, 768);
        }
        // 仅在调用夹具动作的同步边界移除 offline；恢复后禁止窗口做真实发现/连接。
        auto ready = [&] {
            qunsetenv("TESTTOOLS_OFFLINE");
            QTest::qWait(170);
        };
        auto start = [&](MaintenanceRequest request) {
            ready();
            QTimer::singleShot(150, [&, request] {
                for (auto* widget : QApplication::topLevelWidgets()) {
                    if (auto* dialog = qobject_cast<QMessageBox*>(widget)) {
                        for (auto* button : dialog->buttons())
                            if (dialog->buttonRole(button) == QMessageBox::AcceptRole) {
                                button->click();
                                break;
                            }
                    } else if (auto* dialog = qobject_cast<QFileDialog*>(widget)) {
                        auto* filename =
                            dialog->findChild<QLineEdit*>(QStringLiteral("fileNameEdit"));
                        check(filename, "save dialog filename control");
                        filename->setText(QString::fromStdWString(request.output.wstring()));
                        QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
                    }
                }
            });
            (request.action == MaintenanceAction::RootRemount ? panel->root_ : panel->dmesg_)
                ->click();
            qputenv("TESTTOOLS_OFFLINE", "1");
        };
        bool confirmChecked = false;
        ready();
        QTimer::singleShot(0, [&] {
            for (auto* widget : QApplication::topLevelWidgets())
                if (auto* dialog = qobject_cast<QMessageBox*>(widget)) {
                    check(dialog->text().contains("device-A") &&
                              dialog->defaultButton() == dialog->button(QMessageBox::Cancel),
                          "confirmation names fixed target and defaults to cancel");
                    confirmChecked = true;
                    if (argc == 1)
                        capture(dialog, "remote-confirmation");
                    dialog->reject();
                }
        });
        panel->reboot_->click();
        qputenv("TESTTOOLS_OFFLINE", "1");
        check(confirmChecked && backend->calls == 0,
              "cancel confirmation performs no device action");
        ready();
        QTimer::singleShot(0, [&] {
            for (auto* widget : QApplication::topLevelWidgets())
                if (auto* dialog = qobject_cast<QMessageBox*>(widget)) {
                    panel->setDevice({"device-B", "device", "fixture B"});
                    for (auto* button : dialog->buttons())
                        if (dialog->buttonRole(button) == QMessageBox::AcceptRole) {
                            button->click();
                            break;
                        }
                }
        });
        panel->root_->click();
        qputenv("TESTTOOLS_OFFLINE", "1");
        check(backend->calls == 0 &&
                  window.activityLabel_->text().contains(QStringLiteral("上下文已变化")),
              "context change during confirmation rejected");
        panel->setDevice(device);

        MaintenanceRequest request;
        request.device = device;
        request.action = MaintenanceAction::RootRemount;
        auto& local = window.serialWorkspace_->channels_[1];
        local.sourceCombo->setCurrentIndex(0);
        local.portCombo->lineEdit()->setFocus();
        local.portCombo->lineEdit()->selectAll();
        QTest::keyClicks(local.portCombo->lineEdit(), "COM_TEST");
        start(request);
        check(await([&] { return backend->calls == 1; }), "root fixture active");
        check(window.maintenanceBlocking_ && window.fileWorkspace_->maintenanceBlocked_ &&
                  window.serialWorkspace_->maintenanceBlocked_ && !window.deviceCombo_->isEnabled(),
              "maintenance interlocks established synchronously");
        check(local.connectButton->isEnabled(), "Windows local serial remains available");
        const auto before = window.fileWorkspace_->service_->snapshot().id;
        window.fileWorkspace_->localPane_->transferRequested();
        check(window.fileWorkspace_->service_->snapshot().id == before,
              "direct file start cannot bypass maintenance interlock");
        window.screenMirrorPanel_->startCurrentDevice();
        check(!window.screenMirrorPanel_->busy_, "mirror start blocked during maintenance");
        backend->answer = {MaintenanceOutcome::Partial,
                           "Root 已核验，Remount 未确认完成",
                           "fixture: permission denied"};
        backend->answer.rootVerified = true;
        backend->hold = false;
        check(await([&] { return !panel->wasActive_ && !window.maintenanceBlocking_; }),
              "root fixture settled and gates released");
        check(panel->status_->text().contains(QStringLiteral("Remount 未确认")),
              "partial maintenance result remains visible");
        if (argc == 1)
            capture(&window, "remote-partial-result");
        backend->hold = true;
        request.action = MaintenanceAction::Dmesg;
        request.output = testOutputDirectory() / "partial.log";
        const auto previousJob = service->snapshot().id;
        start(request);
        check(await([&] {
                  const auto state = service->snapshot();
                  return state.active && state.id != previousJob &&
                         state.request.action == MaintenanceAction::Dmesg && state.progress.bytes > 0;
              }), "new export progress observed");
        QMetaObject::invokeMethod(window.taskMenu_, "aboutToShow", Qt::DirectConnection);
        const auto actions = window.taskMenu_->actions();
        check(!actions.empty(), "active export has a global stop action");
        auto* oldStop = actions.back();
        // Retain this action explicitly: QMenu::clear otherwise deletes an owned action,
        // making a later stale-token check a use-after-free in the fixture itself.
        oldStop->setParent(&window);
        const auto oldId = service->snapshot().id;
        check(window.taskButton_->isVisible() && !window.maintenanceBlocking_,
              "export appears globally without device maintenance lock");
        panel->cancelTask(oldId);
        check(await([&] { return !panel->wasActive_; }), "export cancelled");
        start(request);
        check(await([&] { return service->snapshot().active && service->snapshot().id != oldId; }),
              "new export prepared");
        oldStop->trigger();
        check(!service->snapshot().cancelling, "old global stop cannot affect new export");
        window.close();
        check(await([&] { return !window.isVisible(); }),
              "window closes after cancellation and owned task drainage");
        check(!service->snapshot().active && window.maintenanceShutdownComplete_,
              "maintenance worker settled before window close");
        auto probeBackend = std::make_shared<Backend>(); probeBackend->probeHold = true;
        auto probeService = std::make_shared<DeviceMaintenanceService>(probeBackend);
        DeviceMaintenancePanel probePanel(nullptr, probeService); probePanel.resize(720, 600); probePanel.show();
        probePanel.setDevice(device); qunsetenv("TESTTOOLS_OFFLINE");
        check(await([&] { return probeBackend->probeEntered.load(); }), "Loader probe did not start on worker");
        check(!probePanel.loader_->isHidden() && !probePanel.loader_->isEnabled() && probePanel.loaderHint_->text().contains(QStringLiteral("识别")), "checking Loader state is unclear");
        probeBackend->probeHold = false;
        check(await([&] { return probeService->loaderSupport(device).state == LoaderSupportState::Unknown; }), "unknown Loader probe did not settle");
        probeBackend->support = LoaderSupportState::Unsupported; probeService->cancelLoaderProbe();
        check(await([&] { return probeService->loaderSupport(device).state == LoaderSupportState::Unsupported; }), "unsupported Loader state missing");
        check(!probePanel.loader_->isEnabled(), "unsupported Loader allowed");
        probeBackend->support = LoaderSupportState::Supported; probeService->cancelLoaderProbe();
        check(await([&] { return probePanel.loader_->isEnabled(); }), "supported Loader not enabled");
        check(probePanel.loader_->text() == QStringLiteral("进入 Loader") && probePanel.reboot_->text() == QStringLiteral("重启设备") && probePanel.logcat_->text().endsWith(QStringLiteral("…")), "maintenance action punctuation incorrect");
        probePanel.conflicts = [](const AdbDevice&) { MaintenanceConflicts c; c.board = true; return c; };
        QTest::qWait(180); check(!probePanel.loader_->isEnabled(), "same-device conflict did not block Loader");
        probePanel.conflicts = {}; QTest::qWait(180);
        QTimer::singleShot(0, [] { for (auto* w : QApplication::topLevelWidgets()) if (auto* box = qobject_cast<QMessageBox*>(w)) box->reject(); });
        probePanel.loader_->click(); check(probeBackend->calls == 0, "cancelled Loader confirmation executed");
        probeBackend->probeHold = true; probeBackend->probeEntered = false; probeService->cancelLoaderProbe();
        check(await([&] { return probeBackend->probeEntered.load(); }), "closing probe fixture did not start");
        probePanel.shutdown();
        check(await([&] { return probePanel.shutdownEmitted_; }), "closing did not cancel and drain probe");
        probePanel.hide(); qputenv("TESTTOOLS_OFFLINE", "1");
        std::cout << "PASS maintenance UI, responsive layout, confirmations, context checks, "
                     "interlocks, Windows COM independence, task IDs, partial results and close\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
