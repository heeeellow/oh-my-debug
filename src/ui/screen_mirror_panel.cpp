#include "ui/screen_mirror_panel.h"

#include <QtCore/QDateTime>
#include <QtCore/QMetaObject>
#include <QtCore/QPointer>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMenu>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QStyle>
#include <QtWidgets/QVBoxLayout>
#include <exception>

namespace {
QString text(const std::string& value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}
} // namespace

ScreenMirrorPanel::ScreenMirrorPanel(QWidget* parent,
                                     std::shared_ptr<testtools::ScreenMirrorService> service,
                                     bool embedded)
    : QDialog(parent),
      service_(service ? std::move(service) : testtools::createScreenMirrorService())
{
    if (embedded)
        setWindowFlags(Qt::Widget);
    setWindowTitle(QStringLiteral("Android 投屏"));
    setObjectName(QStringLiteral("screenMirrorPanel"));
    setMinimumWidth(430);
    resize(480, 355);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    target_ = new QLabel(this);
    target_->setWordWrap(true);
    target_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    target_->setObjectName(QStringLiteral("mirrorTarget"));
    status_ = new QLabel(QStringLiteral("尚未投屏"), this);
    status_->setWordWrap(true);
    status_->setObjectName(QStringLiteral("mirrorStatus"));
    layout->addWidget(target_);
    layout->addWidget(status_);
    auto* session = new QHBoxLayout;
    start_ = new QPushButton(QStringLiteral("开始投屏"), this);
    start_->setObjectName(QStringLiteral("mirrorStartButton"));
    stop_ = new QPushButton(QStringLiteral("停止投屏"), this);
    stop_->setObjectName(QStringLiteral("mirrorStopButton"));
    session->addWidget(start_);
    session->addWidget(stop_);
    session->addStretch();
    layout->addLayout(session);
    connect(start_, &QPushButton::clicked, this, &ScreenMirrorPanel::startCurrentDevice);
    connect(stop_, &QPushButton::clicked, this, [this] {
        submit(QStringLiteral("正在停止投屏…"), [service = service_] {
            service->stop();
            return testtools::MirrorOperationResult{true, {}};
        });
    });
    auto* toolbar = new QHBoxLayout;
    using Control = testtools::MirrorControl;
    auto* power = new QPushButton(QStringLiteral("电源键"), this);
    power->setObjectName(QStringLiteral("mirrorPowerButton"));
    controlButtons_.push_back(power);
    toolbar->addWidget(power);
    connect(power, &QPushButton::clicked, this, [this] {
        submit(QStringLiteral("正在操作电源键…"),
               [service = service_] { return service->control(Control::Power); });
    });
    auto* capture = new QPushButton(QStringLiteral("保存截图…"), this);
    capture->setObjectName(QStringLiteral("mirrorScreenshotButton"));
    capture->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
    controlButtons_.push_back(capture);
    toolbar->addWidget(capture);
    connect(capture, &QPushButton::clicked, this, &ScreenMirrorPanel::saveScreenshot);
    auto* more = new QPushButton(QStringLiteral("更多操作"), this);
    more->setObjectName(QStringLiteral("mirrorMoreButton"));
    auto* menu = new QMenu(more);
    struct Action {
        const char* label;
        Control control;
    };
    const Action actions[] = {{"返回", Control::Back},
                              {"Home", Control::Home},
                              {"多任务", Control::Recents},
                              {"音量 +", Control::VolumeUp},
                              {"音量 −", Control::VolumeDown}};
    for (const auto& action : actions)
        menu->addAction(QString::fromUtf8(action.label), this, [this, control = action.control] {
            submit(QStringLiteral("正在操作投屏设备…"),
                   [service = service_, control] { return service->control(control); });
        });
    more->setMenu(menu);
    controlButtons_.push_back(more);
    toolbar->addWidget(more);
    toolbar->addStretch();
    layout->addLayout(toolbar);
    start_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    stop_->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
    auto* guide = new QFrame(this);
    guide->setObjectName(QStringLiteral("mirrorGuide"));
    auto* guideLayout = new QVBoxLayout(guide);
    guideLayout->setContentsMargins(24, 24, 24, 24);
    guideLayout->setSpacing(12);
    auto* heading = new QLabel(QStringLiteral("在设备画面中直接操作"), guide);
    heading->setObjectName(QStringLiteral("sectionLabel"));
    guideLayout->addWidget(heading);
    auto* hint =
        new QLabel(QStringLiteral("开始投屏后，在独立窗口中使用鼠标和键盘操作应用、系统设置与浏览器"
                                  "。\n将 APK 拖入投屏窗口即可安装；文件页中的 APK "
                                  "只作传输。\n\nAlt+F 切换全屏；切换工作区会保留当前投屏。"),
                   guide);
    hint->setObjectName(QStringLiteral("mutedLabel"));
    hint->setWordWrap(true);
    guideLayout->addWidget(hint);
    layout->addWidget(guide);
    layout->addStretch(1);
    detail_ = new QPlainTextEdit(this);
    detail_->setReadOnly(true);
    detail_->setMaximumHeight(130);
    detail_->setObjectName(QStringLiteral("mirrorErrorDetail"));
    detail_->hide();
    layout->addWidget(detail_);
    timer_ = new QTimer(this);
    timer_->setInterval(500);
    connect(timer_, &QTimer::timeout, this, &ScreenMirrorPanel::poll);
    timer_->start();
    updateControls();
}

