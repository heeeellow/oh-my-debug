#include "ui/icons.h"
#include "app_info.h"
#include "main_window.h"

#include "application/agent/adb_agent_deployment.h"
#include "application/agent/agent_connection_service.h"
#include "application/device/adb_terminal_service.h"
#include "theme.h"
#include "ui/pages/file_transfer_workspace.h"
#include "ui/pages/serial_workspace.h"
#include "ui/pages/serial_terminal_workspace.h"
#include "ui/screen_mirror_panel.h"
#include "ui/widgets/board_info_panel.h"
#include "ui/widgets/device_maintenance_panel.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QMetaObject>
#include <QtCore/QPointer>
#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
#include <QtGui/QCloseEvent>
#include <QtGui/QGuiApplication>
#include <QtGui/QIcon>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMenu>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QStyle>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>

#include <utility>
#include <vector>

namespace {

using testtools::ui::fromUtf8;
using testtools::ui::toUtf8;

QString nonEmptyOr(const std::string& text, const QString& fallback = {})
{
    return text.empty() ? fallback : fromUtf8(text);
}

QString deviceDisplayName(const testtools::AdbDevice& device)
{
    const QString description = fromUtf8(device.description);
    const QStringList fields = description.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString& field : fields) {
        if (field.startsWith(QStringLiteral("model:"))) {
            QString model = field.mid(6);
            model.replace(QLatin1Char('_'), QLatin1Char(' '));
            return QStringLiteral("%1 · %2").arg(model, fromUtf8(device.serial));
        }
    }
    return fromUtf8(device.serial);
}

QString deviceStateText(const std::string& state)
{
    if (state == "unauthorized") {
        return QStringLiteral("待授权");
    }
    if (state == "offline") {
        return QStringLiteral("离线");
    }
    return QStringLiteral("不可用");
}

QString deviceScanSignature(const testtools::DeviceScanResult& result)
{
    QStringList entries;
    entries.reserve(static_cast<qsizetype>(result.devices.size()));
    for (const auto& device : result.devices) {
        entries.push_back(QStringLiteral("%1\x1f%2\x1f%3")
                              .arg(fromUtf8(device.serial),
                                   fromUtf8(device.state),
                                   fromUtf8(device.description)));
    }
    entries.sort();
    return fromUtf8(result.error) + QLatin1Char('\x1e') + entries.join(QLatin1Char('\x1e'));
}

} // namespace

MainWindow::MainWindow(QWidget* parent,
                       std::shared_ptr<testtools::DeviceMaintenanceService> maintenance,
                       std::shared_ptr<testtools::BoardInfoService> boardInfo)
    : QMainWindow(parent),
      boardInfoService_(boardInfo ? std::move(boardInfo)
                                  : std::make_shared<testtools::BoardInfoService>()),
      maintenanceService_(maintenance ? std::move(maintenance)
                                      : testtools::createDeviceMaintenanceService())
{
    buildUi();
    taskTimer_ = new QTimer(this);
    taskTimer_->setInterval(250);
    connect(taskTimer_, &QTimer::timeout, this, [this] {
        updateTasks();
        if (closeRequested_ && fileShutdownComplete_ && mirrorShutdownComplete_ &&
            maintenanceShutdownComplete_ && boardShutdownComplete_ && !recordingsPending())
            close();
    });
    taskTimer_->start();
    connect(QCoreApplication::instance(),
            &QCoreApplication::aboutToQuit,
            this,
            &MainWindow::prepareForShutdown,
            Qt::DirectConnection);
    if (qEnvironmentVariableIsSet("TESTTOOLS_OFFLINE")) {
        setAdbStatus(QStringLiteral("ADB · 未连接"), StatusTone::Neutral);
        setActivity(QStringLiteral("当前未连接设备，本地串口仍可使用"));
        refreshButton_->setEnabled(false);
        deviceCombo_->addItem(QStringLiteral("未连接开发板"));
        deviceCombo_->setEnabled(false);
    } else {
        refreshDevices();
        deviceRefreshTimer_ = new QTimer(this);
        deviceRefreshTimer_->setInterval(2500);
        deviceRefreshTimer_->setTimerType(Qt::CoarseTimer);
        connect(deviceRefreshTimer_, &QTimer::timeout, this, [this] {
            deviceRefreshTimer_->setInterval(
                QGuiApplication::applicationState() == Qt::ApplicationActive ? 2500 : 10000);
            startDeviceScan(true);
        });
        deviceRefreshTimer_->start();
        connect(qGuiApp,
                &QGuiApplication::applicationStateChanged,
                this,
                [this](Qt::ApplicationState state) {
                    if (state == Qt::ApplicationActive && !closeRequested_ && !shuttingDown_) {
                        deviceRefreshTimer_->setInterval(2500);
                        startDeviceScan(true);
                        checkServiceHealth();
                    }
                });
    }
    serviceHealthTimer_ = new QTimer(this);
    serviceHealthTimer_->setInterval(3000);
    connect(serviceHealthTimer_, &QTimer::timeout, this, &MainWindow::checkServiceHealth);
    if (!qEnvironmentVariableIsSet("TESTTOOLS_OFFLINE"))
        serviceHealthTimer_->start();
    updateServicePresentation();
    updateAdbTerminalControls();
}

MainWindow::~MainWindow()
{
    prepareForShutdown();
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (!shuttingDown_ &&
        (!mirrorShutdownComplete_ || !fileShutdownComplete_ || !maintenanceShutdownComplete_ ||
         !boardShutdownComplete_ || recordingsPending())) {
        event->ignore();
        closeRequested_ = true;
        if (serviceHealthTimer_)
            serviceHealthTimer_->stop();
        if (deviceRefreshTimer_) deviceRefreshTimer_->stop();
        centralWidget()->setEnabled(false);
        updateAdbTerminalControls();
        setActivity(QStringLiteral("正在保存设置、录制并结束后台任务…"));
        for (auto* workspace : {static_cast<SerialEndpointWorkspace*>(serialWorkspace_),
                                static_cast<SerialEndpointWorkspace*>(terminalWorkspace_)}) if (workspace) {
            workspace->stopRecordings();
            workspace->flushPreferences();
            for (const auto& task : workspace->activeTasks())
                if (task.kind != SerialEndpointWorkspace::BackgroundTask::Kind::Recording) workspace->stopTask(task);
        }
        if (fileWorkspace_) fileWorkspace_->shutdown();
        else fileShutdownComplete_ = true;
        if (screenMirrorPanel_) screenMirrorPanel_->shutdown();
        else mirrorShutdownComplete_ = true;
        if (maintenancePanel_) maintenancePanel_->shutdown();
        else maintenanceShutdownComplete_ = true;
        if (boardInfoPanel_)
            boardInfoPanel_->shutdown();
        else
            boardShutdownComplete_ = true;
        return;
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::prepareForShutdown()
{
    if (shuttingDown_) {
        return;
    }
    shuttingDown_ = true;
    if (taskTimer_) taskTimer_->stop();
    if (serviceHealthTimer_)
        serviceHealthTimer_->stop();
    if (fileWorkspace_) fileWorkspace_->shutdown();
    if (screenMirrorPanel_) screenMirrorPanel_->shutdown();
    if (maintenancePanel_) maintenancePanel_->shutdown();
    if (boardInfoPanel_)
        boardInfoPanel_->shutdown();
    updateAdbTerminalControls();
    const std::string selectedSerial = selectedDevice().serial;
    if (serialWorkspace_) {
        serialWorkspace_->prepareForShutdown();
    }
    if (terminalWorkspace_)
        terminalWorkspace_->prepareForShutdown();
    terminalTasks_.wait();
    if (qEnvironmentVariableIsSet("TESTTOOLS_OFFLINE")) return;
    ++agentGeneration_;
    agentConnecting_ = false;
    if (agentClient_) {
        agentClient_->close();
        agentClient_.reset();
    }
    const std::string agentSerial = agentAdbSerial_;
    const std::uint16_t agentPort = agentForwardPort_;
    agentAdbSerial_.clear();
    agentForwardPort_ = 0;
    if (!agentSerial.empty() && agentPort != 0) {
        testtools::AdbAgentDeployment().removeForward(agentSerial, agentPort);
    }
    scanTasks_.wait();
    agentTasks_.wait();
    // 部署任务可能在事件循环开始退出后才完成，此时排队回调尚未接管端口所有权；
    // 因此仅针对当前 Agent 固定控制端口执行定向转发清理。
    testtools::AdbAgentDeployment().removeControlForwards(selectedSerial);
    if (!agentSerial.empty() && agentSerial != selectedSerial) {
        testtools::AdbAgentDeployment().removeControlForwards(agentSerial);
    }
    QCoreApplication::removePostedEvents(this);
}

bool MainWindow::navigateTo(const QString& pageName)
{
    const QString normalized = pageName.trimmed().toLower();
    if (normalized == QStringLiteral("serial") || normalized == QStringLiteral("overview")) {
        showPage(lastSerialPage_);
    } else if (normalized == QStringLiteral("terminal")) {
        showPage(TerminalPage);
    } else if (normalized == QStringLiteral("settings") ||
               normalized == QStringLiteral("device")) {
        showPage(DeviceInfoPage);
    } else if (normalized == QStringLiteral("adb")) {
        showPage(lastAdbPage_);
    } else if (normalized == QStringLiteral("files") || normalized == QStringLiteral("transfer")) {
        showPage(FileTransferPage);
    } else if ((normalized == QStringLiteral("mirror") || normalized == QStringLiteral("remote"))) {
        showPage(MirrorPage);
    } else {
        return false;
    }
    return true;
}

bool MainWindow::setSerialModeForDiagnostics(const QString& modeName)
{
    if (modeName == QStringLiteral("terminal")) {
        showPage(TerminalPage);
        return terminalWorkspace_ != nullptr;
    }
    showPage(SerialPage);
    return serialWorkspace_ && serialWorkspace_->setWorkspaceModeForDiagnostics(modeName);
}

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral(OMD_APP_NAME));
    resize(1440, 900);
    setMinimumSize(1120, 720);
    setStyleSheet(testtools::ui::applicationStyleSheet());

    auto* surface = new QWidget(this);
    surface->setObjectName(QStringLiteral("appSurface"));
    auto* root = new QVBoxLayout(surface);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(buildTopBar());

    auto* body = new QWidget(surface);
    auto* bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);
    bodyLayout->addWidget(buildNavigation());

    pages_ = new QStackedWidget(body);
    pages_->addWidget(buildSerialPage());
    pages_->addWidget(buildDeviceInfoPage());
    pages_->addWidget(buildFileTransferPage());
    pages_->addWidget(buildMirrorPage());
    pages_->addWidget(buildSerialPage(true));
    bodyLayout->addWidget(pages_, 1);
    root->addWidget(body, 1);

    auto* activity = new QFrame(surface);
    activity->setObjectName(QStringLiteral("activityBar"));
    activity->setMinimumHeight(38);
    activity->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto* activityLayout = new QHBoxLayout(activity);
    activityLayout->setContentsMargins(18, 9, 18, 9);
    auto* activityTitle = new QLabel(QStringLiteral("最近操作"), activity);
    activityTitle->setObjectName(QStringLiteral("activityTitle"));
    activityLabel_ = new QLabel(QStringLiteral("正在初始化设备列表"), activity);
    activityLabel_->setObjectName(QStringLiteral("activityMessage"));
    activityLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    activityLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    activityLayout->addWidget(activityTitle);
    activityLayout->addWidget(activityLabel_, 1);
    taskButton_ = new QToolButton(activity);
    taskButton_->setObjectName(QStringLiteral("backgroundTasksButton"));
    taskButton_->setText(QStringLiteral("无后台任务"));
    taskButton_->hide();
    taskButton_->setPopupMode(QToolButton::InstantPopup);
    taskMenu_ = new QMenu(taskButton_);
    taskButton_->setMenu(taskMenu_);
    connect(taskMenu_, &QMenu::aboutToShow, this, [this] { updateTasks(true); });
    activityLayout->addWidget(taskButton_);
    root->addWidget(activity);

    setCentralWidget(surface);
    for (auto* button : findChildren<QPushButton*>()) button->setCursor(Qt::PointingHandCursor);
    for (auto* button : findChildren<QToolButton*>()) button->setCursor(Qt::PointingHandCursor);
    showPage(SerialPage);
}

