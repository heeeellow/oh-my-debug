#include "ui/pages/file_transfer_workspace.h"
#include "ui/theme.h"
#include "ui/widgets/file_pane.h"
#include "ui/widgets/file_permissions_dialog.h"
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>
#include <QtCore/QMimeData>
#include <QtCore/QUrl>
#include <QtCore/QUuid>
#include <QtWidgets/QStyle>
#include <QtWidgets/QToolButton>

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QMetaObject>
#include <QtCore/QPointer>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtGui/QShowEvent>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QVBoxLayout>
#include <algorithm>
#include <utility>

namespace {
QString text(const std::string& value)
{
    return testtools::ui::fromUtf8(value);
}
std::filesystem::path path(const QString& value)
{
    return std::filesystem::path(value.toStdWString());
}
QString outcome(testtools::TransferOutcome value)
{
    switch (value) {
    case testtools::TransferOutcome::Completed:
        return QStringLiteral("完成");
    case testtools::TransferOutcome::Skipped:
        return QStringLiteral("跳过");
    case testtools::TransferOutcome::Cancelled:
        return QStringLiteral("取消");
    default:
        return QStringLiteral("失败");
    }
}
QString operationName(testtools::FileOperation operation)
{
    using O = testtools::FileOperation;
    switch (operation) {
    case O::CreateFolder: return QStringLiteral("新建文件夹");
    case O::Rename: return QStringLiteral("重命名");
    case O::Remove: return QStringLiteral("删除");
    case O::SetPermissions: return QStringLiteral("修改权限");
    default: return QStringLiteral("传输");
    }
}
QString taskSummary(const testtools::FileTransferSnapshot& state)
{
    return QStringLiteral("%1 · %2 · %3").arg(state.remote ? text(state.serial) : QStringLiteral("本地"),
        operationName(state.operation), text(state.summary));
}
} // namespace

FileTransferWorkspace::FileTransferWorkspace(
    QWidget* parent, std::shared_ptr<testtools::FileTransferService> service)
    : QWidget(parent),
      service_(service ? std::move(service) : testtools::createFileTransferService())
{
    setObjectName(QStringLiteral("fileTransferWorkspace"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);
    auto* panes = new QSplitter(Qt::Horizontal, this);
    panes->addWidget(buildPane(false));
    panes->addWidget(buildPane(true));
    panes->setChildrenCollapsible(false);
    panes->setHandleWidth(10);
    panes->setStretchFactor(0, 1);
    panes->setStretchFactor(1, 1);
    layout->addWidget(panes, 1);
    auto* recordHeader = new QHBoxLayout;
    dragToken_ = QUuid::createUuid().toString();
    taskToggle_ = new QToolButton(this);
    taskToggle_->setText(QStringLiteral("文件任务"));
    taskToggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    taskToggle_->setCheckable(true);
    taskToggle_->setChecked(false);
    taskToggle_->setArrowType(Qt::RightArrow);
    recordHeader->addWidget(taskToggle_);
    status_ =
        new QLabel(QStringLiteral("选择在线设备并打开目录，选择文件或文件夹后上传 / 下载"), this);
    status_->setObjectName(QStringLiteral("fileTransferStatus"));
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(false);
    status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    recordHeader->addWidget(status_, 1);
    cancel_ = new QPushButton(QStringLiteral("停止任务"), this);
    cancel_->setObjectName(QStringLiteral("fileCancelButton"));
    retry_ = new QPushButton(QStringLiteral("重试失败项"), this);
    retry_->setObjectName(QStringLiteral("fileRetryButton"));
    retry_->setToolTip(QStringLiteral("仅重试当前批次的失败项，沿用原设备、目录和同名处理选择"));
    retry_->setIcon(style()->standardIcon(QStyle::SP_DialogRetryButton));
    recordHeader->addWidget(retry_);
    recordHeader->addWidget(cancel_);
    connect(retry_, &QPushButton::clicked, this, &FileTransferWorkspace::retryFailed);
    layout->addLayout(recordHeader);
    records_ = new QTreeWidget(this);
    records_->setObjectName(QStringLiteral("fileTransferRecords"));
    records_->setHeaderLabels({QStringLiteral("文件名"),
                               QStringLiteral("操作"),
                               QStringLiteral("状态"),
                               QStringLiteral("详情")});
    records_->setRootIsDecorated(false);
    records_->setUniformRowHeights(true);
    records_->setMinimumHeight(105);
    records_->setMaximumHeight(140);
    records_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    records_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    records_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    records_->header()->setStretchLastSection(true);
    layout->addWidget(records_);
    records_->hide();
    connect(taskToggle_, &QToolButton::toggled, this, [this](bool open) {
        records_->setVisible(open);
        taskToggle_->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
    });
    connect(cancel_, &QPushButton::clicked, this, [this] {
        service_->cancel();
        updateControls();
    });
    connect(records_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
        QMessageBox::information(this, QStringLiteral("文件传输详情"), item->toolTip(0));
    });
    auto desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    if (desktop.isEmpty())
        desktop = QDir::homePath();
    localPath_->setText(QDir::toNativeSeparators(desktop));
    remotePath_->setText(QStringLiteral("/"));
    loadLocations();
    updateRemoteLocations();
    timer_ = new QTimer(this);
    timer_->setInterval(150);
    connect(timer_, &QTimer::timeout, this, &FileTransferWorkspace::poll);
    timer_->start();
    loadLocal();
    updateControls();
}

