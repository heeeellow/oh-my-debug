#include "ui/pages/file_transfer_workspace.h"
#include "ui/widgets/file_pane.h"
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
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
    return QString::fromUtf8(value.data(), qsizetype(value.size()));
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
    taskToggle_->setText(QStringLiteral("传输任务"));
    taskToggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    taskToggle_->setCheckable(true);
    taskToggle_->setChecked(true);
    taskToggle_->setArrowType(Qt::DownArrow);
    recordHeader->addWidget(taskToggle_);
    status_ =
        new QLabel(QStringLiteral("选择在线设备并打开目录，选择文件或文件夹后上传 / 下载"), this);
    status_->setObjectName(QStringLiteral("fileTransferStatus"));
    status_->setWordWrap(true);
    status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    recordHeader->addWidget(status_, 1);
    cancel_ = new QPushButton(QStringLiteral("取消传输"), this);
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
                               QStringLiteral("方向"),
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
    pane->dragData = [this, remote] { return dragData(remote); };
    pane->dropData = [this, remote](const QMimeData* mime) { return dropData(remote, mime); };
    return pane;
}

void FileTransferWorkspace::loadLocal()
{
    if (closing_ || service_->snapshot().active)
        return;
    if (localCancel_)
        *localCancel_ = true;
    auto cancel = localCancel_ = std::make_shared<std::atomic<bool>>(false);
    const auto generation = ++localGeneration_;
    localLoading_ = true;
    const auto requested = localPath_->text();
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
                    guard->local_ = result;
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
                    guard->remote_ = result;
                    if (result.error.empty()) {
                        guard->remoteDirectories_[guard->device_.serial] = result.path;
                        if (guard->remotePath_->text().toUtf8().toStdString() == directory)
                            guard->remotePath_->setText(text(result.path));
                        guard->status_->setText(
                            QStringLiteral("已打开设备目录，共 %1 项").arg(result.entries.size()));
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
    ++remoteGeneration_;
    if (remoteCancel_)
        *remoteCancel_ = true;
    remoteLoading_ = false;
    remote_ = {};
    const auto found = remoteDirectories_.find(device.serial);
    remotePath_->setText(found == remoteDirectories_.end() ? QStringLiteral("/")
                                                           : text(found->second));
    remotePane_->clear(QStringLiteral("选择在线设备后打开目录"));
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
    // Modal dialogs run an event loop: recheck that the context has not changed.
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
    // No file-system queries here: the worker's directory listing validates names and types.
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
    shownRecords_ = 0;
    while (records_->topLevelItemCount() + int(lastRequest_.entries.size()) > 500)
        delete records_->takeTopLevelItem(0);
    batchRowOffset_ = std::size_t(records_->topLevelItemCount());
    for (const auto& entry : lastRequest_.entries)
        new QTreeWidgetItem(records_,
                            {text(entry.name),
                             downloading ? QStringLiteral("下载") : QStringLiteral("上传"),
                             QStringLiteral("等待中"),
                             QString()});
    wasActive_ = true;
    updateControls();
    emit activityChanged(QStringLiteral("正在%1 · %2 · %3 项")
                             .arg(downloading ? QStringLiteral("下载") : QStringLiteral("上传"),
                                  text(device_.serial))
                             .arg(service_->snapshot().total));
    tasks_.start([service = service_] { service->executePrepared(); });
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
    uploadTarget_->setText(QStringLiteral("上传到：%1").arg(remotePath_->text()));
    downloadTarget_->setText(QStringLiteral("下载到：%1").arg(localPath_->text()));
}

void FileTransferWorkspace::poll()
{
    tasks_.reapFinished();
    const auto state = service_->snapshot();
    if (entryRefreshPending_ && isVisible() && !closing_ && !state.active && !wasActive_ &&
        !localLoading_ && !remoteLoading_ && device_.state == "device" &&
        !qEnvironmentVariableIsSet("TESTTOOLS_OFFLINE")) {
        entryRefreshPending_ = false;
        loadLocal();
        const auto found = remoteDirectories_.find(device_.serial);
        loadRemote(found == remoteDirectories_.end() ? "/" : found->second);
    }
    if (state.active || wasActive_) {
        while (shownRecords_ < state.records.size()) {
            const auto& record = state.records[shownRecords_++];
            auto* item = records_->topLevelItem(int(batchRowOffset_ + shownRecords_ - 1));
            if (!item)
                item = new QTreeWidgetItem(records_);
            item->setText(0, text(record.name));
            item->setText(1,
                          state.direction == testtools::TransferDirection::Download
                              ? QStringLiteral("下载")
                              : QStringLiteral("上传"));
            item->setText(2, outcome(record.result.outcome));
            item->setText(3, text(record.result.message));
            const auto detail = QStringLiteral("设备：%1\n本地：%2\n设备路径：%3\n%4：%5")
                                    .arg(text(state.serial),
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
            QStringLiteral("传输任务 %1 / %2").arg(state.records.size()).arg(state.total));
        status_->setText(state.active ? QStringLiteral("%1 · %2 · %3/%4\n%5")
                                            .arg(text(state.serial),
                                                 state.cancelling ? QStringLiteral("正在取消…")
                                                                  : text(state.current))
                                            .arg(state.records.size())
                                            .arg(state.total)
                                            .arg(text(state.progress).trimmed())
                                      : text(state.summary));
        if (wasActive_ && !state.active) {
            wasActive_ = false;
            emit activityChanged(
                QStringLiteral("文件传输 · %1 · %2").arg(text(state.serial), text(state.summary)));
            if (!closing_) {
                loadLocal();
                if (device_.serial == state.serial) {
                    entryRefreshPending_ = false;
                    loadRemote();
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
    ++localGeneration_;
    ++remoteGeneration_;
    if (localCancel_)
        *localCancel_ = true;
    if (remoteCancel_)
        *remoteCancel_ = true;
    service_->cancel();
    updateControls();
}
