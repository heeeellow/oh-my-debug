#include "../board_info_fixture.h"
#include "application/device/device_maintenance_service.h"
#include "core/protocol/agent_version.h"
#include "test_paths.h"
#include "ui/theme.h"
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtTest/QTest>
#include <QtWidgets>
#include <iostream>
#define private public
#include "ui/main_window.h"
#include "ui/widgets/board_info_panel.h"
#include "ui/widgets/device_maintenance_panel.h"
#undef private

namespace {
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
        QTest::qWait(5);
    }
    return predicate();
}
void capture(QWidget* widget, const QString& name)
{
    if (!qEnvironmentVariableIsSet("TESTTOOLS_BOARD_INFO_SNAPSHOTS"))
        return;
    QTest::qWait(60);
    const auto pixels = widget->grab();
    check(pixels.save(testOutputPath(name + ".png")), "save device page image");
    QFile file(testOutputPath(name + ".json"));
    check(file.open(QIODevice::WriteOnly), "image metadata");
    file.write(QJsonDocument(QJsonObject{{"fixture", true},
                                         {"logical_width", widget->width()},
                                         {"logical_height", widget->height()},
                                         {"pixel_width", pixels.width()},
                                         {"pixel_height", pixels.height()},
                                         {"device_pixel_ratio", pixels.devicePixelRatio()}})
                   .toJson());
}
} // namespace
int main(int argc, char** argv)
{
    try {
        QApplication app(argc, argv);
        app.setStyle("Fusion");
        app.setFont(QFont("Microsoft YaHei UI", 10));
        app.setStyleSheet(testtools::ui::applicationStyleSheet());
        app.setQuitOnLastWindowClosed(false);
        qputenv("TESTTOOLS_OFFLINE", "1");
        MainWindow window;
        window.resize(1366, 768);
        window.show();
        window.navigateTo("device");
        auto* panel = window.boardInfoPanel_;
        check(!panel->all_->isEnabled(), "no service means no board reads");
        auto select = [&](const QString& serial) {
            QSignalBlocker block(window.deviceCombo_);
            window.deviceCombo_->clear();
            window.deviceCombo_->addItem(serial, serial);
            window.deviceCombo_->setItemData(0, "device", Qt::UserRole + 1);
        };
        select("device-A");
        window.deviceCombo_->currentIndexChanged(0);
        window.agentStatusLabel_->setText(QStringLiteral("板端服务 · 已就绪（夹具）"));
        window.agentConnectionDetail_->setText(QStringLiteral("只读信息交互夹具"));
        window.agentVersionValue_->setText(QStringLiteral("应用要求：v%1 · 设备：v%1").arg(QString::fromLatin1(testtools::AgentVersion)));
        window.agentSecurityValue_->setText(QStringLiteral("连接权限：夹具"));
        window.agentCapabilitiesValue_->setText(QStringLiteral("内存容量、存储信息、存储健康"));
        window.agentModelValue_->setText(QStringLiteral("模拟设备"));
        window.agentHardwareValue_->setText(
            QStringLiteral("aarch64 · 4 核 CPU · 3.80 GiB 系统可见内存"));
        auto connection = std::make_shared<boardfixture::Connection>();
        panel->setSession("device-A", 1, connection, {"memory.capacity"});
        QTest::qWait(180);
        check(!panel->all_->isEnabled() && connection->calls == 0,
              "memory is already in system summary, no duplicate task or row");
        window.navigateTo("remote");
        panel->setSession("device-A", 2, connection, {"storage.device.info"});
        QTest::qWait(180);
        check(connection->calls == 0, "hidden dashboard does not start information reads");
        window.navigateTo("device");
        check(await([&] { return !panel->wasActive_ && panel->snapshot().id != 0; }),
              "entering dashboard automatically loads declared storage capability");
        check(connection->starts() == std::vector<std::string>{"storage.device.info"} &&
                  panel->healthSummary_->text() == QStringLiteral("未提供"),
              "unavailable health stays explicit without issuing unsupported task");
        connection = std::make_shared<boardfixture::Connection>();
        panel->setSession("device-A", 3, connection, testtools::BoardInfoService::capabilities());
        check(await([&] { return !panel->wasActive_; }), "new session auto reads complete");
        const std::vector<std::string> storageActions{"storage.device.info",
                                                      "storage.device.health"};
        check(connection->starts() == storageActions, "only the two storage actions run in order");
        check(panel->storageSummary_->text().contains("eMMC mmcblk0") &&
                  panel->storageSummary_->text().contains("UFS sda") &&
                  panel->storageSummary_->text().contains("29.12 GiB") &&
                  panel->storageSummary_->text().contains("58.24 GiB"),
              "all eMMC and UFS candidates shown");
        check(panel->findChildren<QTreeWidget*>().isEmpty() && panel->height() < 150 &&
                  panel->parentWidget() == window.agentHardwareValue_->parentWidget(),
              "storage is a compact part of hardware summary without a main-page table");
        const auto cachedId = panel->snapshot().id;
        window.navigateTo("remote");
        window.navigateTo("device");
        QTest::qWait(180);
        check(panel->snapshot().id == cachedId && connection->starts() == storageActions,
              "returning to same session reuses loaded information");
        panel->all_->click();
        check(await([&] { return !panel->wasActive_; }) && panel->snapshot().id != cachedId &&
                  connection->starts().size() == 4,
              "explicit refresh remains available");
        auto* columns = window.findChild<QWidget*>("deviceInfoColumns");
        check(columns && columns->property("columnCount").toInt() == 2,
              "normal information cards side by side");
        if (argc > 1) {
            window.resize(1366, 768);
            QTest::qWait(70);
            capture(&window, "device-info-highdpi");
        } else {
            capture(&window, "device-info-1366");
            window.resize(1920, 1080);
            QTest::qWait(60);
            capture(&window, "device-info-1920");
            window.resize(1120, 768);
            QTest::qWait(100);
            check(columns->property("columnCount").toInt() == 1, "narrow device cards stack");
            capture(&window, "device-info-narrow");
            window.resize(1366, 768);
        }
        bool detailsSeen = false;
        QTimer::singleShot(80, [&] {
            for (auto* widget : QApplication::topLevelWidgets())
                if (auto* dialog = qobject_cast<QDialog*>(widget)) {
                    auto* raw = dialog->findChild<QPlainTextEdit*>();
                    if (raw) {
                        auto* values = dialog->findChild<QTreeWidget*>("boardInfoDetailValues");
                        check(values && values->topLevelItemCount() == 2 &&
                                  values->topLevelItem(0)->childCount() == 2 &&
                                  values->topLevelItem(1)->childCount() == 2,
                              "all candidates and fields remain available on demand");
                        auto* health = values->topLevelItem(1)->child(0);
                        check(health->child(0)->text(2) == "0" &&
                                  health->child(2)->text(2) == QStringLiteral("未知") &&
                                  health->child(3)->text(2) == QStringLiteral("0 字节"),
                              "details distinguish real zero from missing health fields");
                        check(raw->toPlainText().contains("device-A") &&
                                  raw->toPlainText().contains("life_time_b") &&
                                  raw->toPlainText().contains("provider"),
                              "raw evidence includes original schema, provider and null");
                        detailsSeen = true;
                        dialog->reject();
                    }
                }
        });
        panel->details_->click();
        check(detailsSeen, "raw result dialog opens");
        connection = std::make_shared<boardfixture::Connection>();
        connection->mode = boardfixture::Mode::FailStorage;
        panel->setSession("device-A", 4, connection, testtools::BoardInfoService::capabilities());
        check(await([&] { return !panel->wasActive_; }), "item failure settled");
        check(panel->storageSummary_->text() == QStringLiteral("读取失败") &&
                  panel->healthSummary_->text().contains(QStringLiteral("已读取")),
              "read failure is separate from later successful information");
        if (argc == 1)
            capture(&window, "device-info-read-failure");

        testtools::MaintenanceRequest maintenance;
        maintenance.device = {"device-A", "device", "fixture"};
        maintenance.action = testtools::MaintenanceAction::RootRemount;
        std::string error;
        check(window.maintenanceService_->prepare(maintenance, {}, error),
              "maintenance guard fixture prepared without device I/O");
        const auto before = connection->calls.load();
        panel->all_->clicked(false);
        check(connection->calls == before,
              "active maintenance blocks information even through direct button signal");
        window.maintenanceService_->cancel();
        window.maintenanceService_->executePrepared();
        connection = std::make_shared<boardfixture::Connection>();
        connection->mode = boardfixture::Mode::Hold;
        panel->setSession("device-A", 5, connection, testtools::BoardInfoService::capabilities());
        check(await([&] { return connection->starts().size() == 1; }), "held job active");
        check(panel->cancel_->isVisible() &&
                  panel->cancel_->mapTo(&window, QPoint()).y() < window.height() - 40,
              "stop action remains visible in standard window");
        check(window.maintenancePanel_->conflicts({"device-A", "device", "fixture"}).board,
              "reader blocks same-device maintenance");
        QMetaObject::invokeMethod(window.taskMenu_, "aboutToShow", Qt::DirectConnection);
        auto* oldStop = window.taskMenu_->actions().back();
        const auto oldId = panel->snapshot().id;
        check(window.taskButton_->isVisible(), "board task appears in global task summary");
        select("device-B");
        window.deviceCombo_->currentIndexChanged(0);
        auto second = std::make_shared<boardfixture::Connection>();
        second->storageBytes = 64LL * 1024 * 1024 * 1024;
        second->mode = boardfixture::Mode::Hold;
        panel->setSession("device-B", 6, second, testtools::BoardInfoService::capabilities());
        check(connection->starts().size() == 1 &&
                  panel->storageSummary_->text() == QStringLiteral("—"),
              "old observations never enter new device");
        check(await([&] { return panel->snapshot().id != oldId && second->starts().size() == 1; }),
              "new session read starts");
        oldStop->trigger();
        check(!panel->snapshot().cancelling, "old global stop cannot cancel new board task");
        second->mode = boardfixture::Mode::Normal;
        check(await([&] { return !panel->wasActive_; }), "new device information complete");
        check(panel->storageSummary_->text().contains("64.00 GiB") &&
                  !panel->storageSummary_->text().contains("29.12 GiB"),
              "new generation values replace old data");
        second = std::make_shared<boardfixture::Connection>();
        second->mode = boardfixture::Mode::Hold;
        panel->setSession("device-B", 7, second, testtools::BoardInfoService::capabilities());
        check(await([&] { return second->starts().size() == 1; }), "active close fixture");
        window.close();
        check(await([&] { return !window.isVisible(); }),
              "window waits for board task cancellation and drainage");
        check(!panel->snapshot().active && window.boardShutdownComplete_,
              "board worker settled before window closed");
        std::cout << "PASS device information UI, capability gates, observations, raw results, "
                     "responsive cards, maintenance interlocks, generations, task IDs and close\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