FileTransferWorkspace::~FileTransferWorkspace()
{
    closing_ = true;
    service_->cancel();
    if (localCancel_)
        *localCancel_ = true;
    if (remoteCancel_)
        *remoteCancel_ = true;
    tasks_.wait();
}

QWidget* FileTransferWorkspace::buildPane(bool remote)
{
    auto* pane = new FilePane(remote, this);
    if (remote) {
        remotePane_ = pane;
        remotePath_ = pane->pathEdit();
        download_ = pane->transferButton();
        downloadTarget_ = pane->targetLabel();
    } else {
        localPane_ = pane;
        localPath_ = pane->pathEdit();
        upload_ = pane->transferButton();
        uploadTarget_ = pane->targetLabel();
    }
    pane->openRequested = [this, remote] { remote ? loadRemote() : loadLocal(); };
    pane->selectionChanged = [this] { updateControls(); };
    pane->transferRequested = [this, remote] {
        transfer(remote ? testtools::TransferDirection::Download
                        : testtools::TransferDirection::Upload);
    };
    pane->changeRequested = [this, remote](testtools::FileOperation op) { changeFiles(remote, op); };
    if (remote) {
        pane->directoryVisited = [this](const QString& directory) {
            if (device_.serial.empty()) return;
            auto& location = locations_[text(device_.serial)];
            location.used = QDateTime::currentMSecsSinceEpoch();
            location.recent.removeAll(directory); location.recent.prepend(directory);
            while (location.recent.size() > 20) location.recent.removeLast();
            if (!remote_.platform.empty()) location.platform = text(remote_.platform);
            saveLocations(); updateRemoteLocations();
        };
        pane->favoriteRequested = [this](const QString& directory) {
            if (device_.serial.empty() || directory.isEmpty()) return;
            auto& location = locations_[text(device_.serial)];
            if (location.favorites.contains(directory)) location.favorites.removeAll(directory);
            else if (location.favorites.size() < 20) location.favorites.append(directory);
            else { emit activityChanged(QStringLiteral("每台设备最多收藏 20 个目录")); return; }
            location.used = QDateTime::currentMSecsSinceEpoch();
            saveLocations(); updateRemoteLocations();
        };
    }
    pane->dragData = [this, remote] { return dragData(remote); };
    pane->dropData = [this, remote](const QMimeData* mime) { return dropData(remote, mime); };
    return pane;
}

void FileTransferWorkspace::loadLocal(const QString& requestedDirectory)
{
    if (closing_ || service_->snapshot().active)
        return;
    if (localCancel_)
        *localCancel_ = true;
    auto cancel = localCancel_ = std::make_shared<std::atomic<bool>>(false);
    const auto generation = ++localGeneration_;
    localLoading_ = true;
    const auto requested = requestedDirectory.isEmpty() ? localPath_->text() : requestedDirectory;
    const auto directory = path(requested);
    QPointer<FileTransferWorkspace> guard(this);
    updateControls();
    tasks_.start([guard, cancel, generation, directory, requested] {
        const auto result = testtools::FileTransferService::listLocal(directory, *cancel);
        if (guard)
            QMetaObject::invokeMethod(
                guard,
                [guard, generation, directory, requested, result] {
                    if (!guard || guard->closing_ || generation != guard->localGeneration_)
                        return;
                    guard->localLoading_ = false;
                    if (result.error.empty()) guard->local_ = result;
                    if (result.error.empty()) {
                        if (path(guard->localPath_->text()) == directory)
                            guard->localPath_->setText(QDir::toNativeSeparators(text(result.path)));
                    } else {
                        guard->status_->setText(text(result.error));
                        emit guard->activityChanged(text(result.error));
                    }
                    guard->localPane_->applyListing(result, requested);
                    if (!guard->pendingDropNames_.isEmpty()) {
                        const auto names = std::exchange(guard->pendingDropNames_, {});
                        if (result.error.empty() &&
                            guard->device_.serial == guard->pendingDropSerial_ &&
                            guard->remote_.path == guard->pendingDropRemote_ &&
                            path(guard->pendingDropLocal_) == directory &&
                            path(guard->localPath_->text()) == directory) {
                            guard->localPane_->selectNames(names);
                            if (guard->localPane_->selectedEntries().size() ==
                                std::size_t(names.size()))
                                guard->transfer(testtools::TransferDirection::Upload);
                            else
                                guard->status_->setText(
                                    QStringLiteral("拖入的项目已变化，请重新选择"));
                        }
                    }
                    guard->updateControls();
                },
                Qt::QueuedConnection);
    });
}