QWidget* MainWindow::buildTopBar()
{
    auto* bar = new QFrame(this);
    bar->setObjectName(QStringLiteral("topBar"));
    bar->setMinimumHeight(58);
    bar->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto* layout = new QHBoxLayout(bar);
    layout->setContentsMargins(18, 11, 16, 11);
    layout->setSpacing(12);

    auto* brand = new QHBoxLayout;
    brand->setSpacing(8);
    auto* title = new QLabel(QStringLiteral(OMD_APP_NAME), bar);
    title->setObjectName(QStringLiteral("appTitle"));
    auto* subtitle = new QLabel(QStringLiteral("调试工作台"), bar);
    subtitle->setObjectName(QStringLiteral("appSubtitle"));
    brand->addWidget(title);
    brand->addWidget(subtitle);
    layout->addLayout(brand);
    layout->addSpacing(8);

    auto* caption = new QLabel(QStringLiteral("设备"), bar);
    caption->setObjectName(QStringLiteral("topCaption"));
    deviceCombo_ = new QComboBox(bar);
    deviceCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    deviceCombo_->setMinimumContentsLength(10);
    deviceCombo_->setMinimumWidth(180);
    deviceCombo_->setMaximumWidth(280);
    adbStatusLabel_ = new QLabel(QStringLiteral("设备 · 检测中"), bar);
    adbStatusLabel_->setObjectName(QStringLiteral("topStatusNeutral"));
    refreshButton_ = new QPushButton(QStringLiteral("刷新设备"), bar);
    refreshButton_->setObjectName(QStringLiteral("topGhostButton"));

    layout->addWidget(caption);
    auto* deviceActions = new QHBoxLayout;
    deviceActions->setSpacing(6);
    deviceActions->addWidget(deviceCombo_);
    refreshButton_->setText(QStringLiteral("刷新"));
    refreshButton_->setIcon(testtools::ui::appIcon(testtools::ui::Icon::Refresh));
    refreshButton_->setIconSize({20, 20});
    refreshButton_->setToolTip(QStringLiteral("刷新设备列表"));
    refreshButton_->setCursor(Qt::PointingHandCursor);
    deviceActions->addWidget(refreshButton_);
    layout->addLayout(deviceActions);
    layout->addStretch(1);
    layout->addWidget(adbStatusLabel_);
    topServiceStatus_ = new QLabel(QStringLiteral("● 板端服务：未连接"), bar);
    topServiceStatus_->setObjectName(QStringLiteral("topServiceStatus"));
    layout->addWidget(topServiceStatus_);
    topServiceButton_ = new QPushButton(QStringLiteral("连接服务"), bar);
    topServiceButton_->setObjectName(QStringLiteral("serviceStatusButton"));
    topServiceButton_->setProperty("diagnosticObjectName", QStringLiteral("serviceStatusButton"));
    topServiceButton_->setCursor(Qt::PointingHandCursor);
    layout->addWidget(topServiceButton_);
    connect(topServiceButton_, &QPushButton::clicked, this, [this] {
        if (agentClient_)
            showPage(DeviceInfoPage);
        else
            deploySelectedAgent();
    });

    connect(refreshButton_, &QPushButton::clicked, this, &MainWindow::refreshDevices);
    connect(deviceCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::applySelectedDevice);
    return bar;
}

QWidget* MainWindow::buildNavigation()
{
    auto* rail = new QFrame(this);
    rail->setObjectName(QStringLiteral("navigationRail"));
    rail->setFixedWidth(168);
    auto* layout = new QVBoxLayout(rail);
    layout->setContentsMargins(10, 16, 10, 12);
    layout->setSpacing(4);

    serialGroupRow_ = new QFrame(rail);
    serialGroupRow_->setObjectName(QStringLiteral("navigationGroupRow"));
    serialGroupRow_->setAttribute(Qt::WA_Hover);
    auto* serialRow = new QHBoxLayout(serialGroupRow_);
    serialRow->setContentsMargins(0,0,0,0); serialRow->setSpacing(0);
    auto* serial = new QPushButton(QStringLiteral("串口调试"), rail);
    serialGroupButton_ = serial;
    serial->setCheckable(true);
    serial->setObjectName(QStringLiteral("navigationGroupButton"));
    serialExpandButton_ = new QToolButton(rail); serialExpandButton_->setFixedSize(32,38); serialExpandButton_->setAutoRaise(true);
    serialExpandButton_->setObjectName(QStringLiteral("serialExpandButton"));
    serialExpandButton_->setAccessibleName(QStringLiteral("展开或收起串口子页面"));
    serialRow->addWidget(serial,1); serialRow->addWidget(serialExpandButton_); layout->addWidget(serialGroupRow_);
    serialChildren_ = new QWidget(rail); serialChildren_->setObjectName(QStringLiteral("serialNavigationChildren"));
    auto* serialPages = new QVBoxLayout(serialChildren_);
    serialPages->setContentsMargins(18, 0, 0, 2);
    serialPages->setSpacing(4);
    serialPages->addWidget(addNavigationButton(QStringLiteral("单串口调试"), SerialPage, QStringLiteral("single")));
    serialPages->addWidget(addNavigationButton(QStringLiteral("双端调试"), SerialPage, QStringLiteral("dual")));
    serialPages->addWidget(addNavigationButton(QStringLiteral("串口终端"), TerminalPage, QStringLiteral("terminal")));
    layout->addWidget(serialChildren_); setSerialExpanded(true);
    connect(serial, &QPushButton::clicked, this, [this]{showPage(lastSerialPage_);});
    connect(serialExpandButton_, &QToolButton::clicked, this, [this]{setSerialExpanded(serialChildren_->isHidden());});
    adbGroupRow_ = new QFrame(rail);
    adbGroupRow_->setObjectName(QStringLiteral("navigationGroupRow"));
    adbGroupRow_->setAttribute(Qt::WA_Hover);
    auto* parentRow = new QHBoxLayout(adbGroupRow_);
    parentRow->setContentsMargins(0, 0, 0, 0); parentRow->setSpacing(0);
    auto* adb = new QPushButton(QStringLiteral("ADB 调试"), rail);
    adbGroupButton_ = adb;
    adb->setCheckable(true);
    adb->setObjectName(QStringLiteral("navigationGroupButton"));
    adb->setProperty("diagnosticObjectName", QStringLiteral("adbNavigation"));
    adbExpandButton_ = new QToolButton(rail);
    adbExpandButton_->setFixedSize(32, 38); adbExpandButton_->setAutoRaise(true);
    adbExpandButton_->setObjectName(QStringLiteral("adbExpandButton"));
    adbExpandButton_->setAccessibleName(QStringLiteral("展开或收起 ADB 子页面"));
    parentRow->addWidget(adb, 1); parentRow->addWidget(adbExpandButton_); layout->addWidget(adbGroupRow_);
    adbChildren_ = new QWidget(rail); adbChildren_->setObjectName(QStringLiteral("adbNavigationChildren"));
    auto* children = new QVBoxLayout(adbChildren_);
    children->setContentsMargins(18, 0, 0, 2);
    children->setSpacing(4);
    children->addWidget(addNavigationButton(QStringLiteral("远程调试"), MirrorPage));
    children->addWidget(addNavigationButton(QStringLiteral("文件传输"), FileTransferPage));
    layout->addWidget(adbChildren_); setAdbExpanded(false);
    connect(adb, &QPushButton::clicked, this, [this] { showPage(lastAdbPage_); });
    connect(adbExpandButton_, &QToolButton::clicked, this, [this] { setAdbExpanded(adbChildren_->isHidden()); });
    layout->addStretch();
    layout->addWidget(addNavigationButton(QStringLiteral("设备信息"), DeviceInfoPage));
    return rail;
}

void MainWindow::setAdbExpanded(bool expanded)
{
    adbChildren_->setVisible(expanded);
    adbExpandButton_->setIcon(testtools::ui::navigationChevron(expanded, adbGroupButton_->isChecked()));
    adbExpandButton_->setToolTip(expanded ? QStringLiteral("收起 ADB 子页面") : QStringLiteral("展开 ADB 子页面"));
}

