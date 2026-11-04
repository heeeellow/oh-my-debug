#include "application/device/file_transfer_service.h"
#include "application/device/screen_mirror_service.h"
#include "application/support/background_task_group.h"
#include "ui/widgets/file_pane.h"
#include "test_paths.h"
#include <QtTest/QTest>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtWidgets>
#define private public
#define protected public
#include "ui/main_window.h"
#include "ui/pages/file_transfer_workspace.h"
#include "ui/pages/serial_workspace.h"
#include "ui/pages/serial_terminal_workspace.h"
#include "ui/screen_mirror_panel.h"
#undef protected
#undef private
#include <iostream>
#include <thread>

void check(bool b, const char* m)
{
    if (!b)
        throw std::runtime_error(m);
}
bool await(const std::function<bool()>& p, int ms = 6000)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        if (p())
            return true;
        QTest::qWait(10);
    }
    return p();
}
class Files final : public testtools::IFileTransferBackend {
  public:
    testtools::FileTransferResult change(const testtools::FileChange&, const std::atomic<bool>&) override
    { return {testtools::TransferOutcome::Failed, "fixture change unsupported"}; }

    std::atomic<int> transfers{0}, listings{0};
    bool failOnce = false;
    std::vector<std::string> serials, targets;
    testtools::FileListing
    list(const std::string&, const std::string& dir, const std::atomic<bool>&) override
    {
        ++listings;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (dir == "/denied")
            return {dir, {}, "Permission denied (fixture)"};
        testtools::FileListing out{dir, {}, {}};
        if (dir == "/1000" || dir == "/10000") {
            int n = dir == "/1000" ? 1000 : 10000;
            for (int i = 0; i < n; ++i)
                out.entries.push_back({"log-" + std::to_string(i) + ".log",
                                       testtools::FileKind::Regular,
                                       std::uintmax_t(i),
                                       "1792554000"});
            return out;
        }
        out.entries = {{"boot.log", testtools::FileKind::Regular, 200, "1792554000"},
                       {u8"音频日志.log", testtools::FileKind::Regular, 300, "1792554000"},
                       {"capture", testtools::FileKind::Directory, 0, "1792554000"},
                       {"sdcard",
                        testtools::FileKind::Symlink,
                        0,
                        "1792554000",
                        testtools::FileKind::Directory,
                        "/storage/self/primary"},
                       {"broken", testtools::FileKind::Symlink, 0, "1792554000"}};
        return out;
    }
    testtools::FileTransferResult transfer(testtools::TransferDirection,
                                           const std::string& serial,
                                           const std::filesystem::path&,
                                           const std::string& remote,
                                           testtools::FileKind,
                                           bool,
                                           const std::atomic<bool>& cancel,
                                           const testtools::FileProgress& progress) override
    {
        ++transfers;
        serials.push_back(serial);
        targets.push_back(remote);
        for (int i = 0; i < 20; ++i) {
            if (cancel)
                return {testtools::TransferOutcome::Cancelled, "cancelled"};
            progress("fixture 50%");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (failOnce) {
            failOnce = false;
            return {testtools::TransferOutcome::Failed, "fixture transient failure"};
        }
        return {testtools::TransferOutcome::Completed, "fixture completed"};
    }
};
class Mirror final : public testtools::IScreenMirrorBackend {
  public:
    bool running = false;
    int starts = 0, controls = 0;
    bool start(const std::string&, std::string&) override
    {
        ++starts;
        running = true;
        return true;
    }
    void stop() override
    {
        running = false;
    }
    void requestStop() noexcept override {}
    testtools::MirrorProcessStatus poll() override
    {
        return {running, 0, {}};
    }
    bool control(testtools::MirrorControl, std::string&) override
    {
        ++controls;
        return true;
    }
    bool screenshot(const std::filesystem::path&, std::string&) override
    {
        return true;
    }
};
int main(int argc, char** argv)
{
    try {
        QApplication app(argc, argv);
        app.setStyle("Fusion");
        app.setFont(QFont("Microsoft YaHei UI", 10));
        QTemporaryDir preferences;
        check(preferences.isValid(), "isolated file preferences");
        qputenv("TESTTOOLS_DATA_DIR", preferences.path().toUtf8());
        qputenv("TESTTOOLS_OFFLINE", "1");
        if (argc == 4 && std::string(argv[1]) == "--live-files")
            qputenv("ADB_PATH", argv[3]);
        auto* w = new MainWindow;
        w->resize(1366, 768);
        w->show();
        auto* f = w->fileWorkspace_;
        check(!f->download_->isEnabled(), "offline disabled");
        if (argc == 4 && std::string(argv[1]) == "--live-files") {
            qputenv("ADB_PATH", argv[3]);
            {
                QSignalBlocker block(w->deviceCombo_);
                w->deviceCombo_->clear();
                const auto serial = QString::fromUtf8(argv[2]);
                w->deviceCombo_->addItem(serial, serial);
                w->deviceCombo_->setItemData(0, "device", Qt::UserRole + 1);
            }
            w->agentConnecting_ = true;
            qunsetenv("TESTTOOLS_OFFLINE");
            w->deviceCombo_->currentIndexChanged(0);
            w->agentConnecting_ = false;
            w->navigateTo("files");
            check(await([&] { return !f->remoteLoading_ && f->remote_.path == "/"; }, 12000),
                  "live entry auto-loads root");
            f->remotePane_->navigate("/sdcard");
            check(await([&] { return !f->remoteLoading_ && f->remote_.path == "/sdcard"; }, 12000),
                  "live sdcard browsing");
            const auto generation = f->remoteGeneration_;
            w->navigateTo("serial");
            w->navigateTo("files");
            check(await(
                      [&] {
                          return f->remoteGeneration_ > generation && !f->remoteLoading_ &&
                                 f->remote_.path == "/sdcard";
                      },
                      12000),
                  "live reentry refreshes sdcard");
            const auto visited = f->remotePane_->directoryVisited;
            f->remotePane_->directoryVisited = [visited](const QString& directory) {
                QElapsedTimer timer; timer.start(); visited(directory);
                std::cout << "{\"location_refresh_ms\":" << timer.elapsed() << "}\n";
            };
            qint64 maxEventGap = 0;
            for (const auto& directory : {QStringLiteral("/proc"), QStringLiteral("/sdcard/oem_log")}) {
                QElapsedTimer elapsed; elapsed.start(); qint64 lastTick = 0;
                QTimer heartbeat;
                QObject::connect(&heartbeat, &QTimer::timeout, w, [&] {
                    const auto now = elapsed.elapsed(); maxEventGap = qMax(maxEventGap, now - lastTick); lastTick = now;
                });
                heartbeat.start(10); f->remotePane_->navigate(directory);
                check(await([&] { return !f->remoteLoading_ && QString::fromStdString(f->remote_.path) == directory; }, 12000),
                      "live proc and OEM log browsing");
                maxEventGap = qMax(maxEventGap, elapsed.elapsed() - lastTick);
                heartbeat.stop();
                std::cout << "{\"ui_path\":\"" << directory.toStdString() << "\",\"milliseconds\":" << elapsed.elapsed()
                          << ",\"entries\":" << f->remote_.entries.size() << "}\n";
                QTest::qWait(50); // 计时结束后再等待布局稳定，快照不包含读取提示的过渡几何。
                const auto name = directory == "/proc" ? QStringLiteral("files-live-proc") : QStringLiteral("files-live-oem-log");
                const auto pix = w->grab(); check(pix.save(testOutputPath(name + ".png")), "save live directory snapshot");
                QFile metadata(testOutputPath(name + ".json"));
                check(metadata.open(QIODevice::WriteOnly), "live snapshot metadata");
                metadata.write(QJsonDocument(QJsonObject{{"read_only_live", true}, {"path", directory},
                    {"logical_width", w->width()}, {"logical_height", w->height()}, {"pixel_width", pix.width()},
                    {"pixel_height", pix.height()}, {"device_pixel_ratio", pix.devicePixelRatio()}}).toJson());
            }
            std::cout << "{\"ui_timer_max_gap_ms\":" << maxEventGap << "}\n";
            QTest::qWait(100);
            w->grab().save(testOutputPath("files-live-device.png"));
            std::cout << "Read-only live file entry, sdcard and reentry refresh passed; entries="
                      << f->remote_.entries.size() << std::endl;
            qputenv("TESTTOOLS_OFFLINE", "1");
            w->close();
            check(await([&] { return !w->isVisible(); }), "live file shutdown");
            delete w;
            return 0;
        }
        auto backend = std::make_shared<Files>();
        f->service_ = std::make_shared<testtools::FileTransferService>(backend);
        auto mb = std::make_unique<Mirror>();
        auto* mirror = mb.get();
        w->screenMirrorPanel_->service_ =
            std::make_shared<testtools::ScreenMirrorService>(std::move(mb));
        qunsetenv("TESTTOOLS_OFFLINE");
        {
            QSignalBlocker block(w->deviceCombo_);
            w->deviceCombo_->clear();
            w->deviceCombo_->addItem("fixture device-A", "device-A");
            w->deviceCombo_->setItemData(0, "device", Qt::UserRole + 1);
        }
        w->agentConnecting_ = true;
        w->deviceCombo_->currentIndexChanged(0);
        w->agentConnecting_ = false;
        f->setDevice({"device-A", "device", ""});
        w->screenMirrorPanel_->setDevice({"device-A", "device", ""});
        check(w->navigateTo("files") && !w->adbChildren_->isHidden(), "navigation expands");
        check(await([&] { return !f->remoteLoading_ && f->remote_.path == "/"; }),
              "enter files auto-loads remote root");
        check(w->adbGroupButton_->isChecked() && !w->serialGroupButton_->isChecked(),
              "parent selected with child");
        check(w->terminalButtons_.size() == 1 &&
                  !f->parentWidget()->findChild<QPushButton*>("adbTerminalButton"),
              "terminal entry only in remote debugging");
        w->adbExpandButton_->click();
        check(w->adbChildren_->isHidden(), "arrow collapse");
        QPushButton* parent = nullptr;
        for (auto* b : w->findChildren<QPushButton*>())
            if (b->text() == QStringLiteral("ADB 调试"))
                parent = b;
        check(parent, "ADB parent");
        parent->click();
        w->navigateTo("mirror");
        w->navigateTo("serial");
        parent->click();
        check(w->pages_->currentIndex() == MainWindow::MirrorPage, "restore last child");
        check(mirror->starts == 0, "no automatic mirror");
        QTest::keyClick(w->screenMirrorPanel_, Qt::Key_Escape);
        check(w->screenMirrorPanel_->isVisible(), "embedded panel survives Escape");
        w->screenMirrorPanel_->startCurrentDevice();
        check(await([&] { return w->screenMirrorPanel_->state_.running; }),
              "explicit mirror start");
        w->grab().save(testOutputPath("mirror-live.png"));
        w->navigateTo("files");
        check(mirror->running, "keep mirror across pages");
        const auto local = testOutputPath("ui-local");
        QDir().mkpath(local);
        QFile fixture(local + "/upload.apk");
        check(fixture.open(QIODevice::WriteOnly), "local fixture");
        fixture.write("not an installable APK; transfer only");
        fixture.close();
        f->localPane_->navigate(local);
        f->remotePane_->navigate("/delayed");
        f->setDevice({"device-B", "device", ""});
        f->setDevice({"device-A", "device", ""});
        QTest::qWait(80);
        check(f->remote_.path != "/delayed", "stale generation discarded");
        f->remotePane_->navigate("/logs");
        f->remotePath_->setText("/typed");
        check(await([&] { return !f->localLoading_ && !f->remoteLoading_; }), "lists finish");
        check(f->remotePath_->text() == "/typed", "preserve newer typed path");
        f->remotePath_->setText("/logs");
        f->remotePane_->selectNames({"boot.log"});
        check(f->download_->isEnabled(), "selection ready");
        const auto beforeEntry = backend->listings.load();
        f->remotePath_->setText("/unsubmitted");
        w->navigateTo("serial");
        w->navigateTo("files");
        check(await([&] {
                  return backend->listings > beforeEntry && !f->remoteLoading_ && !f->localLoading_;
              }),
              "reenter refreshes current directory");
        check(f->remote_.path == "/logs" && f->remotePath_->text() == "/unsubmitted",
              "refresh preserves unsubmitted path");
        check(f->remotePane_->selectedEntries().size() == 1, "entry refresh preserves selection");
        f->remotePath_->setText("/logs");
        auto* filter = f->findChild<QLineEdit*>("fileNameFilter");
        filter->setText(QStringLiteral("音频"));
        check(!f->download_->isEnabled(), "filtered selection not transferred");
        filter->clear();
        f->remotePane_->selectNames({"boot.log"});
        f->remotePane_->navigate("/logs");
        check(await([&] { return !f->remoteLoading_; }), "refresh");
        check(f->remotePane_->selectedEntries().size() == 1, "refresh preserves selection");
        auto* view = f->remotePane_->view();
        QModelIndex link;
        for (int i = 0; i < view->model()->rowCount(); ++i)
            if (view->model()->index(i, 0).data().toString() == "sdcard")
                link = view->model()->index(i, 0);
        check(link.isValid(), "link row");
        view->doubleClicked(link);
        check(await([&] { return !f->remoteLoading_; }) && f->remote_.path == "/logs/sdcard",
              "directory link navigation");
        f->remotePane_->navigate("/logs");
        check(await([&] { return !f->remoteLoading_; }), "return logs");
        f->remotePane_->selectNames({"boot.log"});
        backend->failOnce = true;
        w->grab().save(testOutputPath("files-live-1366.png"));
        w->resize(1920, 1080);
        QTest::qWait(30);
        w->grab().save(testOutputPath("files-live-1920.png"));
        w->resize(1366, 768);
        f->download_->click();
        check(f->service_->snapshot().active, "async transfer");
        QTest::qWait(30);
        const auto duringBatch = backend->listings.load();
        w->navigateTo("serial");
        w->navigateTo("files");
        QTest::qWait(40);
        check(f->service_->snapshot().active && backend->listings == duringBatch,
              "entry refresh waits for active transfer");
        w->grab().save(testOutputPath("files-progress.png"));
        check(await([&] { return !f->wasActive_ && !f->localLoading_ && !f->remoteLoading_; }),
              "failed batch finishes");
        check(f->retry_->isEnabled(), "retry offered");
        f->remotePane_->navigate("/other");
        check(await([&] { return !f->remoteLoading_; }), "navigate before retry");
        f->remotePath_->setText("/retry-unsubmitted");
        f->localPath_->setText(QDir::toNativeSeparators(local + "/retry-unsubmitted"));
        f->retry_->click();
        check(await([&] { return !f->wasActive_ && !f->localLoading_ && !f->remoteLoading_; }),
              "retry finishes");
        check(f->remote_.path == "/other" && f->remotePath_->text() == "/retry-unsubmitted" &&
                  QDir::fromNativeSeparators(QString::fromStdString(f->local_.path)) == local &&
                  f->localPath_->text() == QDir::toNativeSeparators(local + "/retry-unsubmitted"),
              "retry completion refreshes loaded directories without submitting address drafts");
        f->remotePath_->setText("/other");
        f->localPath_->setText(QDir::toNativeSeparators(local));
        check(backend->targets.size() == 2 && backend->targets[0] == "/logs/boot.log" &&
                  backend->targets[1] == backend->targets[0],
              "retry original fixed path");
        f->taskToggle_->click();
        check(f->records_->isHidden(), "collapse tasks");
        f->taskToggle_->click();
        f->localPane_->selectNames({"upload.apk"});
        std::unique_ptr<QMimeData> data(f->localPane_->dragData());
        check(data && f->remotePane_->dropData(data.get()), "cross pane drag");
        check(await([&] { return !f->wasActive_ && !f->localLoading_ && !f->remoteLoading_; }),
              "drag batch finish");
        check(backend->targets.back() == "/other/upload.apk", "APK normal file upload");
        QMimeData external;
        external.setUrls({QUrl::fromLocalFile(local + "/upload.apk")});
        check(f->remotePane_->dropData(&external), "external local drop");
        check(await([&] {
                  return backend->transfers == 4 && !f->wasActive_ && !f->localLoading_ &&
                         !f->remoteLoading_;
              }),
              "external drop async source listing then transfer");
        f->remotePane_->navigate("/denied");
        check(await([&] { return !f->remoteLoading_; }), "error list");
        check(!f->download_->isEnabled() && f->status_->text().contains("Permission denied"),
              "permission error state");
        w->grab().save(testOutputPath("files-denied.png"));
        for (const auto dir : {"/1000", "/10000"}) {
            QElapsedTimer t;
            t.start();
            f->remotePane_->navigate(dir);
            check(await([&] { return !f->remoteLoading_; }), "large list loaded");
            auto load = t.elapsed();
            auto* v = f->remotePane_->view();
            v->verticalScrollBar()->setValue(400);
            const int scroll = v->verticalScrollBar()->value();
            const auto name = QStringLiteral("log-500.log");
            f->remotePane_->selectNames({name});
            t.restart();
            filter->setText("log-50");
            QCoreApplication::processEvents();
            auto filterMs = t.elapsed();
            filter->clear();
            t.restart();
            v->sortByColumn(1, Qt::DescendingOrder);
            QCoreApplication::processEvents();
            auto sort = t.elapsed();
            v->sortByColumn(0, Qt::AscendingOrder);
            v->verticalScrollBar()->setValue(scroll);
            f->remotePane_->navigate(dir);
            check(await([&] { return !f->remoteLoading_; }), "large refresh");
            check(f->remotePane_->selectedEntries().size() == 1 &&
                      v->verticalScrollBar()->value() == scroll,
                  "large refresh selection/scroll");
            std::cout << dir << " load+UI=" << load << "ms filter=" << filterMs
                      << "ms sort=" << sort << "ms\n";
        }
        // Single, dual and terminal are the same extracted connection/receive components.
        w->navigateTo("serial");
        for (const auto mode : {"single", "dual", "terminal"}) {
            QPushButton* child = nullptr;
            for (auto* b : w->findChildren<QPushButton*>())
                if (b->property("serialMode").toString() == mode)
                    child = b;
            check(child, "serial child exists");
            child->click();
            SerialEndpointWorkspace* activeWorkspace = std::string(mode) == "terminal" ? static_cast<SerialEndpointWorkspace*>(w->terminalWorkspace_) : static_cast<SerialEndpointWorkspace*>(w->serialWorkspace_);
            check(child->isChecked() && activeWorkspace->currentWorkspaceMode() == mode,
                  "left serial mode navigation");
            QTest::qWait(80);
            auto* port = activeWorkspace->channels_[activeWorkspace->primaryChannelIndex()].portCombo;
            std::cerr << mode << " port visible=" << port->isVisible() << " size=" << port->width()
                      << "," << port->height() << " global=" << port->mapTo(w, QPoint()).x() << ","
                      << port->mapTo(w, QPoint()).y() << " window=" << w->width() << std::endl;
            check(port->isVisible(), "serial connection controls visible in all modes");
            w->grab().save(testOutputPath(QString("serial-%1.png").arg(mode)));
        }
        w->serialExpandButton_->click();
        check(w->serialChildren_->isHidden() &&
                  w->pages_->currentIndex() == MainWindow::TerminalPage,
              "serial arrow only collapses");
        w->navigateTo("files");
        w->navigateTo("serial");
        check(!w->serialChildren_->isHidden() &&
                  w->pages_->currentIndex() == MainWindow::TerminalPage,
              "serial parent restores last mode");
        w->navigateTo("files");
        f->remotePane_->navigate("/logs");
        check(await([&] { return !f->remoteLoading_; }), "final list");
        f->remotePane_->selectNames({"boot.log"});
        f->download_->click();
        f->setDevice({"device-B", "device", ""});
        check(!f->retry_->isEnabled(), "retry cannot cross devices");
        w->navigateTo("serial");
        w->navigateTo("files");
        check(await([&] {
                  return !f->wasActive_ && !f->localLoading_ && !f->remoteLoading_ &&
                         f->remote_.path == "/";
              }),
              "deferred entry refresh loads newly selected device after old batch");
        check(backend->serials.back() == "device-A", "active batch retains original device");
        f->remotePane_->selectNames({"boot.log"});
        f->download_->click();
        check(f->service_->snapshot().active, "final close has active file batch");
        // Local file/mirror fixtures own no board-side jobs; never run real ADB exit cleanup.
        qputenv("TESTTOOLS_OFFLINE", "1");
        w->close();
        check(await([&] {
                  return !w->isVisible() && w->fileShutdownComplete_ && w->mirrorShutdownComplete_;
              }),
              "async close joins both services");
        check(!mirror->running && !f->service_->snapshot().active, "owned sessions closed");
        delete w;
        std::cout << "Qt behavior checks passed; DPR=" << app.primaryScreen()->devicePixelRatio()
                  << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