void FileTransferWorkspace::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    refreshOnEntry();
}

void FileTransferWorkspace::refreshOnEntry()
{
    if (!closing_)
        entryRefreshPending_ = true;
}

void FileTransferWorkspace::loadRemote(const std::string& requestedDirectory)
{
    if (closing_ || service_->snapshot().active || qEnvironmentVariableIsSet("TESTTOOLS_OFFLINE"))
        return;
    if (requestedDirectory.empty())
        entryRefreshPending_ = false;
    if (remoteCancel_)
        *remoteCancel_ = true;
    auto cancel = remoteCancel_ = std::make_shared<std::atomic<bool>>(false);
    const auto generation = ++remoteGeneration_;
    remoteLoading_ = true;
    const auto directory = requestedDirectory.empty() ? remotePath_->text().toUtf8().toStdString()
                                                      : requestedDirectory;
    const auto device = device_;
    QPointer<FileTransferWorkspace> guard(this);
    updateControls();
    tasks_.start([guard, service = service_, cancel, generation, directory, device] {
        const auto result = service->listRemote(device, directory, *cancel);
        if (guard)
            QMetaObject::invokeMethod(
                guard,
                [guard, generation, directory, result] {
                    if (!guard || guard->closing_ || generation != guard->remoteGeneration_)
                        return;
                    guard->remoteLoading_ = false;
                    if (result.error.empty()) guard->remote_ = result;
                    if (result.error.empty()) {
                        guard->remoteDirectories_[guard->device_.serial] = result.path;
                        if (guard->remotePath_->text().toUtf8().toStdString() == directory)
                            guard->remotePath_->setText(text(result.path));
                        const auto state = guard->service_->snapshot();
                        guard->status_->setText(guard->hasStartedTask_ && !state.records.empty()
                            ? taskSummary(state) : QStringLiteral("已打开设备目录，共 %1 项").arg(result.entries.size()));
                    } else
                        guard->status_->setText(text(result.error));
                    emit guard->activityChanged(guard->status_->text());
                    guard->remotePane_->applyListing(result, text(directory));
                    guard->updateControls();
                },
                Qt::QueuedConnection);
    });
}

void FileTransferWorkspace::setDevice(const serialdemo::AdbDevice& device)
{
    if (device_.serial == device.serial && device_.state == device.state)
        return;
    device_ = device;
    if (permissionsDialog_) permissionsDialog_->setReadError(QStringLiteral("设备已变化，请关闭后重新操作"));
    ++remoteGeneration_;
    if (remoteCancel_)
        *remoteCancel_ = true;
    remoteLoading_ = false;
    remote_ = {};
    const auto found = remoteDirectories_.find(device.serial);
    remotePath_->setText(found == remoteDirectories_.end() ? QStringLiteral("/")
                                                           : text(found->second));
    remotePane_->clear(QStringLiteral("选择在线设备后打开目录"));
    updateRemoteLocations();
    if (!service_->snapshot().active)
        status_->setText(device.state == "device"
                             ? QStringLiteral("设备已切换，请打开目标目录")
                             : QStringLiteral("请先选择在线且已授权的 ADB 设备"));
    updateControls();
    if (isVisible())
        refreshOnEntry();
}

void FileTransferWorkspace::transfer(testtools::TransferDirection direction)
{
    if (closing_ || service_->snapshot().active || qEnvironmentVariableIsSet("TESTTOOLS_OFFLINE"))
        return;
    if (localLoading_ || remoteLoading_ || local_.path.empty() || remote_.path.empty() ||
        !local_.error.empty() || !remote_.error.empty() || device_.state != "device" ||
        QDir::cleanPath(QDir::fromNativeSeparators(localPath_->text())) !=
            QDir::cleanPath(QDir::fromNativeSeparators(text(local_.path))) ||
        remotePath_->text() != text(remote_.path)) {
        status_->setText(QStringLiteral("请先打开两端目录，再选择文件传输"));
        return;
    }
    const bool downloading = direction == testtools::TransferDirection::Download;
    auto* pane = downloading ? remotePane_ : localPane_;
    const auto& destination = downloading ? local_.entries : remote_.entries;
    testtools::FileTransferRequest request;
    request.device = device_;
    request.direction = direction;
    request.localDirectory = std::filesystem::u8path(local_.path);
    request.remoteDirectory = remote_.path;
    bool collision = false;
    for (const auto& entry : pane->selectedEntries()) {
        request.entries.push_back(entry);
        for (const auto& target : destination)
            if (text(target.name)
                    .compare(text(entry.name),
                             downloading ? Qt::CaseInsensitive : Qt::CaseSensitive) == 0)
                collision = true;
    }
    if (collision) {
        QMessageBox question(
            QMessageBox::Question,
            QStringLiteral("同名文件处理"),
            QStringLiteral(
                "目标已有同名项目。是否覆盖普通文件？已有同名文件夹和链接会跳过，首版不合并目录。"),
            QMessageBox::NoButton,
            this);
        auto* overwrite =
            question.addButton(QStringLiteral("覆盖同名文件"), QMessageBox::AcceptRole);
        auto* skip = question.addButton(QStringLiteral("跳过同名项"), QMessageBox::RejectRole);
        auto* cancel = question.addButton(QStringLiteral("取消"), QMessageBox::DestructiveRole);
        question.setDefaultButton(skip);
        question.exec();
        if (question.clickedButton() == cancel || !question.clickedButton())
            return;
        request.overwrite = question.clickedButton() == overwrite;
    }
    // 模态对话框运行期间会嵌套事件循环，返回后需再次确认设备与目录上下文未发生变更。
    if (closing_ || service_->snapshot().active)
        return;
    if (request.device.serial != device_.serial || request.device.state != device_.state ||
        request.remoteDirectory != remotePath_->text().toUtf8().toStdString() ||
        request.localDirectory != path(localPath_->text())) {
        status_->setText(QStringLiteral("设备或目录已变化，请重新选择文件"));
        return;
    }
    startRequest(std::move(request));
}