QWidget* MainWindow::wrapAdbPage(const QString& title, QWidget* content)
{
    auto* container = new QWidget(this);
    auto* layout = new QVBoxLayout(container); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(12);
    auto* header = new QHBoxLayout;
    auto* heading = new QLabel(title, container); heading->setObjectName(QStringLiteral("pageTitle"));
    header->addWidget(heading); header->addStretch();
    layout->addLayout(header);
    layout->addWidget(content, 1);
    return wrapPage({}, {}, container, false);
}

QWidget* MainWindow::buildFileTransferPage()
{
    fileWorkspace_ = new FileTransferWorkspace(this);
    connect(fileWorkspace_, &FileTransferWorkspace::activityChanged, this, &MainWindow::setActivity);
    connect(fileWorkspace_, &FileTransferWorkspace::shutdownFinished, this, [this] {
        fileShutdownComplete_ = true; if (closeRequested_ && mirrorShutdownComplete_) close();
    });
    return wrapAdbPage(QStringLiteral("文件传输"), fileWorkspace_);
}

namespace {
class RemoteColumns final : public QWidget {
    QBoxLayout* columns_;
  public:
    RemoteColumns(QWidget* left, QWidget* right, QWidget* parent) : QWidget(parent)
    {
        setObjectName("remoteDebugColumns");
        columns_ = new QBoxLayout(QBoxLayout::LeftToRight,this);
        columns_->setContentsMargins(0,0,0,0); columns_->setSpacing(16);
        columns_->addWidget(left,1); columns_->addWidget(right,1);
        columns_->setAlignment(left,Qt::AlignTop); columns_->setAlignment(right,Qt::AlignTop);
        columns_->setSizeConstraint(QLayout::SetMinimumSize);
    }
    void resizeEvent(QResizeEvent* event) override
    {
        columns_->setDirection(width()>=980 ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom);
        setProperty("columnCount", width()>=980 ? 2 : 1);
        QWidget::resizeEvent(event);
    }
};
}
QWidget* MainWindow::buildMirrorPage()
{
    auto* left = new QWidget(this);
    auto* layout = new QVBoxLayout(left); layout->setContentsMargins(0,0,0,0); layout->setSpacing(12);
    auto* shell = new QFrame(left); shell->setObjectName(QStringLiteral("featureCard"));
    auto* shellLayout = new QVBoxLayout(shell); shellLayout->setContentsMargins(16,14,16,14); shellLayout->setSpacing(10);
    auto* shellTitle = new QLabel(QStringLiteral("ADB Shell"),shell); shellTitle->setObjectName(QStringLiteral("sectionLabel"));
    shellLayout->addWidget(shellTitle);
    auto* shellHint = new QLabel(
        QStringLiteral(
            "终端沿用设备当前权限，不自动提权或降权。连接 Android 板端服务时可能已请求 Root。"),
        shell);
    shellHint->setObjectName(QStringLiteral("mutedLabel")); shellHint->setWordWrap(true); shellLayout->addWidget(shellHint);
    adbTerminalButton_ = new QPushButton(QStringLiteral("打开 ADB 终端 ↗"),shell);
    adbTerminalButton_->setObjectName(QStringLiteral("adbTerminalButton"));
    adbTerminalButton_->setProperty("diagnosticObjectName","adbTerminalButton");
    terminalButtons_.push_back(adbTerminalButton_);
    connect(adbTerminalButton_,&QPushButton::clicked,this,&MainWindow::openAdbTerminal);
    shellLayout->addWidget(adbTerminalButton_, 0, Qt::AlignLeft);
    adbShellIdentity_ = new QLabel(QStringLiteral("打开时核对实际 Shell 权限"), shell);
    adbShellIdentity_->setObjectName("adbShellIdentity");
    adbShellIdentity_->setWordWrap(true);
    adbShellIdentity_->setTextFormat(Qt::PlainText);
    shellLayout->addWidget(adbShellIdentity_);
    layout->addWidget(shell);
    maintenancePanel_ = new DeviceMaintenancePanel(left,maintenanceService_);
    maintenancePanel_->conflicts = [this](const testtools::AdbDevice& device){return maintenanceConflicts(device);};
    connect(maintenancePanel_,&DeviceMaintenancePanel::activityChanged,this,&MainWindow::setActivity);
    connect(maintenancePanel_,&DeviceMaintenancePanel::taskChanged,this,&MainWindow::updateMaintenanceState);
    connect(maintenancePanel_,&DeviceMaintenancePanel::shutdownFinished,this,[this]{maintenanceShutdownComplete_=true;});
    layout->addWidget(maintenancePanel_); layout->addStretch();
    auto* right = new QFrame(this); right->setObjectName(QStringLiteral("featureCard"));
    auto* mirrorLayout = new QVBoxLayout(right); mirrorLayout->setContentsMargins(16,14,16,14); mirrorLayout->setSpacing(10);
    auto* mirrorTitle = new QLabel(QStringLiteral("Android 投屏"),right); mirrorTitle->setObjectName(QStringLiteral("sectionLabel")); mirrorLayout->addWidget(mirrorTitle);
    screenMirrorPanel_ = new ScreenMirrorPanel(right,{},true); mirrorLayout->addWidget(screenMirrorPanel_);
    connect(screenMirrorPanel_,&ScreenMirrorPanel::activityChanged,this,&MainWindow::setActivity);
    connect(screenMirrorPanel_,&ScreenMirrorPanel::shutdownFinished,this,[this]{mirrorShutdownComplete_=true;});
    auto* scroll = new QScrollArea(this); scroll->setObjectName("remoteDebugScroll"); scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame); scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet(QStringLiteral("QScrollArea#remoteDebugScroll { background: transparent; }"));
    scroll->viewport()->setAutoFillBackground(false);
    scroll->setWidget(new RemoteColumns(left,right,scroll));
    scroll->widget()->setAutoFillBackground(false);
    return wrapAdbPage(QStringLiteral("远程调试"),scroll);
}

QPushButton* MainWindow::addNavigationButton(const QString& text, int pageIndex, const QString& serialMode)
{
    auto* button = new QPushButton(text, this);
    button->setObjectName(pageIndex == DeviceInfoPage ? QStringLiteral("navigationButton")
                                                      : QStringLiteral("navigationChildButton"));
    button->setProperty("pageIndex", pageIndex);
    button->setProperty("serialMode", serialMode);
    button->setCheckable(true);
    button->setCursor(Qt::PointingHandCursor);
    connect(button, &QPushButton::clicked, this, [this, pageIndex, serialMode] {
        if (pageIndex == SerialPage && !serialMode.isEmpty() &&
            !serialWorkspace_->selectWorkspaceMode(serialMode)) { updateNavigation(); return; }
        showPage(pageIndex);
    });
    navigationButtons_.push_back(button);
    return button;
}

QWidget* MainWindow::buildSerialPage(bool terminal)
{
    SerialEndpointWorkspace* workspace = nullptr;
    if (terminal) {
        terminalWorkspace_ = new SerialTerminalWorkspace(this, portClaims_);
        workspace = terminalWorkspace_;
    } else {
        serialWorkspace_ = new SerialWorkspace(this, portClaims_);
        workspace = serialWorkspace_;
    }
    workspace->setObjectName(terminal ? QStringLiteral("serialTerminalWorkspace")
                                     : QStringLiteral("serialDebugWorkspace"));
    connect(workspace,
            &SerialEndpointWorkspace::prepareServiceRequested,
            this,
            &MainWindow::deploySelectedAgent);
    connect(workspace, &SerialEndpointWorkspace::workspaceModeChanged, this, [this] { updateNavigation(); });
    connect(workspace, &SerialEndpointWorkspace::activityChanged, this, &MainWindow::setActivity);
    connect(workspace, &SerialEndpointWorkspace::busyChanged, this, [this](bool) {
        serialBusy_ = (serialWorkspace_ && serialWorkspace_->hasActiveConnections()) ||
                      (terminalWorkspace_ && terminalWorkspace_->hasActiveConnections());
        updateDeviceControls();
        updateAgentControls();
    });
    return wrapPage({},
                    {},
                    workspace,
                    false);
}

