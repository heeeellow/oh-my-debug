#include "application/device/file_transfer_service.h"
#include "adapters/device/file_management.h"
#include "ui/widgets/file_pane.h"
#include "ui/widgets/file_permissions_dialog.h"
#include "test_paths.h"
#include <QtTest/QTest>
#include <QtWidgets>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <atomic>
#include <iostream>
#include <mutex>
#include <map>
#include <thread>
#include <algorithm>
#define private public
#include "ui/main_window.h"
#include "ui/pages/file_transfer_workspace.h"
#undef private

namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool await(const std::function<bool()>& test, int timeout=6000) {
    QElapsedTimer timer; timer.start();
    while (timer.elapsed()<timeout) { if(test()) return true; QTest::qWait(5); }
    return test();
}
bool highDpi = false;
void capture(QWidget* widget, const QString& name) {
    if (!qEnvironmentVariableIsSet("TESTTOOLS_FILE_SNAPSHOTS") || (highDpi && name!="files-high-dpi")) return;
    const auto filter = qEnvironmentVariable("TESTTOOLS_FILE_CAPTURE_FILTER");
    if (!filter.isEmpty() && !filter.split(QLatin1Char(',')).contains(name)) return;
    QTest::qWait(50);
    const auto pix=widget->grab();
    check(pix.save(testOutputPath(name+".png")), "save representative image");
    QFile meta(testOutputPath(name+".json"));
    QJsonObject value{{"fixture",true},{"scenario",name},{"logical_width",widget->width()},{"logical_height",widget->height()},
        {"pixel_width",pix.width()},{"pixel_height",pix.height()},{"device_pixel_ratio",pix.devicePixelRatio()}};
    check(meta.open(QIODevice::WriteOnly) && meta.write(QJsonDocument(value).toJson())>0, "save image metadata");
}
class Files final : public testtools::IFileTransferBackend {
  public:
    std::atomic<int> changes{0};
    std::atomic<bool> failRead{false}, holdChange{false};
    std::mutex mutex;
    std::map<std::string,std::vector<testtools::FileEntry>> contents;
    testtools::FileListing list(const std::string& serial,const std::string& dir,const std::atomic<bool>&) override {
        if(failRead) return {dir,{},"fixture: permission metadata unavailable"};
        if(dir=="/missing") return {dir,{},"fixture: directory missing"};
        std::lock_guard<std::mutex> lock(mutex);
        auto found=contents.find(dir);
        if(found==contents.end()) found=contents.emplace(dir,std::vector<testtools::FileEntry>{{"boot.log",testtools::FileKind::Regular,40,"1"},
            {"blocked.log",testtools::FileKind::Regular,20,"1"},{"folder",testtools::FileKind::Directory}}).first;
        for (auto& e : found->second) if (e.permissions.empty()) e.permissions = e.kind == testtools::FileKind::Directory ? "0750" : "0644";
        return {dir,found->second,{},serial=="B"?"rockchip_linux":"android"};
    }
    testtools::FileTransferResult transfer(testtools::TransferDirection,const std::string&,const std::filesystem::path&,
        const std::string&,testtools::FileKind,bool,const std::atomic<bool>&,const testtools::FileProgress&) override
    {return {testtools::TransferOutcome::Completed,"fixture transfer"};}
    testtools::FileTransferResult change(const testtools::FileChange& request,const std::atomic<bool>& cancel) override {
        ++changes;
        while (holdChange && !cancel) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if(!request.remote) return testtools::changeLocalFile(request,cancel);
        for(int i=0;i<15;++i) { if(cancel) return {testtools::TransferOutcome::Cancelled,"已停止，保留已完成项"}; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        if(request.entry.name=="blocked.log") return {testtools::TransferOutcome::Failed,"fixture: permission denied"};
        std::lock_guard<std::mutex> lock(mutex);
        auto& entries=contents[request.parent];
        auto found=std::find_if(entries.begin(),entries.end(),[&](const auto& e){return e.name==request.entry.name;});
        using O=testtools::FileOperation;
        if(request.operation==O::CreateFolder) {
            if(found!=entries.end()) return {testtools::TransferOutcome::Failed,"同名项目已存在"};
            entries.push_back(request.entry);
        } else if(found==entries.end()) return {testtools::TransferOutcome::Failed,"目标已消失"};
        else if(request.operation==O::Remove) entries.erase(found);
        else if(request.operation==O::Rename) found->name=request.newName;
        else if(request.operation==O::SetPermissions) found->permissions=request.permissions;
        return {testtools::TransferOutcome::Completed,"fixture completed"};
    }
};
void permissionAction(QWidget* workspace, const std::function<void(QDialog*)>& action) {
    auto* timer = new QTimer(workspace);
    auto elapsed = std::make_shared<QElapsedTimer>(); elapsed->start();
    QObject::connect(timer, &QTimer::timeout, workspace, [workspace, timer, elapsed, action] {
        auto* dialog = workspace->findChild<QDialog*>("filePermissionsDialog");
        if (!dialog || !dialog->isVisible()) return;
        const auto* current = dialog->findChild<QLabel*>("filePermissionCurrent");
        if (!current || current->text() == QStringLiteral("正在读取当前权限…")) {
            if (elapsed->elapsed() > 5000) { timer->stop(); timer->deleteLater(); dialog->reject(); }
            return;
        }
        timer->stop(); timer->deleteLater(); action(dialog);
    });
    timer->start(10);
}
void enterName(const QString& name) {
    QTimer::singleShot(30,[name] {
        for(auto* widget:QApplication::topLevelWidgets()) if(auto* dialog=qobject_cast<QInputDialog*>(widget)) {
            dialog->setTextValue(name); QMetaObject::invokeMethod(dialog,"accept",Qt::QueuedConnection); return;
        }
    });
}
}
int main(int argc,char** argv) {
    try {
        qputenv("TESTTOOLS_OFFLINE","1");
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        QApplication app(argc,argv); app.setStyle("Fusion"); app.setQuitOnLastWindowClosed(false);
        highDpi=argc>1 && std::string(argv[1])=="--high-dpi";
        QTemporaryDir storage; check(storage.isValid(),"isolated preferences");
        qputenv("TESTTOOLS_DATA_DIR",storage.path().toUtf8());
        const auto local=storage.path()+"/local";
        QDir().mkpath(local);
        QFile file(local+"/source.log"); check(file.open(QIODevice::WriteOnly),"local fixture");
        file.write("source");file.close();
        MainWindow window;window.resize(1366,768);window.show();
        QApplication::setActiveWindow(&window);
        auto* work=window.fileWorkspace_;
        auto backend=std::make_shared<Files>();
        check(await([&]{return !work->localLoading_;}),"initial local listing");
        work->service_=std::make_shared<testtools::FileTransferService>(backend);
        qunsetenv("TESTTOOLS_OFFLINE");
        work->setDevice({"A","device",""}); window.navigateTo("files");
        work->localPane_->navigate(local);
        check(await([&]{return !work->localLoading_&&!work->remoteLoading_&&!work->remote_.path.empty();}),"open both panes");
        auto* address=work->remotePane_->findChild<QComboBox*>("fileRemoteAddress");
        check(address && address->findData("/sdcard")>=0 && address->findData("/sdcard/oem_log")>=0 &&
            address->findData("/tmp")<0 && work->records_->isHidden(),"Android log preset, no assumed tmp, empty tasks collapsed");
        work->remotePane_->navigate("/logs");
        check(await([&]{return work->remote_.path=="/logs"&&!work->remoteLoading_;}),"visit logs");
        auto* bookmark = work->remotePane_->findChild<QToolButton*>("fileBookmarks");
        check(bookmark && !bookmark->menu() && !work->remotePane_->findChild<QToolButton*>("remoteFileOperations"), "single address-row favorite, no repeated operations menu");
        check(bookmark->parentWidget() == work->remotePane_, "favorite belongs to path pane");
        bookmark->click();
        check(bookmark->isChecked() && bookmark->text() == QStringLiteral("已收藏"), "one-click favorite reflects current path");
        address->showPopup(); capture(address->view()->window(), "files-locations-popup"); address->hidePopup();
        check(address->findText(QStringLiteral("收藏目录"))>=0 && address->findData("/logs")>=0,"favorite directory");
        const auto original=work->remotePane_->view()->model()->rowCount();
        work->remotePane_->navigate("/missing");
        check(await([&]{return !work->remoteLoading_;}) && work->remote_.path=="/logs" &&
            work->remotePane_->view()->model()->rowCount()==original && address->findData("/missing")<0,"failed open retains list and avoids history");
        work->remotePane_->navigate("/logs"); check(await([&]{return !work->remoteLoading_;}),"restore logs");
        capture(&window,"files-empty-tasks");
        work->setDevice({"B","device",""});work->remotePane_->navigate("/");
        check(await([&]{return !work->remoteLoading_;}),"switch B");
        check(address->findData("/userdata")>=0 && address->findData("/logs")<0 && address->findData("/sdcard")<0,"device-specific platform/history/favorites");
        work->setDevice({"A","device",""});work->remotePane_->navigate("/logs");
        check(await([&]{return !work->remoteLoading_;}),"restore A");
        { FileTransferWorkspace restored(nullptr,std::make_shared<testtools::FileTransferService>(backend));
          restored.setDevice({"A","device",""});
          check(restored.remotePath_->text()=="/logs" && restored.locations_.value("A").favorites.contains("/logs"),"preferences survive new workspace");
          restored.shutdown(); }
        const auto complete=[&]{return await([&]{return !work->service_->snapshot().active&&!work->wasActive_&&!work->remoteLoading_&&!work->localLoading_;});};
        using O=testtools::FileOperation;
        backend->holdChange = true;
        enterName(QStringLiteral("新建目录 & '测试"));
        work->remotePane_->changeRequested(O::CreateFolder);
        check(work->service_->snapshot().active && !work->records_->isHidden(),"first operation expands task list");
        check(!address->isEnabled() && !bookmark->isEnabled() && !work->download_->isEnabled(),"mutations gate navigation and transfer");
        const bool runningTaskVisible = await([&]{return window.taskButton_->isVisible();});
        backend->holdChange = false;
        check(runningTaskVisible, "running mutation shows global task entry");
        check(complete() && work->service_->snapshot().records[0].result.outcome==testtools::TransferOutcome::Completed,"create completed and refreshed");
        work->remotePane_->selectNames({QStringLiteral("新建目录 & '测试")});
        enterName("renamed-folder");work->remotePane_->changeRequested(O::Rename);
        check(complete(),"rename completed");
        work->remotePane_->selectNames({"boot.log","blocked.log"});
        bool confirmed=false;
        QTimer::singleShot(30,[&]{
            for(auto* widget:QApplication::topLevelWidgets())if(auto* dialog=qobject_cast<QMessageBox*>(widget)){
                confirmed=dialog->text().contains("/logs")&&dialog->text().contains("A")&&dialog->defaultButton()==dialog->button(QMessageBox::No);
                capture(dialog,"files-delete-confirmation");
                dialog->done(QMessageBox::Yes);return;
            }
        });
        work->remotePane_->changeRequested(O::Remove);
        check(confirmed&&complete(),"delete confirmation fixed target");
        auto state=work->service_->snapshot();
        check(state.records.size()==2&&state.records[0].result.outcome!=state.records[1].result.outcome,"partial result preserved");
        check(work->status_->text().contains("A")&&work->status_->text().contains(QStringLiteral("失败 1")),"refresh retains completed task target and failure summary");
        check(await([&]{return !window.taskButton_->isEnabled() && window.taskButton_->isHidden();}),"idle global task entry disappears");
        window.resize(1920,1080);capture(&window,"files-results-wide");
        window.resize(1120,768);QTest::qWait(80);capture(&window,"files-narrow");
        window.resize(1366,768);
        const auto readOnlyCount = backend->changes.load();
        work->remotePane_->selectNames({"blocked.log"});
        bool initialMode = false, invalidDisabled = false, bitsSynced = false;
        permissionAction(work, [&](QDialog* dialog) {
            auto* number = dialog->findChild<QLineEdit*>("filePermissionMode");
            auto* apply = dialog->findChild<QPushButton*>("applyFilePermissions");
            initialMode = number && number->text() == "0644" && apply->isEnabled();
            number->setText("888"); invalidDisabled = !apply->isEnabled();
            number->setText("2640");
            auto* group = dialog->findChild<QCheckBox*>("permissionBit10");
            auto* execute = dialog->findChild<QCheckBox*>("permissionBit2");
            group->setChecked(false); execute->setChecked(true);
            bitsSynced = number->text() == "0740";
            capture(dialog, "files-permissions-dialog");
            dialog->reject();
        });
        work->remotePane_->changeRequested(O::SetPermissions);
        check(initialMode && invalidDisabled && bitsSynced && backend->changes == readOnlyCount, "permission dialog reads actual mode, validates, syncs bits and cancel never writes");
        work->remotePane_->selectNames({"blocked.log", "renamed-folder"});
        bool mixedMode = false;
        permissionAction(work, [&](QDialog* dialog) {
            auto* number = dialog->findChild<QLineEdit*>("filePermissionMode");
            auto* apply = dialog->findChild<QPushButton*>("applyFilePermissions");
            mixedMode = number->text().isEmpty() && !apply->isEnabled();
            capture(dialog, "files-permissions-mixed"); dialog->reject();
        });
        work->remotePane_->changeRequested(O::SetPermissions);
        check(mixedMode && backend->changes == readOnlyCount, "mixed permissions require explicit value before applying");
        backend->failRead = true; bool readFailure = false;
        permissionAction(work, [&](QDialog* dialog) {
            readFailure = !dialog->findChild<QPushButton*>("applyFilePermissions")->isEnabled() &&
                dialog->findChild<QLabel*>("filePermissionCurrent")->text().contains("unavailable");
            dialog->reject();
        });
        work->remotePane_->changeRequested(O::SetPermissions); backend->failRead = false;
        check(readFailure && backend->changes == readOnlyCount, "failed mode read cannot start chmod");
        work->remotePane_->selectNames({"blocked.log"});
        permissionAction(work, [&](QDialog* dialog) {
            dialog->findChild<QLineEdit*>("filePermissionMode")->setText("600");
            dialog->findChild<QPushButton*>("applyFilePermissions")->click();
        });
        work->remotePane_->changeRequested(O::SetPermissions);
        check(work->lastChange_.permissions == "0600", "manual permissions reach the fixed task snapshot");
        check(work->service_->snapshot().active,"permission change started");
        work->cancel_->click();check(complete()&&work->service_->snapshot().records[0].result.outcome==testtools::TransferOutcome::Cancelled,"cancel reaches owned mutation");
        work->remotePane_->selectNames({"renamed-folder"});
        permissionAction(work, [&](QDialog* dialog) {
            dialog->findChild<QLineEdit*>("filePermissionMode")->setText("700");
            dialog->findChild<QPushButton*>("applyFilePermissions")->click();
        });
        work->remotePane_->changeRequested(O::SetPermissions);
        check(complete() && work->service_->snapshot().records[0].result.outcome == testtools::TransferOutcome::Completed &&
            std::find_if(work->remote_.entries.begin(), work->remote_.entries.end(), [](const auto& e) { return e.name == "renamed-folder" && e.permissions == "0700"; }) != work->remote_.entries.end(),
            "manual non-recursive directory mode applies and refreshes actual permissions");
        const auto beforeContext = backend->changes.load();
        work->remotePane_->selectNames({"blocked.log"});
        bool changedContextDisabled = false;
        permissionAction(work, [&](QDialog* dialog) {
            work->setDevice({"B", "device", ""});
            changedContextDisabled = !dialog->findChild<QPushButton*>("applyFilePermissions")->isEnabled();
            dialog->accept();
        });
        work->remotePane_->changeRequested(O::SetPermissions);
        check(changedContextDisabled && backend->changes == beforeContext, "device change invalidates permission dialog and never mutates the old target");
        work->setDevice({"A", "device", ""}); work->remotePane_->navigate("/logs");
        check(await([&] { return !work->remoteLoading_; }), "restore permission context");
        const auto previous=backend->changes.load();
        QTimer::singleShot(30,[&]{
            work->setDevice({"B","device",""});
            for(auto* widget:QApplication::topLevelWidgets())if(auto* dialog=qobject_cast<QInputDialog*>(widget)){
                dialog->setTextValue("never-created");QMetaObject::invokeMethod(dialog,"accept",Qt::QueuedConnection);return;
            }
        });
        work->remotePane_->changeRequested(O::CreateFolder);
        check(backend->changes==previous,"device change during modal prevents old operation");
        check(await([&]{return !work->remoteLoading_;}),"new device refresh");
        work->localPane_->selectNames({"source.log"});enterName("renamed.log");
        work->localPane_->changeRequested(O::Rename);
        check(complete()&&QFile::exists(local+"/renamed.log")&&!QFile::exists(local+"/source.log"),"local operation without device mutation");
        check(window.activityLabel_->text().contains(QStringLiteral("本地"))&&window.activityLabel_->text().contains(QStringLiteral("重命名")),"activity describes actual completed operation");
        if(highDpi)capture(&window,"files-high-dpi");
        window.close();check(await([&]{return !window.isVisible();}),"owned tasks settle before close");
        std::cout<<"PASS file-management UI: history/favorites/platform, collapsed/first tasks, confirmations, fixed context, gate/cancel/partial results, local rename\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