QMimeData* FileTransferWorkspace::dragData(bool remote)
{
    if (closing_ || service_->snapshot().active || localLoading_ || remoteLoading_)
        return nullptr;
    auto* pane = remote ? remotePane_ : localPane_;
    if (pane->loadedPath().isEmpty() || pane->selectedEntries().empty())
        return nullptr;
    QJsonArray names;
    for (const auto& entry : pane->selectedEntries())
        names.append(text(entry.name));
    auto* mime = new QMimeData;
    mime->setData("application/x-testtools-files",
                  QJsonDocument(QJsonObject{{"token", dragToken_},
                                            {"remote", remote},
                                            {"directory", pane->loadedPath()},
                                            {"serial", text(device_.serial)},
                                            {"names", names}})
                      .toJson(QJsonDocument::Compact));
    return mime;
}

bool FileTransferWorkspace::dropData(bool remote, const QMimeData* mime)
{
    if (closing_ || service_->snapshot().active || localLoading_ || remoteLoading_ ||
        device_.state != "device" || remote_.path.empty() || local_.path.empty())
        return false;
    if (mime->hasFormat("application/x-testtools-files")) {
        const auto data =
            QJsonDocument::fromJson(mime->data("application/x-testtools-files")).object();
        const bool sourceRemote = data.value("remote").toBool();
        auto* source = sourceRemote ? remotePane_ : localPane_;
        if (data.value("token").toString() != dragToken_ || sourceRemote == remote ||
            data.value("directory").toString() != source->loadedPath() ||
            data.value("serial").toString() != text(device_.serial))
            return false;
        QStringList names;
        for (const auto& value : data.value("names").toArray())
            names << value.toString();
        source->selectNames(names);
        transfer(sourceRemote ? testtools::TransferDirection::Download
                              : testtools::TransferDirection::Upload);
        return true;
    }
    if (!remote || !mime->hasUrls())
        return false;
    QString parent;
    QStringList names;
    // GUI 线程不直接做文件系统状态探测，交由后台目录列举统一校验名称与条目类型。
    for (const auto& url : mime->urls()) {
        if (!url.isLocalFile())
            return false;
        const auto file = QDir::cleanPath(url.toLocalFile());
        const auto slash = file.lastIndexOf('/');
        auto directory = file.left(slash);
        if (slash == 2 && file[1] == QLatin1Char(':'))
            directory += QLatin1Char('/');
        if (!parent.isEmpty() && parent.compare(directory, Qt::CaseInsensitive) != 0) {
            status_->setText(QStringLiteral("请每次拖入同一目录下的文件或文件夹"));
            return false;
        }
        parent = directory;
        names << file.section('/', -1);
    }
    if (names.isEmpty() || names.size() > 256)
        return false;
    pendingDropNames_ = names;
    pendingDropLocal_ = parent;
    pendingDropSerial_ = device_.serial;
    pendingDropRemote_ = remote_.path;
    localPath_->setText(QDir::toNativeSeparators(parent));
    loadLocal();
    return true;
}

void FileTransferWorkspace::retryFailed()
{
    const auto snapshot = service_->snapshot();
    if (lastWasChange_) {
        if (snapshot.active || closing_ || (lastChange_.remote && (device_.state != "device" || lastChange_.device.serial != device_.serial))) return;
        auto request = lastChange_; request.entries.clear();
        for (const auto& record : snapshot.records) if (record.result.outcome == testtools::TransferOutcome::Failed)
            for (const auto& entry : lastChange_.entries) if (entry.name == record.name) request.entries.push_back(entry);
        if (!request.entries.empty()) startChange(std::move(request));
        return;
    }
    if (snapshot.active || closing_ || device_.state != "device" ||
        lastRequest_.device.serial != device_.serial)
        return;
    auto request = lastRequest_;
    request.entries.clear();
    for (const auto& record : snapshot.records) {
        if (record.result.outcome != testtools::TransferOutcome::Failed)
            continue;
        const auto found =
            std::find_if(lastRequest_.entries.begin(),
                         lastRequest_.entries.end(),
                         [&](const auto& entry) { return entry.name == record.name; });
        if (found != lastRequest_.entries.end())
            request.entries.push_back(*found);
    }
    if (!request.entries.empty())
        startRequest(std::move(request));
}