QWidget* MainWindow::buildDeviceInfoPage()
{
    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    auto* connectionCard = new QFrame(content);
    connectionCard->setObjectName("featureCard");
    auto* connectionLayout = new QVBoxLayout(connectionCard);
    connectionLayout->setContentsMargins(16, 14, 16, 14);
    connectionLayout->setSpacing(6);
    auto* heading = new QLabel(QStringLiteral("设备连接"), connectionCard);
    heading->setObjectName("sectionLabel");
    connectionLayout->addWidget(heading);
    auto label = [&](const QString& initial, QWidget* parent) {
        auto* value = new QLabel(initial, parent);
        value->setObjectName("cardBody");
        value->setWordWrap(true);
        value->setTextFormat(Qt::PlainText);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        value->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        return value;
    };
    agentStatusLabel_ = label(QStringLiteral("板端服务 · 未连接"), connectionCard);
    agentStatusLabel_->setObjectName("topStatusNeutral");
    connectionLayout->addWidget(agentStatusLabel_, 0, Qt::AlignLeft);
    agentDeviceValue_ = label(QStringLiteral("目标设备：未选择"), connectionCard);
    connectionLayout->addWidget(agentDeviceValue_);
    auto* actions = new QHBoxLayout;
    deployAgentButton_ = new QPushButton(QStringLiteral("连接板端服务"), connectionCard);
    deployAgentButton_->setObjectName("primaryButton");
    disconnectAgentButton_ = new QPushButton(QStringLiteral("断开板端服务"), connectionCard);
    stopAgentButton_ = new QPushButton(QStringLiteral("重置板端服务"), connectionCard);
    actions->addWidget(deployAgentButton_);
    actions->addWidget(disconnectAgentButton_);
    actions->addWidget(stopAgentButton_);
    actions->addStretch();
    connectionLayout->addLayout(actions);
    agentConnectionDetail_ = label(QStringLiteral("尚未连接设备"), connectionCard);
    connectionLayout->addWidget(agentConnectionDetail_);
    agentVersionValue_ =
        label(QStringLiteral("应用要求：v%1 · 设备：—")
                  .arg(QString::fromLatin1(testtools::AdbAgentDeployment::RequiredAgentVersion)),
              connectionCard);
    connectionLayout->addWidget(agentVersionValue_);
    agentSecurityValue_ = label(QStringLiteral("连接权限：—"), connectionCard);
    connectionLayout->addWidget(agentSecurityValue_);
    agentCapabilitiesValue_ = label(QStringLiteral("可用功能：—"), connectionCard);
    connectionLayout->addWidget(agentCapabilitiesValue_);
    auto* systemCard = new QFrame(content);
    systemCard->setObjectName("featureCard");
    auto* systemLayout = new QVBoxLayout(systemCard);
    systemLayout->setContentsMargins(16, 14, 16, 14);
    systemLayout->setSpacing(10);
    auto* systemTitle = new QLabel(QStringLiteral("硬件与系统"), systemCard);
    systemTitle->setObjectName("sectionLabel");
    systemLayout->addWidget(systemTitle);
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(16);
    grid->setColumnMinimumWidth(0, 76);
    grid->setVerticalSpacing(9);
    grid->setColumnStretch(1, 1);
    const auto row = [&](int index, const QString& name, QLabel*& value) {
        auto* nameLabel = new QLabel(name, systemCard);
        nameLabel->setObjectName("cardEyebrow");
        value = label(QStringLiteral("—"), systemCard);
        grid->addWidget(nameLabel, index, 0, Qt::AlignTop);
        grid->addWidget(value, index, 1);
    };
    row(0, QStringLiteral("板卡型号"), agentModelValue_);
    row(1, QStringLiteral("厂商 / 产品"), agentManufacturerValue_);
    row(2, QStringLiteral("硬件资源"), agentHardwareValue_);
    row(3, QStringLiteral("系统内核"), agentOsValue_);
    row(4, QStringLiteral("系统构建"), agentBuildValue_);
    systemLayout->addLayout(grid);
    boardInfoPanel_ = new BoardInfoPanel(systemCard, boardInfoService_);
    systemLayout->addWidget(boardInfoPanel_);
    systemLayout->addStretch();
    auto* columns = new RemoteColumns(connectionCard, systemCard, content);
    columns->setObjectName("deviceInfoColumns");
    layout->addWidget(columns);
    boardInfoPanel_->maintenanceBusy = [this](const std::string& serial) {
        const auto state = maintenanceService_->snapshot();
        return state.active && testtools::disruptive(state.request.action) &&
               state.request.device.serial == serial;
    };
    connect(boardInfoPanel_, &BoardInfoPanel::activityChanged, this, &MainWindow::setActivity);
    connect(boardInfoPanel_, &BoardInfoPanel::taskChanged, this, [this] {
        updateTasks();
        updateAgentControls();
    });
    connect(boardInfoPanel_, &BoardInfoPanel::shutdownFinished, this, [this] {
        boardShutdownComplete_ = true;
    });
    layout->addStretch();
    connect(deployAgentButton_, &QPushButton::clicked, this, &MainWindow::deploySelectedAgent);
    connect(disconnectAgentButton_, &QPushButton::clicked, this, [this] {
        disconnectAgent(QStringLiteral("设备连接已断开"));
    });
    connect(stopAgentButton_, &QPushButton::clicked, this, &MainWindow::stopSelectedAgent);
    updateAgentControls();
    auto* page =
        wrapPage(QStringLiteral("设备信息"), QStringLiteral("查看当前设备与只读硬件信息"), content);
    content->setAutoFillBackground(false);
    return page;
}

QWidget* MainWindow::wrapPage(const QString& title,
                              const QString& subtitle,
                              QWidget* content,
                              bool scrollable)
{
    auto* page = new QWidget(this);
    page->setObjectName(QStringLiteral("pageSurface"));
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(18, title.isEmpty() ? 12 : 18, 18, 14);
    layout->setSpacing(title.isEmpty() ? 8 : 13);

    auto* titleLabel = new QLabel(title, page);
    titleLabel->setObjectName(QStringLiteral("pageTitle"));
    auto* subtitleLabel = new QLabel(subtitle, page);
    subtitleLabel->setObjectName(QStringLiteral("pageSubtitle"));
    subtitleLabel->setWordWrap(true);
    layout->addWidget(titleLabel);
    layout->addWidget(subtitleLabel);
    titleLabel->setVisible(!title.isEmpty());
    subtitleLabel->setVisible(!subtitle.isEmpty());

    if (scrollable) {
        auto* scroll = new QScrollArea(page);
        scroll->setObjectName(QStringLiteral("pageScroll"));
        scroll->setWidgetResizable(true);
        scroll->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
        scroll->setMinimumSize(0, 0);
        scroll->setFrameShape(QFrame::NoFrame);
        content->setMinimumSize(0, 0);
        content->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored);
        scroll->setWidget(content);
        layout->addWidget(scroll, 1);
    } else {
        layout->addWidget(content, 1);
    }
    return page;
}

void MainWindow::deploySelectedAgent()
{
    connectSelectedAgent(true);
}

