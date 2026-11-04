#include "ui/widgets/device_maintenance_panel.h"
#include "ui/icons.h"
#include "ui/theme.h"
#include <QtCore/QDateTime>
#include <QtCore/QFileInfo>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>

namespace {
using A = testtools::MaintenanceAction;
using O = testtools::MaintenanceOutcome;
QString actionName(A action)
{
    switch (action) {
    case A::Logcat:
        return QStringLiteral("导出 Logcat");
    case A::Dmesg:
        return QStringLiteral("导出 dmesg");
    case A::RootRemount:
        return QStringLiteral("Root 与 Remount");
    case A::Reboot:
        return QStringLiteral("重启设备");
    case A::Loader:
        return QStringLiteral("重启至 Loader");
    }
    return {};
}
QString text(const std::string& value)
{
    return testtools::ui::fromUtf8(value);
}
} // namespace
DeviceMaintenancePanel::DeviceMaintenancePanel(
    QWidget* parent, std::shared_ptr<testtools::DeviceMaintenanceService> service)
    : QWidget(parent),
      service_(service ? std::move(service) : testtools::createDeviceMaintenanceService())
{
    setObjectName("deviceMaintenancePanel");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    auto card = [&](const QString& title) {
        auto* frame = new QFrame(this);
        frame->setObjectName("featureCard");
        auto* inner = new QVBoxLayout(frame);
        inner->setContentsMargins(16, 14, 16, 14);
        inner->setSpacing(10);
        auto* heading = new QLabel(title, frame);
        heading->setObjectName("sectionLabel");
        inner->addWidget(heading);
        layout->addWidget(frame);
        return inner;
    };
    auto* logs = card(QStringLiteral("诊断日志"));
    auto* hint = new QLabel(QStringLiteral("保存当前日志到电脑；Logcat 适用于 Android。"), this);
    hint->setObjectName("mutedLabel");
    hint->setWordWrap(true);
    logs->addWidget(hint);
    auto* exports = new QHBoxLayout;
    auto button = [&](A action, const char* name) {
        auto* value = new QPushButton(actionName(action) + QStringLiteral("…"), this);
        value->setObjectName(name);
        connect(value, &QPushButton::clicked, this, [this, action] { start(action); });
        return value;
    };
    logcat_ = button(A::Logcat, "maintenanceLogcat");
    dmesg_ = button(A::Dmesg, "maintenanceDmesg");
    for (auto* value : {logcat_, dmesg_}) {
        value->setIcon(testtools::ui::appIcon(testtools::ui::Icon::Download));
        value->setIconSize({20, 20});
        exports->addWidget(value);
    }
    exports->addStretch();
    logs->addLayout(exports);
    auto* controls = card(QStringLiteral("设备维护"));
    target_ = new QLabel(this);
    target_->setObjectName("maintenanceTarget");
    target_->setWordWrap(true);
    target_->setTextFormat(Qt::PlainText);
    target_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    controls->addWidget(target_);
    auto* actions = new QHBoxLayout;
    root_ = button(A::RootRemount, "maintenanceRoot");
    reboot_ = button(A::Reboot, "maintenanceReboot");
    loader_ = button(A::Loader, "maintenanceLoader");
    root_->setToolTip(QStringLiteral("用于 Android 调试系统；先核验 Root 身份，再尝试 Remount"));
    for (auto* value : {root_, reboot_, loader_})
        actions->addWidget(value);
    actions->addStretch();
    controls->addLayout(actions);
    status_ = new QLabel(QStringLiteral("尚未执行诊断或维护"), this);
    status_->setObjectName("maintenanceStatus");
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    controls->addWidget(status_);
    artifact_ = new QLabel(this);
    artifact_->setObjectName("maintenanceArtifact");
    artifact_->setWordWrap(true);
    artifact_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    artifact_->setTextFormat(Qt::PlainText);
    artifact_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    controls->addWidget(artifact_);
    auto* footer = new QHBoxLayout;
    auto* toggle = new QToolButton(this);
    toggle->setText(QStringLiteral("查看详情"));
    toggle->setCheckable(true);
    toggle->setObjectName("maintenanceDetailsToggle");
    footer->addWidget(toggle);
    footer->addStretch();
    cancel_ = new QPushButton(QStringLiteral("停止任务"), this);
    cancel_->setObjectName("maintenanceCancel");
    footer->addWidget(cancel_);
    controls->addLayout(footer);
    detail_ = new QPlainTextEdit(this);
    detail_->setObjectName("maintenanceDetails");
    detail_->setReadOnly(true);
    detail_->setMinimumHeight(100);
    detail_->setMaximumHeight(160);
    detail_->hide();
    controls->addWidget(detail_);
    connect(toggle, &QToolButton::toggled, detail_, &QWidget::setVisible);
    connect(cancel_, &QPushButton::clicked, this, [this] { cancelTask(service_->snapshot().id); });
    timer_ = new QTimer(this);
    timer_->setInterval(150);
    connect(timer_, &QTimer::timeout, this, &DeviceMaintenancePanel::poll);
    timer_->start();
    updateControls();
}
DeviceMaintenancePanel::~DeviceMaintenancePanel()
{
    service_->cancel();
    tasks_.wait();
}
void DeviceMaintenancePanel::setDevice(const testtools::AdbDevice& device)
{
    device_ = device;
    updateControls();
}
testtools::MaintenanceSnapshot DeviceMaintenancePanel::snapshot() const
{
    return service_->snapshot();
}
void DeviceMaintenancePanel::cancelTask(std::uint64_t id)
{
    service_->cancel(id);
    poll();
}
void DeviceMaintenancePanel::shutdown()
{
    closing_ = true;
    service_->cancel();
    updateControls();
}
void DeviceMaintenancePanel::start(A action)
{
    if (closing_ || service_->snapshot().active || device_.serial.empty() ||
        device_.state != "device" || qEnvironmentVariableIsSet("TESTTOOLS_OFFLINE"))
        return;
    testtools::MaintenanceRequest request;
    request.device = device_;
    request.action = action;
    if (testtools::disruptive(action)) {
        const auto blockers =
            conflicts ? conflicts(request.device) : testtools::MaintenanceConflicts{};
        if (blockers.any()) {
            emit activityChanged(QStringLiteral("请先结束该设备的活动任务，再执行维护"));
            return;
        }
        const auto message =
            action == A::RootRemount
                ? QStringLiteral("将请求 Root，等待原设备恢复后尝试 Remount。可能中断 ADB "
                                 "连接并改变挂载状态。不会自动关闭校验或重启系统。")
            : action == A::Loader
                ? QStringLiteral(
                      "将请求重启至 Loader，当前设备会退出 ADB。需要通过 USB 枚举或实机确认结果。")
                : QStringLiteral(
                      "将立即重启该设备，当前连接会中断。停止等待无法撤销已发出的重启命令。");
        QMessageBox confirm(
            QMessageBox::Warning,
            actionName(action),
            QStringLiteral("设备：%1\n\n%2").arg(text(request.device.serial), message),
            QMessageBox::Cancel,
            this);
        confirm.setObjectName("maintenanceConfirmation");
        confirm.button(QMessageBox::Cancel)->setText(QStringLiteral("取消"));
        auto* apply = confirm.addButton(actionName(action), QMessageBox::AcceptRole);
        confirm.setDefaultButton(QMessageBox::Cancel);
        confirm.setEscapeButton(QMessageBox::Cancel);
        confirm.exec();
        if (confirm.clickedButton() != apply)
            return;
    } else {
        const auto name =
            (action == A::Logcat ? QStringLiteral("logcat-") : QStringLiteral("dmesg-")) +
            QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + QStringLiteral(".log");
        const auto path = QFileDialog::getSaveFileName(
            this,
            actionName(action),
            QStandardPaths::writableLocation(QStandardPaths::DownloadLocation) + "/" + name,
            QStringLiteral("日志文件 (*.log);;所有文件 (*)"));
        if (path.isEmpty())
            return;
        request.output = std::filesystem::path(path.toStdWString());
        // 保存对话框保留系统默认的覆盖确认；未存在的目标不授权覆盖稍后出现的同名文件。
        request.overwrite = QFileInfo::exists(path);
    }
    startRequest(std::move(request));
}
void DeviceMaintenancePanel::startRequest(testtools::MaintenanceRequest request)
{
    if (closing_ || qEnvironmentVariableIsSet("TESTTOOLS_OFFLINE"))
        return;
    if (device_.serial != request.device.serial || device_.state != "device") {
        emit activityChanged(QStringLiteral("设备上下文已变化，未执行原操作"));
        return;
    }
    std::string error;
    const auto blockers = conflicts ? conflicts(request.device) : testtools::MaintenanceConflicts{};
    if (!service_->prepare(std::move(request), blockers, error)) {
        emit activityChanged(text(error));
        return;
    }
    wasActive_ = true;
    updateControls();
    emit taskChanged();
    tasks_.start([service = service_] { service->executePrepared(); });
}
void DeviceMaintenancePanel::updateControls()
{
    const auto state = service_->snapshot();
    const auto blockers = conflicts ? conflicts(device_) : testtools::MaintenanceConflicts{};
    const bool ready = !closing_ && !state.active && !device_.serial.empty() &&
                       device_.state == "device" && !qEnvironmentVariableIsSet("TESTTOOLS_OFFLINE");
    logcat_->setEnabled(ready);
    dmesg_->setEnabled(ready);
    root_->setEnabled(ready && !blockers.any());
    reboot_->setEnabled(ready && !blockers.any());
    const auto conflictHint = QStringLiteral("请先结束该设备的文件任务、设备串口、投屏或服务准备");
    root_->setToolTip(
        blockers.any() ? conflictHint
                       : QStringLiteral("用于 Android 调试系统；先核验 Root 身份，再尝试 Remount"));
    reboot_->setToolTip(blockers.any()
                            ? conflictHint
                            : QStringLiteral("确认后重启当前设备；停止等待不能撤销设备动作"));
    loader_->setVisible(service_->loaderSupported(device_));
    loader_->setEnabled(ready && !blockers.any());
    cancel_->setVisible(state.active);
    cancel_->setEnabled(state.active && !state.cancelling && !closing_);
    cancel_->setText(state.cancelling                              ? QStringLiteral("正在停止…")
                     : testtools::disruptive(state.request.action) ? QStringLiteral("停止等待")
                                                                   : QStringLiteral("取消导出"));
    target_->setText(device_.serial.empty()
                         ? QStringLiteral("未选择设备")
                         : QStringLiteral("当前设备：%1").arg(text(device_.serial)));
    const auto phase = state.active ? state.progress.message : state.result.message;
    if (state.id) {
        status_->setText(QStringLiteral("%1 · %2\n%3")
                             .arg(text(state.request.device.serial),
                                  actionName(state.request.action),
                                  text(phase)));
        if (state.active && !testtools::disruptive(state.request.action))
            status_->setText(status_->text() +
                             QStringLiteral(" · %1 KiB").arg(state.progress.bytes / 1024));
        status_->setStyleSheet(state.active                           ? "color:#87652f;"
                               : state.result.outcome == O::Completed ? "color:#5d715f;"
                               : state.result.outcome == O::Partial   ? "color:#87652f;"
                                                                      : "color:#a34a42;");
    }
    const auto artifact = state.active ? state.progress.artifact : state.result.artifact;
    artifact_->setVisible(!artifact.empty());
    if (!artifact.empty())
        artifact_->setText((!state.active && state.result.outcome == O::Completed
                                ? QStringLiteral("日志文件：")
                                : QStringLiteral("部分日志：")) +
                           QString::fromStdWString(artifact.wstring()));
    const auto details = state.id == 0 ? QStringLiteral("暂无诊断或维护记录")
                                       : QStringLiteral("目标设备：%1\n动作：%2\n%3")
                                             .arg(text(state.request.device.serial),
                                                  actionName(state.request.action),
                                                  text(state.result.detail));
    if (detail_->toPlainText() != details)
        detail_->setPlainText(details);
}
void DeviceMaintenancePanel::poll()
{
    tasks_.reapFinished();
    const auto state = service_->snapshot();
    updateControls();
    if (wasActive_ && !state.active) {
        wasActive_ = false;
        emit activityChanged(text(state.request.device.serial) + QStringLiteral(" · ") +
                             text(state.result.message));
        emit taskChanged();
    }
    if (closing_ && !state.active && tasks_.retainedTaskCount() == 0 && !shutdownEmitted_) {
        shutdownEmitted_ = true;
        timer_->stop();
        emit shutdownFinished();
    }
}