void FileTransferWorkspace::startRequest(testtools::FileTransferRequest request)
{
    const bool downloading = request.direction == testtools::TransferDirection::Download;
    std::string error;
    lastRequest_ = request;
    if (!service_->prepare(std::move(request), error)) {
        status_->setText(text(error));
        return;
    }
    lastWasChange_ = false;
    beginTaskRows(lastRequest_.entries, downloading ? QStringLiteral("下载") : QStringLiteral("上传"));
    emit activityChanged(QStringLiteral("正在%1 · %2 · %3 项")
                             .arg(downloading ? QStringLiteral("下载") : QStringLiteral("上传"), text(device_.serial))
                             .arg(service_->snapshot().total));
    tasks_.start([service = service_] { service->executePrepared(); });
}

void FileTransferWorkspace::beginTaskRows(const std::vector<testtools::FileEntry>& entries, const QString& action)
{
    shownRecords_ = 0;
    while (records_->topLevelItemCount() + int(entries.size()) > 500)
        delete records_->takeTopLevelItem(0);
    batchRowOffset_ = std::size_t(records_->topLevelItemCount());
    for (const auto& entry : entries)
        new QTreeWidgetItem(records_,
                            {text(entry.name),
                             action,
                             QStringLiteral("等待中"),
                             QString()});
    wasActive_ = true;
    if (!hasStartedTask_) { hasStartedTask_ = true; taskToggle_->setChecked(true); }
    updateControls();
}

void FileTransferWorkspace::updateControls()
{
    if (!status_ || !upload_ || !download_ || !localPath_ || !remotePath_)
        return;
    const auto state = service_->snapshot();
    const bool online = !device_.serial.empty() && device_.state == "device" &&
                        !qEnvironmentVariableIsSet("TESTTOOLS_OFFLINE");
    localPane_->setAvailable(!closing_ && !state.active, localLoading_);
    remotePane_->setAvailable(!closing_ && !state.active && online, remoteLoading_);
    const bool ready = !closing_ && !state.active && online && !localLoading_ && !remoteLoading_ &&
                       !local_.path.empty() && local_.error.empty() && !remote_.path.empty() &&
                       remote_.error.empty() &&
                       QDir::cleanPath(QDir::fromNativeSeparators(localPath_->text())) ==
                           QDir::cleanPath(QDir::fromNativeSeparators(text(local_.path))) &&
                       remotePath_->text() == text(remote_.path);
    upload_->setEnabled(ready && !localPane_->selectedEntries().empty());
    download_->setEnabled(ready && !remotePane_->selectedEntries().empty());
    const bool failed =
        std::any_of(state.records.begin(), state.records.end(), [](const auto& record) {
            return record.result.outcome == testtools::TransferOutcome::Failed;
        });
    retry_->setEnabled(!closing_ && !state.active && online && failed &&
                       lastRequest_.device.serial == device_.serial);
    cancel_->setEnabled(state.active && !state.cancelling && !closing_);
    const auto localReady = !closing_ && !state.active && !localLoading_ && !local_.path.empty() &&
        QDir::cleanPath(QDir::fromNativeSeparators(localPath_->text())) == QDir::cleanPath(text(local_.path));
    const auto remoteReady = !closing_ && !state.active && !remoteLoading_ && online && !remote_.path.empty() && remotePath_->text() == text(remote_.path);
    localPane_->setManagementAvailable(localReady);
    remotePane_->setManagementAvailable(remoteReady);
    if (lastWasChange_) retry_->setEnabled(!closing_ && !state.active && failed &&
        (!lastChange_.remote || (online && lastChange_.device.serial == device_.serial)));
    uploadTarget_->setText(QStringLiteral("上传到：%1").arg(remote_.path.empty() ? QStringLiteral("先打开设备目录") : text(remote_.path)));
    downloadTarget_->setText(QStringLiteral("下载到：%1").arg(local_.path.empty() ? QStringLiteral("先打开本地目录") : text(local_.path)));
    uploadTarget_->setToolTip(uploadTarget_->text()); downloadTarget_->setToolTip(downloadTarget_->text());
}