void MainWindow::connectSelectedAgent(bool allowDeploy)
{
    if (qEnvironmentVariableIsSet("TESTTOOLS_OFFLINE")) return;
    if (maintenanceBlocking_ || (agentConnecting_ && !(allowDeploy && passiveAgentProbe_)) ||
        agentCleanupPending_ || agentClient_ || shuttingDown_ || closeRequested_) {
        return;
    }
    const auto device = selectedDevice();
    if (device.serial.empty() || device.state != "device") {
        setAgentStatus(QStringLiteral("板端服务 · 未连接"),
                       StatusTone::Error,
                       QStringLiteral("请先在顶部选择已连接并授权的设备"));
        return;
    }

    const std::string serial = device.serial;
    const QByteArray applicationDirectoryBytes = QCoreApplication::applicationDirPath().toUtf8();
    const std::string applicationDirectory(
        applicationDirectoryBytes.constData(),
        static_cast<std::size_t>(applicationDirectoryBytes.size()));
    const std::uint64_t generation = ++agentGeneration_;
    agentConnecting_ = true;
    passiveAgentProbe_ = !allowDeploy;
    serviceHealthFailures_ = 0;
    setAgentStatus(
        allowDeploy ? QStringLiteral("板端服务 · 准备中") : QStringLiteral("板端服务 · 检查中"),
        StatusTone::Neutral,
        allowDeploy ? QStringLiteral("正在准备设备连接") : QStringLiteral("正在检查设备连接"));
    updateAgentControls();

    QPointer<MainWindow> guard(this);
    agentTasks_.start([guard, serial, applicationDirectory, generation, allowDeploy] {
        const auto progress = [guard, generation, allowDeploy](const std::string& message) {
            if (!allowDeploy)
                return;
            QString detail = fromUtf8(message);
            detail.replace(QStringLiteral("Android Agent Bundle"), QStringLiteral("服务文件"));
            detail.replace(QStringLiteral("Linux Agent"), QStringLiteral("设备服务"));
            detail.replace(QStringLiteral("板端 Agent"), QStringLiteral("设备服务"));
            detail.replace(QStringLiteral("Agent"), QStringLiteral("服务"));
            QMetaObject::invokeMethod(
                guard,
                [guard, generation, allowDeploy, detail] {
                    if (guard && !guard->shuttingDown_ && generation == guard->agentGeneration_) {
                        guard->setAgentStatus(allowDeploy ? QStringLiteral("板端服务 · 准备中")
                                                          : QStringLiteral("板端服务 · 检查中"),
                                              StatusTone::Neutral,
                                               detail);
                        guard->setActivity(detail);
                    }
                },
                Qt::QueuedConnection);
        };
        auto connectionResult = testtools::AgentConnectionService::connectDevice(
            serial, applicationDirectory, allowDeploy, progress);
        auto client = std::move(connectionResult.client);
        auto hello = std::move(connectionResult.hello);
        auto capabilities = std::move(connectionResult.capabilities);
        auto systemInfo = std::move(connectionResult.systemInfo);
        const QString observedVersion = fromUtf8(connectionResult.observedVersion);
        auto deploymentResult = std::move(connectionResult.deployment);
        const bool succeeded = connectionResult.succeeded;
        const QString connectionError = fromUtf8(connectionResult.error);
        QMetaObject::invokeMethod(
            guard,
            [guard,
             client = std::move(client),
             hello = std::move(hello),
             capabilities = std::move(capabilities),
             systemInfo = std::move(systemInfo),
             observedVersion = std::move(observedVersion),
             deploymentResult = std::move(deploymentResult),
             succeeded,
             connectionError,
             serial,
             generation,
             allowDeploy]() mutable {
                if (!guard || guard->shuttingDown_ || generation != guard->agentGeneration_) {
                    if (guard) {
                        // 手动连接可接管后台探测；旧探测的定向清理仍在后台完成。
                        guard->agentTasks_.start([client, serial, port = deploymentResult.localPort] {
                            client->close();
                            if (port)
                                testtools::AdbAgentDeployment().removeForward(serial, port);
                        });
                    }
                    return;
                }
                guard->agentConnecting_ = false;
                guard->passiveAgentProbe_ = false;
                if (!succeeded) {
                    client->close();
                    guard->setAgentStatus(
                        allowDeploy ? QStringLiteral("板端服务 · 连接失败")
                                    : QStringLiteral("板端服务 · 未连接"),
                        allowDeploy ? StatusTone::Error : StatusTone::Neutral,
                        allowDeploy
                            ? QStringLiteral("连接失败：%1").arg(connectionError)
                            : QStringLiteral("板端服务未连接：%1\n点击“连接板端服务”准备或重试。")
                                  .arg(connectionError));
                    if (guard->agentVersionValue_) {
                        guard->agentVersionValue_->setText(
                            QStringLiteral("应用要求：v%1 · 设备：%2")
                                .arg(QString::fromLatin1(
                                         testtools::AdbAgentDeployment::RequiredAgentVersion),
                                     observedVersion.isEmpty()
                                         ? QStringLiteral("未发现")
                                         : QStringLiteral("v%1 · 需要升级").arg(observedVersion)));
                    }
                    guard->updateAgentControls();
                    return;
                }

                guard->agentClient_ = std::move(client);
                guard->agentAdbSerial_ = serial;
                guard->agentForwardPort_ = deploymentResult.localPort;
                if (guard->serialWorkspace_) {
                    guard->serialWorkspace_->setAgentConnection(serial, deploymentResult.localPort);
                }
                if (guard->terminalWorkspace_)
                    guard->terminalWorkspace_->setAgentConnection(serial, deploymentResult.localPort);
                // 通过应用层服务解析握手、能力清单与系统信息，界面层仅负责展示文案拼装。
                const auto snapshot =
                    testtools::parseAgentServiceSnapshot(hello, capabilities, systemInfo);
                const QString version = nonEmptyOr(snapshot.agentVersion);
                const bool systemInfoCapability = snapshot.hasCapability("system.info");
                const bool serialStream = snapshot.hasCapability("serial.stream");
                const bool memoryCapacity = snapshot.hasCapability("memory.capacity");
                const bool storageInfo = snapshot.hasCapability("storage.device.info");
                const bool storageHealth = snapshot.hasCapability("storage.device.health");
                std::vector<std::string> boardCapabilities;
                QStringList boardCapabilityLabels;
                if (memoryCapacity) {
                    boardCapabilities.emplace_back("memory.capacity");
                    boardCapabilityLabels << QStringLiteral("内存容量（只读）");
                }
                if (storageInfo) {
                    boardCapabilities.emplace_back("storage.device.info");
                    boardCapabilityLabels << QStringLiteral("存储信息（只读）");
                }
                if (storageHealth) {
                    boardCapabilities.emplace_back("storage.device.health");
                    boardCapabilityLabels << QStringLiteral("存储健康（只读）");
                }
                if (guard->boardInfoPanel_)
                    guard->boardInfoPanel_->setSession(
                        guard->agentAdbSerial_,
                        generation,
                        guard->agentClient_,
                        boardCapabilities,
                        [port = guard->agentForwardPort_](
                            std::string& error) -> std::shared_ptr<testtools::IAgentConnection> {
                            auto client = testtools::AgentConnectionService::createClient();
                            if (!client->connect("127.0.0.1", port, error) ||
                                !testtools::probeAgentService(
                                    *client,
                                    testtools::AdbAgentDeployment::RequiredAgentVersion,
                                    error)) {
                                client->close();
                                return {};
                            }
                            return client;
                        });
                guard->agentSummary_ = QStringLiteral("已就绪");
                guard->setAgentStatus(
                    QStringLiteral("板端服务 · %1").arg(guard->agentSummary_),
                    StatusTone::Good,
                    QStringLiteral("设备连接已建立，可使用已启用的调试与检查功能"));
                guard->agentVersionValue_->setText(
                    QStringLiteral("应用要求：v%1 · 设备：v%2 · 已兼容")
                        .arg(QString::fromLatin1(
                                 testtools::AdbAgentDeployment::RequiredAgentVersion),
                             version.isEmpty() ? QStringLiteral("未知") : version));
                QStringList capabilityLabels;
                if (systemInfoCapability)
                    capabilityLabels << QStringLiteral("设备信息（只读）");
                if (serialStream)
                    capabilityLabels << QStringLiteral("串口调试");
                capabilityLabels.append(boardCapabilityLabels);
                guard->agentCapabilitiesValue_->setText(
                    QStringLiteral("可用功能：%1")
                        .arg(capabilityLabels.isEmpty()
                                 ? QStringLiteral("—")
                                 : capabilityLabels.join(QStringLiteral("、"))));
                guard->agentCapabilitiesValue_->setToolTip({});
                const auto& info = snapshot.systemInfo;
                const QString model = nonEmptyOr(info.model, QStringLiteral("未知型号"));
                const QString manufacturer = nonEmptyOr(info.manufacturer);
                const QString brand = nonEmptyOr(info.brand);
                const QString product = nonEmptyOr(info.product);
                const QString board = nonEmptyOr(info.board);
                const QString hardware = nonEmptyOr(info.hardware);
                const QString architecture =
                    nonEmptyOr(info.architecture, fromUtf8(deploymentResult.target));
                const int cores = info.cpuCores;
                const qint64 memoryKib = static_cast<qint64>(info.memoryTotalKib);
                const QString release = nonEmptyOr(info.osRelease, QStringLiteral("未知版本"));
                const QString sdk = nonEmptyOr(info.androidSdk);
                const QString kernel = nonEmptyOr(info.kernel, QStringLiteral("未知内核"));
                const QString systemBuild = nonEmptyOr(info.buildId);
                const int uid = info.effectiveUid;

                guard->agentModelValue_->setText(
                    board.isEmpty() ? model : QStringLiteral("%1 · %2").arg(model, board));
                QStringList manufacturerParts;
                for (const QString& part : {manufacturer, brand, product}) {
                    if (!part.isEmpty()) {
                        manufacturerParts << part;
                    }
                }
                const QString manufacturerLine = manufacturerParts.join(QStringLiteral(" · "));
                guard->agentManufacturerValue_->setText(
                    manufacturerLine.isEmpty() ? QStringLiteral("—") : manufacturerLine);
                QStringList hardwareParts{hardware, architecture};
                if (cores > 0) {
                    hardwareParts << QStringLiteral("%1 核 CPU").arg(cores);
                }
                if (memoryKib > 0) {
                    hardwareParts << QStringLiteral("%1 GiB 系统可见内存")
                                         .arg(double(memoryKib) / (1024.0 * 1024.0), 0, 'f', 2);
                }
                hardwareParts.removeAll(QString{});
                guard->agentHardwareValue_->setText(hardwareParts.join(QStringLiteral(" · ")));
                const QString platformRelease =
                    QStringLiteral("%1 %2").arg(fromUtf8(deploymentResult.platform), release);
                guard->agentOsValue_->setText(
                    sdk.isEmpty()
                        ? QStringLiteral("%1 · %2").arg(platformRelease, kernel)
                        : QStringLiteral("%1（API %2） · %3").arg(platformRelease, sdk, kernel));
                guard->agentBuildValue_->setText(systemBuild.isEmpty() ? QStringLiteral("—")
                                                                       : systemBuild);
                guard->agentSecurityValue_->setText(
                    uid == 0   ? QStringLiteral("服务权限：Root（UID 0）")
                    : uid >= 0 ? QStringLiteral("服务权限：UID %1").arg(uid)
                               : QStringLiteral("服务权限：未知"));
                for (QLabel* label : {guard->agentModelValue_,
                                      guard->agentManufacturerValue_,
                                      guard->agentHardwareValue_,
                                      guard->agentOsValue_,
                                      guard->agentBuildValue_,
                                      guard->agentSecurityValue_}) {
                    label->setToolTip({});
                }
                guard->agentHardwareValue_->setToolTip(
                    memoryKib > 0
                        ? QStringLiteral("系统可见内存：%1 KiB（MemTotal），不代表 DDR 标称容量")
                              .arg(memoryKib)
                        : QStringLiteral("系统可见内存未知"));
                guard->setActivity(QStringLiteral("设备连接完成，可开始调试或检查"));
                guard->updateAgentControls();
            },
            Qt::QueuedConnection);
    });
}

void MainWindow::stopSelectedAgent()
{
    if (maintenanceBlocking_ ||
        (boardInfoPanel_ && boardInfoPanel_->busyForDevice(selectedDevice().serial)) ||
        agentConnecting_ || shuttingDown_) {
        return;
    }
    std::string serial = agentAdbSerial_;
    if (serial.empty()) {
        serial = selectedDevice().serial;
    }
    if (serial.empty()) {
        setAgentStatus(QStringLiteral("板端服务 · 未连接"),
                       StatusTone::Error,
                       QStringLiteral("请先选择已连接的设备"));
        return;
    }
    disconnectAgent(QStringLiteral("正在重置设备连接"));
    const std::uint64_t generation = ++agentGeneration_;
    agentConnecting_ = true;
    updateAgentControls();
    QPointer<MainWindow> guard(this);
    agentTasks_.start([guard, serial, generation] {
        std::string error;
        const bool stopped = testtools::AdbAgentDeployment().stopRemote(serial, error);
        QMetaObject::invokeMethod(
            guard,
            [guard, stopped, generation] {
                if (!guard || guard->shuttingDown_ || generation != guard->agentGeneration_) {
                    return;
                }
                guard->agentConnecting_ = false;
                guard->setAgentStatus(stopped ? QStringLiteral("板端服务 · 已重置")
                                              : QStringLiteral("板端服务 · 重置失败"),
                                      stopped ? StatusTone::Neutral : StatusTone::Error,
                                      stopped ? QStringLiteral("设备连接已重置")
                                              : QStringLiteral("重置失败，请刷新设备后重试"));
                guard->updateAgentControls();
            },
            Qt::QueuedConnection);
    });
}

void MainWindow::disconnectAgent(const QString& reason)
{
    if (boardInfoPanel_)
        boardInfoPanel_->clearSession();
    ++agentGeneration_;
    agentConnecting_ = false;
    passiveAgentProbe_ = false;
    serviceHealthFailures_ = 0;
    auto client = std::move(agentClient_);
    if (serialWorkspace_)
        serialWorkspace_->setAgentConnection({}, 0);
    if (terminalWorkspace_)
        terminalWorkspace_->setAgentConnection({}, 0);
    const auto oldSerial = agentAdbSerial_;
    const auto oldPort = agentForwardPort_;
    if (client || (!oldSerial.empty() && oldPort)) {
        agentCleanupPending_ = true;
        QPointer<MainWindow> guard(this);
        const bool offline = qEnvironmentVariableIsSet("TESTTOOLS_OFFLINE");
        agentTasks_.start([guard, client, oldSerial, oldPort, offline] {
            if (client)
                client->close();
            if (!offline && !oldSerial.empty() && oldPort)
                testtools::AdbAgentDeployment().removeForward(oldSerial, oldPort);
            if (guard)
                QMetaObject::invokeMethod(
                    guard,
                    [guard] {
                        if (guard) {
                            guard->agentCleanupPending_ = false;
                            guard->updateAgentControls();
                        }
                    },
                    Qt::QueuedConnection);
        });
    }
    agentAdbSerial_.clear();
    agentForwardPort_ = 0;
    agentSummary_ = QStringLiteral("未连接");
    setAgentStatus(QStringLiteral("板端服务 · 未连接"),
                   StatusTone::Neutral,
                   reason.isEmpty() ? QStringLiteral("尚未连接设备") : reason);
    if (agentCapabilitiesValue_) {
        agentCapabilitiesValue_->setText(QStringLiteral("可用功能：—"));
        agentCapabilitiesValue_->setToolTip({});
    }
    if (agentVersionValue_) {
        agentVersionValue_->setText(
            QStringLiteral("应用要求：v%1 · 设备：—")
                .arg(QString::fromLatin1(testtools::AdbAgentDeployment::RequiredAgentVersion)));
    }
    for (QLabel* label : {agentModelValue_,
                          agentManufacturerValue_,
                          agentHardwareValue_,
                          agentOsValue_,
                          agentBuildValue_,
                          agentSecurityValue_}) {
        if (label) {
            label->setText(QStringLiteral("—"));
            label->setToolTip({});
        }
    }
    if (!reason.isEmpty()) {
        setActivity(reason);
    }
    updateAgentControls();
}