ScreenMirrorPanel::~ScreenMirrorPanel()
{
    // Normal main-window close awaits shutdownFinished before destroying this panel.
    service_->requestShutdown();
    tasks_.wait();
}

void ScreenMirrorPanel::reject()
{
    if (isWindow())
        QDialog::reject();
}

void ScreenMirrorPanel::setDevice(const serialdemo::AdbDevice& device)
{
    selected_ = device;
    updateControls();
}

void ScreenMirrorPanel::updateControls()
{
    const bool sessionContext = state_.running || !state_.detail.empty() || busy_;
    const auto target = sessionContext && !state_.serial.empty() ? state_.serial : selected_.serial;
    target_->setText(target.empty() ? QStringLiteral("未选择设备")
                                    : QStringLiteral("设备：%1").arg(text(target)));
    if (state_.running && selected_.serial != state_.serial)
        target_->setText(target_->text() +
                         QStringLiteral("\n已切换全局选择；当前控制仍属于此投屏设备。"));
    else if (!state_.detail.empty() && !selected_.serial.empty() &&
             selected_.serial != state_.serial)
        target_->setText(target_->text() +
                         QStringLiteral("\n下次启动设备：%1").arg(text(selected_.serial)));
    if (!busy_)
        status_->setText(text(state_.message));
    start_->setEnabled(!shuttingDown_ && !busy_ && !state_.running && !selected_.serial.empty() &&
                       selected_.state == "device" &&
                       !qEnvironmentVariableIsSet("TESTTOOLS_OFFLINE"));
    stop_->setEnabled(!shuttingDown_ && !busy_ && state_.running);
    for (auto* button : controlButtons_)
        button->setEnabled(!shuttingDown_ && !busy_ && state_.running);
    detail_->setVisible(!state_.detail.empty());
    detail_->setPlainText(text(state_.detail));
}

void ScreenMirrorPanel::startCurrentDevice()
{
    if (state_.running || busy_ || shuttingDown_ || qEnvironmentVariableIsSet("TESTTOOLS_OFFLINE"))
        return;
    state_.serial = selected_.serial;
    state_.detail.clear();
    submit(QStringLiteral("正在启动投屏…"),
           [service = service_, device = selected_] { return service->start(device); });
}

void ScreenMirrorPanel::submit(const QString& activity,
                               std::function<testtools::MirrorOperationResult()> operation)
{
    if (busy_ || shuttingDown_)
        return;
    busy_ = true;
    const auto generation = ++generation_;
    status_->setText(activity);
    updateControls();
    emit activityChanged(activity);
    QPointer<ScreenMirrorPanel> guard(this);
    tasks_.start([guard, service = service_, operation = std::move(operation), generation] {
        testtools::MirrorOperationResult result;
        try {
            result = operation();
        } catch (const std::exception& error) {
            result.error = error.what();
        }
        const auto state = service->snapshot();
        if (!guard)
            return;
        QMetaObject::invokeMethod(
            guard,
            [guard, result, state, generation] {
                if (!guard || guard->shuttingDown_ || generation != guard->generation_)
                    return;
                guard->busy_ = false;
                guard->state_ = state;
                if (!result.success) {
                    guard->state_.message = "操作失败";
                    guard->state_.detail = result.error;
                }
                guard->updateControls();
                emit guard->activityChanged(
                    result.success ? text(state.message)
                                   : QStringLiteral("投屏操作失败：%1").arg(text(result.error)));
            },
            Qt::QueuedConnection);
    });
}

void ScreenMirrorPanel::poll()
{
    if (!state_.running || busy_ || polling_ || shuttingDown_)
        return;
    polling_ = true;
    const auto generation = generation_;
    QPointer<ScreenMirrorPanel> guard(this);
    tasks_.start([guard, service = service_, generation] {
        const auto state = service->poll();
        if (!guard)
            return;
        QMetaObject::invokeMethod(
            guard,
            [guard, state, generation] {
                if (!guard)
                    return;
                guard->polling_ = false;
                if (guard->shuttingDown_ || generation != guard->generation_)
                    return;
                const bool ended = guard->state_.running && !state.running;
                guard->state_ = state;
                guard->updateControls();
                if (ended)
                    emit guard->activityChanged(text(state.message));
            },
            Qt::QueuedConnection);
    });
}

void ScreenMirrorPanel::saveScreenshot()
{
    const auto suggested =
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation) +
        QStringLiteral("/TestTools-%1.png")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
    const auto file = QFileDialog::getSaveFileName(
        this, QStringLiteral("保存设备截图"), suggested, QStringLiteral("PNG 图片 (*.png)"));
    if (file.isEmpty())
        return;
    const auto path = std::filesystem::path(file.toStdWString());
    submit(QStringLiteral("正在保存截图…"),
           [service = service_, path] { return service->screenshot(path); });
}

void ScreenMirrorPanel::shutdown()
{
    if (shuttingDown_)
        return;
    shuttingDown_ = true;
    ++generation_;
    timer_->stop();
    service_->requestShutdown();
    status_->setText(QStringLiteral("正在关闭投屏…"));
    updateControls();
    QPointer<ScreenMirrorPanel> guard(this);
    tasks_.start([guard, service = service_] {
        service->stop();
        if (guard)
            QMetaObject::invokeMethod(
                guard,
                [guard] {
                    if (guard)
                        emit guard->shutdownFinished();
                },
                Qt::QueuedConnection);
    });
}