void FileTransferWorkspace::poll()
{
    tasks_.reapFinished();
    const auto state = service_->snapshot();
    if (entryRefreshPending_ && isVisible() && !closing_ && !state.active && !wasActive_ &&
        !localLoading_ && !remoteLoading_) {
        entryRefreshPending_ = false;
        loadLocal(local_.path.empty() ? QString() : text(local_.path));
        if (device_.state == "device" && !qEnvironmentVariableIsSet("TESTTOOLS_OFFLINE")) {
            const auto found = remoteDirectories_.find(device_.serial);
            loadRemote(found == remoteDirectories_.end() ? "/" : found->second);
        }
    }
    if (state.active || wasActive_) {
        while (shownRecords_ < state.records.size()) {
            const auto& record = state.records[shownRecords_++];
            auto* item = records_->topLevelItem(int(batchRowOffset_ + shownRecords_ - 1));
            if (!item)
                item = new QTreeWidgetItem(records_);
            item->setText(0, text(record.name));
            item->setText(1,
                          state.operation != testtools::FileOperation::Transfer ? operationName(state.operation) : state.direction == testtools::TransferDirection::Download
                              ? QStringLiteral("下载")
                              : QStringLiteral("上传"));
            item->setText(2, outcome(record.result.outcome));
            item->setText(3, text(record.result.message));
            const auto detail = QStringLiteral("对象：%1\n本地：%2\n设备路径：%3\n%4：%5")
                                    .arg(state.serial.empty() ? QStringLiteral("本地文件") : text(state.serial),
                                         text(record.local),
                                         text(record.remote),
                                         outcome(record.result.outcome),
                                         text(record.result.message));
            for (int column = 0; column < 4; ++column)
                item->setToolTip(column, detail);
            while (records_->topLevelItemCount() > 500)
                delete records_->takeTopLevelItem(0);
        }
        if (state.active &&
            batchRowOffset_ + shownRecords_ < std::size_t(records_->topLevelItemCount()))
            records_->topLevelItem(int(batchRowOffset_ + shownRecords_))
                ->setText(2,
                          state.cancelling ? QStringLiteral("取消中") : QStringLiteral("进行中"));
        taskToggle_->setText(
            QStringLiteral("文件任务 %1 / %2").arg(state.records.size()).arg(state.total));
        status_->setText(state.active ? QStringLiteral("%1 · %2 · %3/%4 · %5")
                                            .arg(state.serial.empty() ? QStringLiteral("本地") : text(state.serial),
                                                 state.cancelling ? QStringLiteral("正在取消…")
                                                                  : text(state.current))
                                            .arg(state.records.size())
                                            .arg(state.total)
                                            .arg(text(state.progress).simplified())
                                      : taskSummary(state));
        status_->setToolTip(status_->text());
        if (wasActive_ && !state.active) {
            wasActive_ = false;
            emit activityChanged(taskSummary(state));
            if (!closing_ && state.operation != testtools::FileOperation::Transfer) {
                if (!state.remote && local_.path == state.parent) loadLocal(text(state.parent));
                if (state.remote && device_.serial == state.serial && remote_.path == state.parent) loadRemote(state.parent);
            } else if (!closing_) {
                loadLocal(local_.path.empty() ? QString() : text(local_.path));
                if (device_.serial == state.serial) {
                    entryRefreshPending_ = false;
                    const auto found = remoteDirectories_.find(device_.serial);
                    loadRemote(found == remoteDirectories_.end() ? "/" : found->second);
                }
            }
        }
        updateControls();
    }
    if (closing_ && !state.active && tasks_.retainedTaskCount() == 0) {
        timer_->stop();
        emit shutdownFinished();
    }
}

void FileTransferWorkspace::shutdown()
{
    if (closing_)
        return;
    closing_ = true;
    if (permissionsDialog_) permissionsDialog_->reject();
    ++localGeneration_;
    ++remoteGeneration_;
    if (localCancel_)
        *localCancel_ = true;
    if (remoteCancel_)
        *remoteCancel_ = true;
    service_->cancel();
    updateControls();
}

testtools::FileTransferSnapshot FileTransferWorkspace::transferSnapshot() const { return service_->snapshot(); }