void MainWindow::setAgentStatus(const QString& text, StatusTone tone, const QString& detail)
{
    serviceState_ = agentConnecting_            ? testtools::AgentServiceState::Connecting
                    : agentClient_              ? testtools::AgentServiceState::Ready
                    : tone == StatusTone::Error ? testtools::AgentServiceState::Failed
                                                : testtools::AgentServiceState::Disconnected;
    serviceDetail_ = detail;
    updateServicePresentation();
    if (!agentStatusLabel_)
        return;
    agentStatusLabel_->setText(text);
    QString objectName = QStringLiteral("topStatusNeutral");
    if (tone == StatusTone::Good) {
        objectName = QStringLiteral("topStatusGood");
    } else if (tone == StatusTone::Warning) {
        objectName = QStringLiteral("topStatusWarn");
    } else if (tone == StatusTone::Error) {
        objectName = QStringLiteral("topStatusError");
    }
    agentStatusLabel_->setObjectName(objectName);
    agentStatusLabel_->setToolTip(detail);
    agentStatusLabel_->style()->unpolish(agentStatusLabel_);
    agentStatusLabel_->style()->polish(agentStatusLabel_);
    if (agentConnectionDetail_) {
        agentConnectionDetail_->setText(detail.isEmpty() ? text : detail);
    }
}

void MainWindow::updateServicePresentation()
{
    if (!topServiceButton_)
        return;
    using State = testtools::AgentServiceState;
    const auto state = passiveAgentProbe_ ? State::Disconnected : serviceState_;
    const QString label = state == State::Ready        ? QStringLiteral("● 板端服务：已就绪")
                          : state == State::Connecting ? QStringLiteral("● 板端服务：正在启动…")
                          : state == State::Failed     ? QStringLiteral("● 板端服务：连接失败")
                                                       : QStringLiteral("● 板端服务：未连接");
    const QString tone = state == State::Ready    ? "ready"
                         : state == State::Failed ? "failed"
                         : state == State::Connecting ? "working"
                                                  : "neutral";
    topServiceStatus_->setText(state == State::Connecting && !serviceDetail_.isEmpty()
        ? QStringLiteral("● 板端服务：%1").arg(topServiceStatus_->fontMetrics().elidedText(serviceDetail_, Qt::ElideRight, 180))
        : label);
    topServiceStatus_->setToolTip(serviceDetail_.isEmpty()
        ? QStringLiteral("板端服务提供设备串口和只读信息；PC端串口、ADB 文件、Shell 和投屏独立可用。")
        : serviceDetail_);
    topServiceButton_->setText(state == State::Ready ? QStringLiteral("服务详情")
        : state == State::Connecting ? QStringLiteral("启动中…")
        : state == State::Failed ? QStringLiteral("重试连接") : QStringLiteral("连接服务"));
    topServiceButton_->setToolTip(
        selectedDevice().state != "device" ? QStringLiteral("请先连接并选择在线且已授权的 ADB 设备")
        : serviceDetail_.isEmpty()
            ? QStringLiteral("准备 Android 板端服务可能请求 Root；文件传输、终端和投屏可独立使用。")
            : serviceDetail_);
    topServiceButton_->setEnabled(
        !shuttingDown_ && !closeRequested_ && !maintenanceBlocking_ &&
        (!agentConnecting_ || passiveAgentProbe_) && !agentCleanupPending_ &&
        (agentClient_ || (!agentSerialBusy() && selectedDevice().state == "device")) &&
        !qEnvironmentVariableIsSet("TESTTOOLS_OFFLINE"));
    const bool needsConnection = state == State::Disconnected || state == State::Failed;
    if (topServiceButton_->property("needsConnection").toBool() != needsConnection) {
        topServiceButton_->setProperty("needsConnection", needsConnection);
        topServiceButton_->style()->unpolish(topServiceButton_);
        topServiceButton_->style()->polish(topServiceButton_);
    }
    if (topServiceButton_->property("serviceState").toString() != tone) {
        topServiceButton_->setProperty("serviceState", tone);
        topServiceButton_->style()->unpolish(topServiceButton_);
        topServiceButton_->style()->polish(topServiceButton_);
        topServiceStatus_->setProperty("serviceState", tone);
        topServiceStatus_->style()->unpolish(topServiceStatus_);
        topServiceStatus_->style()->polish(topServiceStatus_);
    }
    if (serialWorkspace_)
        serialWorkspace_->setServiceStatus(state, serviceDetail_, agentCleanupPending_);
    if (terminalWorkspace_)
        terminalWorkspace_->setServiceStatus(state, serviceDetail_, agentCleanupPending_);
}

bool MainWindow::agentSerialBusy() const
{
    return (serialWorkspace_ && serialWorkspace_->hasActiveAgentConnection()) ||
           (terminalWorkspace_ && terminalWorkspace_->hasActiveAgentConnection());
}

void MainWindow::checkServiceHealth()
{
    agentTasks_.reapFinished();
    if (shuttingDown_ || closeRequested_ || serviceProbeActive_ || agentConnecting_ ||
        !agentClient_ || !agentForwardPort_ ||
        (boardInfoPanel_ && boardInfoPanel_->snapshot().active) ||
        selectedDevice().serial != agentAdbSerial_ ||
        qEnvironmentVariableIsSet("TESTTOOLS_OFFLINE"))
        return;
    serviceProbeActive_ = true;
    const auto generation = agentGeneration_;
    const auto port = agentForwardPort_;
    QPointer<MainWindow> guard(this);
    agentTasks_.start([guard, generation, port] {
        std::string error;
        const bool healthy = testtools::AgentConnectionService::checkConnection(port, error);
        if (guard)
            QMetaObject::invokeMethod(
                guard,
                [guard, generation, healthy, error] {
                    if (!guard)
                        return;
                    guard->serviceProbeActive_ = false;
                    if (guard->shuttingDown_ || guard->closeRequested_ ||
                        generation != guard->agentGeneration_ || !guard->agentClient_)
                        return;
                    if (healthy) {
                        guard->serviceHealthFailures_ = 0;
                    } else if (++guard->serviceHealthFailures_ >= 2) {
                        guard->disconnectAgent(QStringLiteral("板端服务连接已失效，请重试。"));
                        guard->setAgentStatus(
                            QStringLiteral("板端服务 · 连接失败"),
                            StatusTone::Error,
                            QStringLiteral("板端服务连接已失效：%1").arg(fromUtf8(error)));
                    }
                },
                Qt::QueuedConnection);
    });
}

void MainWindow::updateAgentControls()
{
    updateAdbTerminalControls();
    if (!deployAgentButton_) {
        return;
    }
    const bool connected = static_cast<bool>(agentClient_);
    const auto device = selectedDevice();
    deviceCombo_->setToolTip(deviceCombo_->currentText());
    const bool online = !device.serial.empty() && device.state == "device";
    deployAgentButton_->setEnabled(online && !maintenanceBlocking_ &&
                                   (!agentConnecting_ || passiveAgentProbe_) &&
                                   !agentCleanupPending_ && !connected && !agentSerialBusy());
    disconnectAgentButton_->setEnabled(connected && !agentSerialBusy());
    stopAgentButton_->setEnabled(
        online && !maintenanceBlocking_ && !agentConnecting_ && !agentSerialBusy() &&
        !(boardInfoPanel_ && boardInfoPanel_->busyForDevice(device.serial)));
    updateServicePresentation();
    deployAgentButton_->setText(agentConnecting_ && !passiveAgentProbe_
                                    ? QStringLiteral("正在连接…")
                                    : QStringLiteral("连接板端服务"));
    deployAgentButton_->setToolTip(
        QStringLiteral("准备 Android 板端服务可能请求 Root，以访问设备串口；不会自动 Remount。"));
    if (agentDeviceValue_) {
        agentDeviceValue_->setText(
            online ? QStringLiteral("目标设备：%1").arg(deviceDisplayName(device))
                   : QStringLiteral("目标设备：未选择已连接设备"));
        agentDeviceValue_->setToolTip(
            online ? QStringLiteral("设备编号：%1").arg(fromUtf8(device.serial)) : QString{});
    }
}

void MainWindow::refreshDevices()
{
    startDeviceScan(false);
}

void MainWindow::openAdbTerminal()
{
    const auto device = selectedDevice();
    if (shuttingDown_ || maintenanceBlocking_ || terminalLaunching_ || qEnvironmentVariableIsSet("TESTTOOLS_OFFLINE") ||
        device.serial.empty() || device.state != "device") {
        return;
    }
    terminalLaunching_ = true;
    updateAdbTerminalControls();
    setActivity(QStringLiteral("正在为 %1 打开 ADB 终端…").arg(fromUtf8(device.serial)));
    QPointer<MainWindow> guard(this);
    // 在点击瞬间捕获目标设备快照；外部独立终端在整个生命周期内固定绑定该设备。
    terminalTasks_.start([guard, device] {
        const auto result = testtools::AdbTerminalService().open(device);
        QMetaObject::invokeMethod(guard, [guard, device, result] {
            if (!guard || guard->shuttingDown_) {
                return;
            }
            guard->terminalLaunching_ = false;
            guard->updateAdbTerminalControls();
            const QString identity =
                result.effectiveUid == 0 ? QStringLiteral("Root（UID 0）")
                : result.effectiveUid >= 0
                    ? QStringLiteral("普通权限（UID %1）").arg(result.effectiveUid)
                    : QStringLiteral("权限未知");
            if (guard->selectedDevice().serial == device.serial)
                guard->adbShellIdentity_->setText(
                    QStringLiteral("启动时 Shell 权限：%1 · 沿用设备当前状态").arg(identity));
            guard->setActivity(
                result.started
                    ? QStringLiteral("已为 %1 打开 ADB 终端 · %2（%3）")
                          .arg(fromUtf8(device.serial), identity, fromUtf8(result.host))
                    : QStringLiteral("ADB 终端打开失败：%1").arg(fromUtf8(result.error)));
        }, Qt::QueuedConnection);
    });
}