void FileTransferWorkspace::changeFiles(bool remote, testtools::FileOperation operation)
{
    using O = testtools::FileOperation;
    auto* pane = remote ? remotePane_ : localPane_;
    const auto& listing = remote ? remote_ : local_;
    const auto draft = remote ? remotePath_->text() : QDir::fromNativeSeparators(localPath_->text());
    if (closing_ || service_->snapshot().active || (remote ? remoteLoading_ : localLoading_) ||
        listing.path.empty() || QDir::cleanPath(draft) != QDir::cleanPath(text(listing.path)) ||
        (remote && (device_.state != "device" || qEnvironmentVariableIsSet("TESTTOOLS_OFFLINE")))) return;
    testtools::FileManagementRequest request;
    request.operation = operation; request.remote = remote; request.device = device_; request.parent = listing.path;
    request.entries = pane->selectedEntries();
    if (operation == O::CreateFolder || operation == O::Rename) {
        if (operation == O::Rename && request.entries.size() != 1) return;
        QInputDialog prompt(this);
        prompt.setWindowTitle(operationName(operation));
        prompt.setLabelText(QStringLiteral("名称（同目录，不覆盖已有项目）"));
        prompt.setTextValue(operation == O::CreateFolder ? QStringLiteral("新建文件夹") : text(request.entries[0].name));
        prompt.setOkButtonText(operation == O::CreateFolder ? QStringLiteral("创建") : QStringLiteral("重命名"));
        prompt.setCancelButtonText(QStringLiteral("取消"));
        if (prompt.exec() != QDialog::Accepted) return;
        const auto name = prompt.textValue();
        if (operation == O::CreateFolder) request.entries = {{testtools::ui::toUtf8(name), testtools::FileKind::Directory}};
        else request.newName = testtools::ui::toUtf8(name);
    } else if (operation == O::SetPermissions) {
        if (!remote || request.entries.empty() || request.entries.size() > 256 ||
            std::any_of(request.entries.begin(), request.entries.end(), [](const auto& e) {
                return e.kind != testtools::FileKind::Regular && e.kind != testtools::FileKind::Directory;
            })) return;
        FilePermissionsDialog prompt(text(request.device.serial), text(request.parent), request.entries, this);
        permissionsDialog_ = &prompt;
        // 模态窗口保持响应；权限重新读取复用受持有、可取消的目录任务与代次检查。
        const auto cancel = std::make_shared<std::atomic<bool>>(false);
        remoteCancel_ = cancel; remoteLoading_ = true;
        const auto generation = ++remoteGeneration_;
        QPointer<FileTransferWorkspace> guard(this);
        QPointer<FilePermissionsDialog> dialog(&prompt);
        updateControls();
        tasks_.start([guard, dialog, service = service_, cancel, generation, request] {
            const auto result = service->listRemote(request.device, request.parent, *cancel);
            if (!guard) return;
            QMetaObject::invokeMethod(guard, [guard, dialog, generation, request, result] {
                if (!guard || !dialog) return;
                if (guard->closing_ || generation != guard->remoteGeneration_ || guard->device_.serial != request.device.serial) {
                    dialog->setReadError(QStringLiteral("设备或目录已变化，请关闭后重新操作")); return;
                }
                guard->remoteLoading_ = false; guard->updateControls();
                if (!result.error.empty()) { dialog->setReadError(text(result.error)); return; }
                auto selected = request.entries;
                for (auto& entry : selected) {
                    const auto found = std::find_if(result.entries.begin(), result.entries.end(), [&](const auto& e) { return e.name == entry.name; });
                    if (found == result.entries.end() || found->kind != entry.kind) {
                        dialog->setReadError(QStringLiteral("所选项目已消失或类型发生变化，请刷新目录")); return;
                    }
                    entry = *found;
                }
                dialog->setEntries(std::move(selected));
            }, Qt::QueuedConnection);
        });
        const bool accepted = prompt.exec() == QDialog::Accepted;
        permissionsDialog_.clear();
        *cancel = true;
        if (generation == remoteGeneration_) { ++remoteGeneration_; remoteLoading_ = false; updateControls(); }
        if (!accepted || prompt.permissions().empty() || prompt.entries().size() != request.entries.size()) return;
        request.entries = prompt.entries(); request.permissions = prompt.permissions();
    } else if (operation == O::Remove) {
        if (request.entries.empty()) return;
        QStringList names; for (const auto& e : request.entries) names.append(text(e.name));
        QMessageBox confirm(QMessageBox::Warning, QStringLiteral("删除所选项目"),
            QStringLiteral("对象：%1\n目录：%2\n将直接删除 %3 个所选项目及目录内容。\n%4\n此操作不进入回收站，取消不会恢复已删除项目。")
                .arg(remote ? text(request.device.serial) : QStringLiteral("本地文件"), text(request.parent))
                .arg(names.size()).arg(names.mid(0, 8).join(QStringLiteral("、"))),
            QMessageBox::Yes | QMessageBox::No, this);
        confirm.setDefaultButton(QMessageBox::No);
        confirm.setTextFormat(Qt::PlainText);
        confirm.setDetailedText(names.join(QLatin1Char('\n')));
        confirm.button(QMessageBox::Yes)->setText(QStringLiteral("删除"));
        confirm.button(QMessageBox::No)->setText(QStringLiteral("取消"));
        for (auto* button : confirm.findChildren<QPushButton*>())
            if (button != confirm.button(QMessageBox::Yes) && button != confirm.button(QMessageBox::No)) {
                button->setText(QStringLiteral("完整名单…"));
                connect(button, &QPushButton::clicked, &confirm, [button, expanded = false]() mutable {
                    expanded = !expanded;
                    button->setText(expanded ? QStringLiteral("收起名单") : QStringLiteral("完整名单…"));
                });
            }
        if (confirm.exec() != QMessageBox::Yes) return;
    }
    // 确认框会处理其他事件；只执行用户刚确认的原设备、目录和选择快照。
    const auto& current = remote ? remote_ : local_;
    const auto currentDraft = remote ? remotePath_->text() : QDir::fromNativeSeparators(localPath_->text());
    if (closing_ || service_->snapshot().active || current.path != request.parent ||
        QDir::cleanPath(currentDraft) != QDir::cleanPath(text(request.parent)) ||
        (remote && (device_.serial != request.device.serial || device_.state != "device"))) {
        status_->setText(QStringLiteral("设备、目录或任务已变化，请重新操作")); return;
    }
    startChange(std::move(request));
}