void MainWindow::updateAdbTerminalControls()
{
    if (!adbTerminalButton_) {
        return;
    }
    const auto device = selectedDevice();
    const bool online = !device.serial.empty() && device.state == "device";
    for (auto* button : terminalButtons_) {
        button->setEnabled(online && !maintenanceBlocking_ && !terminalLaunching_ && !shuttingDown_ && !closeRequested_ &&
                           !qEnvironmentVariableIsSet("TESTTOOLS_OFFLINE"));
        button->setText(terminalLaunching_ ? QStringLiteral("正在打开…")
                                           : QStringLiteral("打开 ADB 终端 ↗"));
        button->setToolTip(online
            ? QStringLiteral("在系统终端打开 %1 的 Shell，无需连接 Agent").arg(fromUtf8(device.serial))
            : QStringLiteral("请先选择在线且已授权的 ADB 设备"));
    }
}

void MainWindow::startDeviceScan(bool automatic)
{
    if (qEnvironmentVariableIsSet("TESTTOOLS_OFFLINE")) return;
    if (shuttingDown_ || closeRequested_ || maintenanceBlocking_ || scanning_ || agentConnecting_ ||
        (automatic && deviceCombo_->view()->isVisible())) {
        return;
    }
    scanning_ = true;
    automaticScan_ = automatic;
    if (!automatic) {
        setAdbStatus(QStringLiteral("设备 · 检测中"), StatusTone::Neutral);
        setActivity(QStringLiteral("正在查找已连接设备"));
    }
    if (!automatic)
        updateDeviceControls();

    QPointer<MainWindow> guard(this);
    scanTasks_.start([guard, automatic] {
        testtools::DeviceDiscovery discovery;
        auto result = discovery.scanAdbDevices();
        QMetaObject::invokeMethod(
            guard,
            [guard, result = std::move(result), automatic]() mutable {
                if (guard && !guard->shuttingDown_) {
                    guard->applyDeviceScan(std::move(result), automatic);
                }
            },
            Qt::QueuedConnection);
    });
}

void MainWindow::applyDeviceScan(testtools::DeviceScanResult result, bool automatic)
{
    if (shuttingDown_) {
        return;
    }
    if (agentConnecting_ || maintenanceBlocking_ ||
        (automatic && deviceCombo_->view()->isVisible())) {
        scanning_ = false;
        updateDeviceControls();
        return;
    }
    const QString signature = deviceScanSignature(result);
    if (!result.error.empty()) {
        scanning_ = false;
        if (!deviceScanFailed_ || !automatic) {
            setAdbStatus(QStringLiteral("ADB · 检查失败"), StatusTone::Error);
            setActivity(QStringLiteral("设备检查失败；已保留当前选择，可手动刷新重试"));
        }
        deviceScanFailed_ = true;
        updateDeviceControls();
        return;
    }
    const bool recovered = deviceScanFailed_;
    deviceScanFailed_ = false;
    // A manual refresh can resolve a retained placeholder even when the list is
    // unchanged; otherwise the user remains stuck without an active device.
    const bool selectOnRefresh = !automatic && selectedDevice().serial.empty() &&
                                 !result.devices.empty();
    if (signature == lastDeviceScanSignature_ && !selectOnRefresh) {
        scanning_ = false;
        if (recovered || !automatic)
            applySelectedDevice();
        if (!automatic && selectedDevice().state == "device" && !agentClient_ &&
            !agentConnecting_ && !agentCleanupPending_)
            connectSelectedAgent(false);
        updateDeviceControls();
        return;
    }
    lastDeviceScanSignature_ = signature;
    const QString previousSerial = deviceCombo_->currentData().toString();
    const QSignalBlocker blocker(deviceCombo_);
    deviceCombo_->clear();

    int firstOnline = -1;
    int onlineCount = 0;
    int restoreIndex = -1;
    for (const auto& device : result.devices) {
        QString label = deviceDisplayName(device);
        if (device.state != "device") {
            label += QStringLiteral(" [%1]").arg(deviceStateText(device.state));
        }
        deviceCombo_->addItem(label, fromUtf8(device.serial));
        const int index = deviceCombo_->count() - 1;
        deviceCombo_->setItemData(
            index,
            QStringLiteral("设备编号：%1").arg(fromUtf8(device.serial)),
            Qt::ToolTipRole);
        deviceCombo_->setItemData(index, fromUtf8(device.state), Qt::UserRole + 1);
        deviceCombo_->setItemData(index, fromUtf8(device.description), Qt::UserRole + 2);
        if (device.state == "device") {
            if (firstOnline < 0) {
                firstOnline = index;
            }
            ++onlineCount;
        }
        if (fromUtf8(device.serial) == previousSerial) {
            restoreIndex = index;
        }
    }

    scanning_ = false;
    if (result.devices.empty()) {
        deviceCombo_->addItem(QStringLiteral("未发现设备"));
        deviceCombo_->setCurrentIndex(0);
        setAdbStatus(QStringLiteral("设备 · 未连接"), StatusTone::Neutral);
        setActivity(automatic && !previousSerial.isEmpty()
                        ? QStringLiteral("当前设备已拔出；PC端串口仍可使用")
                        : QStringLiteral("未发现设备；PC端串口仍可使用"));
    } else if (onlineCount == 0) {
        if (automatic && restoreIndex < 0) {
            deviceCombo_->insertItem(0, previousSerial.isEmpty()
                                            ? QStringLiteral("请选择设备")
                                            : QStringLiteral("原设备已断开，请重新选择"));
            deviceCombo_->setCurrentIndex(0);
        } else {
            deviceCombo_->setCurrentIndex(restoreIndex >= 0 ? restoreIndex : 0);
        }
        setAdbStatus(QStringLiteral("设备 · 待授权"), StatusTone::Warning);
        setActivity(QStringLiteral("已发现设备，请在设备端确认授权"));
    } else {
        if (automatic && restoreIndex < 0 &&
            (deviceSelectionEstablished_ || onlineCount != 1)) {
            deviceCombo_->insertItem(0, previousSerial.isEmpty()
                                            ? QStringLiteral("请选择设备")
                                            : QStringLiteral("原设备已断开，请重新选择"));
            deviceCombo_->setCurrentIndex(0);
        } else {
            deviceCombo_->setCurrentIndex(restoreIndex >= 0 ? restoreIndex : firstOnline);
        }
        setAdbStatus(QStringLiteral("ADB · 已连接"));
        setActivity(automatic ? QStringLiteral("设备列表已自动更新")
                              : QStringLiteral("设备列表已更新"));
    }
    applySelectedDevice();
    updateDeviceControls();
}

testtools::AdbDevice MainWindow::selectedDevice() const
{
    testtools::AdbDevice device;
    if (!deviceCombo_ || deviceCombo_->currentData().toString().isEmpty()) {
        return device;
    }
    device.serial = toUtf8(deviceCombo_->currentData().toString());
    device.state = toUtf8(deviceCombo_->currentData(Qt::UserRole + 1).toString());
    device.description = toUtf8(deviceCombo_->currentData(Qt::UserRole + 2).toString());
    return device;
}

void MainWindow::applySelectedDevice()
{
    if (!serialWorkspace_) {
        return;
    }
    const auto device = selectedDevice();
    if (!device.serial.empty())
        deviceSelectionEstablished_ = true;
    if (device.state == "device")
        setAdbStatus(QStringLiteral("ADB · 已连接"), StatusTone::Good);
    else if (device.serial.empty()) {
        int online = 0;
        for (int i = 0; i < deviceCombo_->count(); ++i)
            if (deviceCombo_->itemData(i, Qt::UserRole + 1).toString() == QStringLiteral("device"))
                ++online;
        setAdbStatus(online ? QStringLiteral("ADB · 已发现 %1 台，待选择").arg(online)
                            : QStringLiteral("ADB · 未连接"),
                     online ? StatusTone::Warning : StatusTone::Neutral);
    } else
        setAdbStatus(QStringLiteral("ADB · %1").arg(deviceStateText(device.state)),
                     StatusTone::Neutral);
    const bool identityChanged =
        appliedDevice_.serial != device.serial || appliedDevice_.state != device.state;
    const bool changed = identityChanged || appliedDevice_.description != device.description;
    appliedDevice_ = device;
    if (!changed) {
        updateAgentControls();
        return;
    }
    if (identityChanged && adbShellIdentity_)
        adbShellIdentity_->setText(QStringLiteral("打开时核对实际 Shell 权限"));
    if (identityChanged && agentConnecting_)
        disconnectAgent(QStringLiteral("设备上下文已改变，原服务检查已取消"));
    if (screenMirrorPanel_) screenMirrorPanel_->setDevice(device);
    if (maintenancePanel_) maintenancePanel_->setDevice(device);
    if (boardInfoPanel_)
        boardInfoPanel_->setTarget(device);
    if (fileWorkspace_) fileWorkspace_->setDevice(device);
    if (!agentAdbSerial_.empty() &&
        (agentAdbSerial_ != device.serial || device.state != "device")) {
        disconnectAgent(QStringLiteral("已切换设备，原连接已断开"));
    }
    serialWorkspace_->setAdbDevice(device);
    if (terminalWorkspace_)
        terminalWorkspace_->setAdbDevice(device);
    updateAgentControls();
    if (identityChanged && !maintenanceBlocking_ && !agentClient_ && !agentConnecting_ &&
        device.state == "device" && !device.serial.empty()) {
        connectSelectedAgent(false);
    }
}

void MainWindow::showPage(int pageIndex)
{
    if (!pages_ || pageIndex < 0 || pageIndex >= pages_->count()) {
        return;
    }
    pages_->setCurrentIndex(pageIndex);
    if (boardInfoPanel_)
        boardInfoPanel_->setDashboardVisible(pageIndex == DeviceInfoPage);
    if (pageIndex == FileTransferPage || pageIndex == MirrorPage) {
        lastAdbPage_ = pageIndex; setAdbExpanded(true);
    }
    if (pageIndex == SerialPage || pageIndex == TerminalPage) {
        lastSerialPage_ = pageIndex;
        setSerialExpanded(true);
    }
    if (pageIndex == TerminalPage && terminalWorkspace_)
        terminalWorkspace_->selectWorkspaceMode(QStringLiteral("terminal"));
    updateNavigation();
}

void MainWindow::setSerialExpanded(bool expanded)
{
    serialChildren_->setVisible(expanded);
    serialExpandButton_->setIcon(testtools::ui::navigationChevron(expanded, serialGroupButton_->isChecked()));
    serialExpandButton_->setToolTip(expanded ? QStringLiteral("收起串口子页面") : QStringLiteral("展开串口子页面"));
}

void MainWindow::updateNavigation()
{
    if (!pages_) return;
    serialGroupButton_->setChecked(pages_->currentIndex() == SerialPage ||
                                   pages_->currentIndex() == TerminalPage);
    adbGroupButton_->setChecked(pages_->currentIndex() == FileTransferPage ||
                                pages_->currentIndex() == MirrorPage);
    for (auto* row : {serialGroupRow_, adbGroupRow_}) {
        const bool active = row == serialGroupRow_ ? serialGroupButton_->isChecked() : adbGroupButton_->isChecked();
        row->setProperty("active", active);
        row->style()->unpolish(row);
        row->style()->polish(row);
    }
    serialExpandButton_->setIcon(testtools::ui::navigationChevron(!serialChildren_->isHidden(), serialGroupButton_->isChecked()));
    adbExpandButton_->setIcon(testtools::ui::navigationChevron(!adbChildren_->isHidden(), adbGroupButton_->isChecked()));
    for (auto* button : navigationButtons_) {
        const auto mode = button->property("serialMode").toString();
        button->setChecked(button->property("pageIndex").toInt() == pages_->currentIndex() &&
            (mode.isEmpty() || pages_->currentIndex() == TerminalPage ||
             (serialWorkspace_ && mode == serialWorkspace_->currentWorkspaceMode())));
    }
}

void MainWindow::setAdbStatus(const QString& text, StatusTone tone)
{
    adbStatusLabel_->setText(QStringLiteral("● ") + QString(text).replace(QStringLiteral(" · "), QStringLiteral("：")));
    QString objectName = QStringLiteral("topStatusNeutral");
    switch (tone) {
    case StatusTone::Good:
        objectName = QStringLiteral("topStatusGood");
        break;
    case StatusTone::Warning:
        objectName = QStringLiteral("topStatusWarn");
        break;
    case StatusTone::Error:
        objectName = QStringLiteral("topStatusError");
        break;
    case StatusTone::Neutral:
        break;
    }
    adbStatusLabel_->setObjectName(objectName);
    adbStatusLabel_->style()->unpolish(adbStatusLabel_);
    adbStatusLabel_->style()->polish(adbStatusLabel_);
}

void MainWindow::setActivity(const QString& message)
{
    if (activityLabel_) {
        activityLabel_->setText(QStringLiteral("[%1] %2").arg(
            QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")), message));
        activityLabel_->setToolTip(activityLabel_->text());
    }
}

bool MainWindow::recordingsPending() const
{
    return (serialWorkspace_ && (serialWorkspace_->hasPendingRecordings() ||
                                 serialWorkspace_->hasPendingBackgroundWork())) ||
           (terminalWorkspace_ && (terminalWorkspace_->hasPendingRecordings() ||
                                   terminalWorkspace_->hasPendingBackgroundWork()));
}

void MainWindow::updateTasks(bool rebuildMenu)
{
    if (!taskButton_ || shuttingDown_) return;
    int count = 0;
    QStringList descriptions;
    if (rebuildMenu) taskMenu_->clear();
    for (auto* workspace : {static_cast<SerialEndpointWorkspace*>(serialWorkspace_),
                            static_cast<SerialEndpointWorkspace*>(terminalWorkspace_)}) if (workspace) {
        for (const auto& task : workspace->activeTasks()) {
            ++count; descriptions.push_back(task.title);
            if (rebuildMenu) {
                const QString verb = task.kind == SerialEndpointWorkspace::BackgroundTask::Kind::Recording ? QStringLiteral("停止录制")
                    : task.kind == SerialEndpointWorkspace::BackgroundTask::Kind::ScheduledSend ? QStringLiteral("停止计划发送") : QStringLiteral("停止压测");
                auto* action = taskMenu_->addAction(task.title + QStringLiteral("  ·  ") + (task.stoppable ? verb : QStringLiteral("等待保存")));
                action->setEnabled(task.stoppable);
                connect(action, &QAction::triggered, this, [this, workspace, task] { workspace->stopTask(task); updateTasks(); });
            }
        }
    }
    if (fileWorkspace_) {
        const auto state = fileWorkspace_->transferSnapshot();
        if (state.active) {
            ++count;
            const bool transfer = state.operation == testtools::FileOperation::Transfer;
            const auto text = QStringLiteral("%1 · %2 · %3/%4%5").arg(transfer ? QStringLiteral("文件传输") : QStringLiteral("文件操作"), state.remote ? fromUtf8(state.serial) : QStringLiteral("本地"))
                .arg(state.records.size()).arg(state.total).arg(state.cancelling ? QStringLiteral(" · 取消中") : QString());
            descriptions.push_back(text);
            if (rebuildMenu) {
                auto* action = taskMenu_->addAction(text + (transfer ? QStringLiteral("  ·  取消传输") : QStringLiteral("  ·  停止文件操作")));
                action->setEnabled(!state.cancelling);
                connect(action, &QAction::triggered, this, [this, id = state.id] { fileWorkspace_->cancelTransfer(id); updateTasks(); });
            }
        }
    }
    if (maintenancePanel_) {
        const auto state=maintenancePanel_->snapshot();
        if(state.active) {
            ++count;
            const auto label=QStringLiteral("诊断与维护 · %1 · %2").arg(fromUtf8(state.request.device.serial),fromUtf8(state.progress.message));
            descriptions.push_back(label);
            if(rebuildMenu) {
                auto* action=taskMenu_->addAction(label+QStringLiteral(" · 停止任务"));
                action->setEnabled(!state.cancelling);
                connect(action,&QAction::triggered,this,[this,id=state.id]{maintenancePanel_->cancelTask(id);updateTasks();});
            }
        }
    }
    if (boardInfoPanel_) {
        const auto state = boardInfoPanel_->snapshot();
        if (state.active || (state.remotePending && boardInfoPanel_->busyForDevice(state.serial))) {
            ++count;
            const auto label = QStringLiteral("硬件信息 · %1 · %2")
                                   .arg(fromUtf8(state.serial), fromUtf8(state.message));
            descriptions.push_back(label);
            if (rebuildMenu) {
                auto* action = taskMenu_->addAction(
                    label + (state.active ? QStringLiteral(" · 停止读取")
                                          : QStringLiteral(" · 远端结束状态未确认")));
                action->setEnabled(state.active && !state.cancelling);
                connect(action, &QAction::triggered, this, [this, id = state.id] {
                    boardInfoPanel_->cancelTask(id);
                    updateTasks();
                });
            }
        }
    }
    taskButton_->setText(count ? QStringLiteral("后台任务 %1").arg(count) : QStringLiteral("无后台任务"));
    taskButton_->setVisible(count > 0);
    taskButton_->setToolTip(descriptions.join(QLatin1Char('\n')));
    taskButton_->setEnabled(count > 0 && !closeRequested_);
}

void MainWindow::updateDeviceControls()
{
    updateServicePresentation();
    updateAdbTerminalControls();
    const bool enabled = !qEnvironmentVariableIsSet("TESTTOOLS_OFFLINE") &&
                         (!scanning_ || automaticScan_) && !agentSerialBusy() &&
                         !agentConnecting_ && !maintenanceBlocking_;
    refreshButton_->setEnabled(enabled);
    bool hasDevice = false;
    for (int index = 0; index < deviceCombo_->count(); ++index) {
        if (!deviceCombo_->itemData(index).toString().isEmpty()) {
            hasDevice = true;
            break;
        }
    }
    deviceCombo_->setEnabled(enabled && hasDevice);
}

testtools::MaintenanceConflicts MainWindow::maintenanceConflicts(const testtools::AdbDevice& device) const
{
    testtools::MaintenanceConflicts result;
    if(fileWorkspace_) {
        const auto state=fileWorkspace_->transferSnapshot();
        result.files=state.active && state.remote && state.serial==device.serial;
    }
    result.serial=(serialWorkspace_ && serialWorkspace_->hasActiveAgentConnection(device.serial)) ||
                  (terminalWorkspace_ && terminalWorkspace_->hasActiveAgentConnection(device.serial));
    result.board = boardInfoPanel_ && boardInfoPanel_->busyForDevice(device.serial);
    result.mirror=screenMirrorPanel_ && screenMirrorPanel_->busyForDevice(device.serial);
    result.preparingService = (agentConnecting_ || agentCleanupPending_ || terminalLaunching_ ||
                               (scanning_ && !automaticScan_)) &&
                              selectedDevice().serial == device.serial;
    return result;
}
void MainWindow::updateMaintenanceState()
{
    const auto state=maintenanceService_->snapshot();
    const bool blocked=state.active && testtools::disruptive(state.request.action);
    const bool previous=maintenanceBlocking_;
    maintenanceBlocking_=blocked;
    if(blocked && !previous) disconnectAgent(QStringLiteral("设备维护中；完成后重新检查连接"));
    if(fileWorkspace_) fileWorkspace_->setDeviceMaintenance(blocked);
    for (auto* workspace : {static_cast<SerialEndpointWorkspace*>(serialWorkspace_),
                            static_cast<SerialEndpointWorkspace*>(terminalWorkspace_)})
        if (workspace) workspace->setDeviceMaintenance(blocked);
    if(screenMirrorPanel_) screenMirrorPanel_->setMaintenanceDevice(blocked?state.request.device.serial:std::string{});
    updateDeviceControls(); updateAgentControls(); updateTasks();
    if(previous && !blocked && !closeRequested_) refreshDevices();
}