void FileTransferWorkspace::startChange(testtools::FileManagementRequest request)
{
    std::string error;
    if (!service_->prepareChange(request, error)) { status_->setText(text(error)); return; }
    lastChange_ = std::move(request); lastWasChange_ = true;
    beginTaskRows(lastChange_.entries, operationName(lastChange_.operation));
    emit activityChanged(QStringLiteral("%1 · %2 · %3").arg(
        lastChange_.remote ? text(lastChange_.device.serial) : QStringLiteral("本地文件"),
        operationName(lastChange_.operation), text(lastChange_.parent)));
    tasks_.start([service = service_] { service->executePrepared(); });
}

void FileTransferWorkspace::loadLocations()
{
    const auto configured = qEnvironmentVariable("TESTTOOLS_DATA_DIR");
    const auto root = configured.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) : configured;
    locationsPath_ = QDir(root).filePath(QStringLiteral("file-locations.json"));
    QFile file(locationsPath_);
    if (!file.exists()) return;
    QJsonParseError parse;
    if (!file.open(QIODevice::ReadOnly) || file.size() > 2 * 1024 * 1024) {
        locationsWritable_ = false; status_->setText(QStringLiteral("无法读取目录偏好，保留原文件")); return;
    }
    const auto doc = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject() || doc.object().value("schema_version").toInt() != 1 ||
        !doc.object().value("devices").isArray() || doc.object().value("devices").toArray().size() > 16) {
        locationsWritable_ = false; status_->setText(QStringLiteral("目录偏好格式无效，保留原文件")); return;
    }
    const auto paths = [](const QJsonValue& value) {
        QStringList result;
        for (const auto& item : value.toArray()) {
            const auto raw = item.toString();
            const auto normalized = testtools::FileTransferService::normalizeRemoteDirectory(testtools::ui::toUtf8(raw));
            if (normalized.empty() || text(normalized) != raw || result.contains(raw)) continue;
            result.append(raw); if (result.size() == 20) break;
        }
        return result;
    };
    for (const auto& item : doc.object().value("devices").toArray()) {
        const auto object = item.toObject();
        const auto serial = object.value("serial").toString();
        if (serial.isEmpty() || serial.size() > 256) continue;
        Locations location;
        location.platform = object.value("platform").toString();
        location.recent = paths(object.value("recent")); location.favorites = paths(object.value("favorites"));
        location.used = qint64(object.value("used").toDouble());
        locations_.insert(serial, location);
        if (!location.recent.isEmpty()) remoteDirectories_[testtools::ui::toUtf8(serial)] = testtools::ui::toUtf8(location.recent.front());
    }
}

void FileTransferWorkspace::saveLocations()
{
    auto keys = locations_.keys();
    std::sort(keys.begin(), keys.end(), [this](const auto& a, const auto& b) { return locations_[a].used > locations_[b].used; });
    while (keys.size() > 16) locations_.remove(keys.takeLast());
    if (!locationsWritable_) return;
    QJsonArray devices;
    for (const auto& serial : keys) {
        const auto& value = locations_[serial];
        devices.append(QJsonObject{{"serial", serial}, {"platform", value.platform}, {"used", double(value.used)},
            {"recent", QJsonArray::fromStringList(value.recent)}, {"favorites", QJsonArray::fromStringList(value.favorites)}});
    }
    const auto bytes = QJsonDocument(QJsonObject{{"schema_version", 1}, {"devices", devices}}).toJson(QJsonDocument::Compact);
    QSaveFile file(locationsPath_);
    if (bytes.size() > 2 * 1024 * 1024 || !QDir().mkpath(QFileInfo(locationsPath_).absolutePath()) ||
        !file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        emit activityChanged(QStringLiteral("目录偏好保存失败，本次会话仍可使用"));
}

void FileTransferWorkspace::updateRemoteLocations()
{
    const auto saved = locations_.value(text(device_.serial));
    QStringList presets{"/"};
    if (saved.platform == "android") presets << "/sdcard/oem_log" << "/sdcard" << "/data/local/tmp" << "/vendor/etc";
    else if (saved.platform == "rockchip_linux") presets << "/userdata";
    presets << "/proc";
    remotePane_->setLocations(presets, saved.recent, saved.favorites);
}

void FileTransferWorkspace::cancelTransfer(std::uint64_t expectedId)
{
    service_->cancel(expectedId);
    updateControls();
}
