#include "main_window.h"

#include "application/agent/adb_agent_deployment.h"
#include "application/agent/agent_connection_service.h"
#include "application/device/adb_terminal_service.h"
#include "theme.h"
#include "ui/pages/serial_workspace.h"
#include "ui/pages/file_transfer_workspace.h"
#include "ui/screen_mirror_panel.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QTimer>
#include <QtGui/QCloseEvent>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QMetaObject>
#include <QtCore/QPointer>
#include <QtCore/QSignalBlocker>
#include <QtGui/QIcon>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
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

QString fromUtf8(const std::string& text)
{
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

QJsonObject jsonResultObject(const std::string& json)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(
        QByteArray(json.data(), static_cast<qsizetype>(json.size())), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return {};
    }
    return document.object().value(QStringLiteral("result")).toObject();
}

QString infoText(const QJsonObject& object, const char* key, const QString& fallback = {})
{
    const QString value = object.value(QString::fromLatin1(key)).toString().trimmed();
    return value.isEmpty() ? fallback : value;
}

bool hasCapability(const std::string& response, const QString& capabilityId)
{
    const QJsonArray capabilities =
        jsonResultObject(response).value(QStringLiteral("capabilities")).toArray();
    for (const QJsonValue& capability : capabilities) {
        if (capability.toObject().value(QStringLiteral("id")).toString() == capabilityId) {
            return true;
        }
    }
    return false;
}

QString deviceDisplayName(const serialdemo::AdbDevice& device)
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

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    buildUi();
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
        connect(deviceRefreshTimer_, &QTimer::timeout, this, [this] { startDeviceScan(true); });
        deviceRefreshTimer_->start();
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
    if (!shuttingDown_ && (!mirrorShutdownComplete_ || !fileShutdownComplete_)) {
        event->ignore();
        closeRequested_ = true;
        if (serviceHealthTimer_)
            serviceHealthTimer_->stop();
        if (deviceRefreshTimer_) deviceRefreshTimer_->stop();
        centralWidget()->setEnabled(false);
        updateAdbTerminalControls();
        setActivity(QStringLiteral("正在结束文件传输和投屏…"));
        if (fileWorkspace_) fileWorkspace_->shutdown();
        else fileShutdownComplete_ = true;
        if (screenMirrorPanel_) screenMirrorPanel_->shutdown();
        else mirrorShutdownComplete_ = true;
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
    if (serviceHealthTimer_)
        serviceHealthTimer_->stop();
    if (fileWorkspace_) fileWorkspace_->shutdown();
    if (screenMirrorPanel_) screenMirrorPanel_->shutdown();
    updateAdbTerminalControls();
    const std::string selectedSerial = selectedDevice().serial;
    if (serialWorkspace_) {
        serialWorkspace_->prepareForShutdown();
    }
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
    // A deployment may have completed after the event loop began shutting
    // down, before its queued result could transfer ownership of the port.
    // Remove only forwards targeting this Agent's fixed control port.
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
        showPage(SerialPage);
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
    return serialWorkspace_ && serialWorkspace_->setWorkspaceModeForDiagnostics(modeName);
}

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("TestTools"));
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
    bodyLayout->addWidget(pages_, 1);
    root->addWidget(body, 1);

    auto* activity = new QFrame(surface);
    activity->setObjectName(QStringLiteral("activityBar"));
    activity->setMinimumHeight(38);
    activity->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto* activityLayout = new QHBoxLayout(activity);
    activityLayout->setContentsMargins(18, 9, 18, 9);
    auto* activityTitle = new QLabel(QStringLiteral("活动"), activity);
    activityTitle->setObjectName(QStringLiteral("activityTitle"));
    activityLabel_ = new QLabel(QStringLiteral("正在初始化设备列表"), activity);
    activityLabel_->setObjectName(QStringLiteral("activityMessage"));
    activityLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    activityLayout->addWidget(activityTitle);
    activityLayout->addWidget(activityLabel_, 1);
    root->addWidget(activity);

    setCentralWidget(surface);
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
    auto* title = new QLabel(QStringLiteral("TestTools"), bar);
    title->setObjectName(QStringLiteral("appTitle"));
    auto* subtitle = new QLabel(QStringLiteral("板卡与模块调试工作台"), bar);
    subtitle->setObjectName(QStringLiteral("appSubtitle"));
    brand->addWidget(title);
    brand->addWidget(subtitle);
    layout->addLayout(brand);
    layout->addSpacing(8);

    auto* caption = new QLabel(QStringLiteral("设备"), bar);
    caption->setObjectName(QStringLiteral("topCaption"));
    deviceCombo_ = new QComboBox(bar);
    deviceCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    deviceCombo_->setMinimumContentsLength(15);
    deviceCombo_->setMaximumWidth(280);
    adbStatusLabel_ = new QLabel(QStringLiteral("设备 · 检测中"), bar);
    adbStatusLabel_->setObjectName(QStringLiteral("topStatusNeutral"));
    refreshButton_ = new QPushButton(QStringLiteral("刷新设备"), bar);
    refreshButton_->setObjectName(QStringLiteral("topGhostButton"));

    layout->addWidget(caption);
    layout->addWidget(deviceCombo_);
    layout->addWidget(adbStatusLabel_);
    topServiceButton_ = new QPushButton(QStringLiteral("调试服务 · 未连接"), bar);
    topServiceButton_->setObjectName(QStringLiteral("serviceStatusButton"));
    topServiceButton_->setProperty("diagnosticObjectName", QStringLiteral("serviceStatusButton"));
    layout->addWidget(topServiceButton_);
    connect(topServiceButton_, &QPushButton::clicked, this, [this] {
        if (agentClient_)
            showPage(DeviceInfoPage);
        else
            deploySelectedAgent();
    });
    layout->addStretch(1);
    layout->addWidget(refreshButton_);

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

    auto* serialRow = new QHBoxLayout;
    serialRow->setContentsMargins(0,0,0,0); serialRow->setSpacing(0);
    auto* serial = new QPushButton(QStringLiteral("串口调试"), rail);
    serialGroupButton_ = serial;
    serial->setCheckable(true);
    serial->setObjectName(QStringLiteral("navigationGroupButton"));
    serialExpandButton_ = new QToolButton(rail); serialExpandButton_->setFixedSize(32,38); serialExpandButton_->setAutoRaise(true);
    serialExpandButton_->setObjectName(QStringLiteral("serialExpandButton"));
    serialExpandButton_->setAccessibleName(QStringLiteral("展开或收起串口子页面"));
    serialRow->addWidget(serial,1); serialRow->addWidget(serialExpandButton_); layout->addLayout(serialRow);
    serialChildren_ = new QWidget(rail); serialChildren_->setObjectName(QStringLiteral("serialNavigationChildren"));
    auto* serialPages = new QVBoxLayout(serialChildren_);
    serialPages->setContentsMargins(18, 0, 0, 2);
    serialPages->setSpacing(4);
    serialPages->addWidget(addNavigationButton(QStringLiteral("单串口调试"), SerialPage, QStringLiteral("single")));
    serialPages->addWidget(addNavigationButton(QStringLiteral("双端调试"), SerialPage, QStringLiteral("dual")));
    serialPages->addWidget(addNavigationButton(QStringLiteral("串口终端"), SerialPage, QStringLiteral("terminal")));
    layout->addWidget(serialChildren_); setSerialExpanded(true);
    connect(serial, &QPushButton::clicked, this, [this]{showPage(SerialPage);});
    connect(serialExpandButton_, &QToolButton::clicked, this, [this]{setSerialExpanded(serialChildren_->isHidden());});
    auto* parentRow = new QHBoxLayout;
    parentRow->setContentsMargins(0, 0, 0, 0); parentRow->setSpacing(0);
    auto* adb = new QPushButton(QStringLiteral("ADB 调试"), rail);
    adbGroupButton_ = adb;
    adb->setCheckable(true);
    adb->setObjectName(QStringLiteral("navigationGroupButton"));
    adb->setProperty("diagnosticObjectName", QStringLiteral("adbNavigation"));
    adbExpandButton_ = new QToolButton(rail); adbExpandButton_->setArrowType(Qt::RightArrow);
    adbExpandButton_->setFixedSize(32, 38); adbExpandButton_->setAutoRaise(true);
    adbExpandButton_->setObjectName(QStringLiteral("adbExpandButton"));
    adbExpandButton_->setAccessibleName(QStringLiteral("展开或收起 ADB 子页面"));
    parentRow->addWidget(adb, 1); parentRow->addWidget(adbExpandButton_); layout->addLayout(parentRow);
    adbChildren_ = new QWidget(rail); adbChildren_->setObjectName(QStringLiteral("adbNavigationChildren"));
    auto* children = new QVBoxLayout(adbChildren_);
    children->setContentsMargins(18, 0, 0, 2);
    children->setSpacing(4);
    children->addWidget(addNavigationButton(QStringLiteral("文件传输"), FileTransferPage));
    children->addWidget(addNavigationButton(QStringLiteral("远程调试"), MirrorPage));
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
    adbExpandButton_->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
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

QWidget* MainWindow::buildMirrorPage()
{
    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content); layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    auto* shell = new QFrame(content);
    shell->setObjectName(QStringLiteral("featureCard"));
    auto* shellLayout = new QHBoxLayout(shell);
    shellLayout->setContentsMargins(16, 12, 16, 12);
    auto* shellText = new QVBoxLayout;
    auto* shellTitle = new QLabel(QStringLiteral("ADB Shell"), shell);
    shellTitle->setObjectName(QStringLiteral("sectionLabel"));
    auto* shellHint =
        new QLabel(QStringLiteral("在独立系统终端中操作当前设备；不需要连接调试服务。"), shell);
    shellHint->setObjectName(QStringLiteral("mutedLabel"));
    shellHint->setWordWrap(true);
    shellText->addWidget(shellTitle);
    shellText->addWidget(shellHint);
    shellLayout->addLayout(shellText, 1);
    adbTerminalButton_ = new QPushButton(QStringLiteral("打开 ADB 终端 ↗"), shell);
    adbTerminalButton_->setObjectName(QStringLiteral("adbTerminalButton"));
    adbTerminalButton_->setProperty("diagnosticObjectName", "adbTerminalButton");
    terminalButtons_.push_back(adbTerminalButton_);
    connect(adbTerminalButton_, &QPushButton::clicked, this, &MainWindow::openAdbTerminal);
    shellLayout->addWidget(adbTerminalButton_);
    layout->addWidget(shell);
    auto* mirrorTitle = new QLabel(QStringLiteral("Android 投屏"), content);
    mirrorTitle->setObjectName(QStringLiteral("sectionLabel"));
    layout->addWidget(mirrorTitle);
    screenMirrorPanel_ = new ScreenMirrorPanel(content, {}, true);
    screenMirrorPanel_->setMaximumWidth(700); layout->addWidget(screenMirrorPanel_); layout->addStretch();
    connect(screenMirrorPanel_, &ScreenMirrorPanel::activityChanged, this, &MainWindow::setActivity);
    connect(screenMirrorPanel_, &ScreenMirrorPanel::shutdownFinished, this, [this] {
        mirrorShutdownComplete_ = true; if (closeRequested_ && fileShutdownComplete_) close();
    });
    return wrapAdbPage(QStringLiteral("远程调试"), content);
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
        if (!serialMode.isEmpty() && !serialWorkspace_->selectWorkspaceMode(serialMode)) { updateNavigation(); return; }
        showPage(pageIndex);
    });
    navigationButtons_.push_back(button);
    return button;
}

QWidget* MainWindow::buildSerialPage()
{
    serialWorkspace_ = new SerialWorkspace(this);
    connect(serialWorkspace_,
            &SerialWorkspace::prepareServiceRequested,
            this,
            &MainWindow::deploySelectedAgent);
    connect(serialWorkspace_, &SerialWorkspace::workspaceModeChanged, this, [this] { updateNavigation(); });
    connect(serialWorkspace_, &SerialWorkspace::activityChanged, this, &MainWindow::setActivity);
    connect(serialWorkspace_, &SerialWorkspace::busyChanged, this, [this](bool busy) {
        serialBusy_ = busy;
        updateDeviceControls();
    });
    return wrapPage({},
                    {},
                    serialWorkspace_,
                    false);
}

QWidget* MainWindow::buildDeviceInfoPage()
{
    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    auto* connectionCard = new QFrame(content);
    connectionCard->setObjectName(QStringLiteral("featureCard"));
    auto* connectionLayout = new QVBoxLayout(connectionCard);
    connectionLayout->setContentsMargins(18, 16, 18, 16);
    connectionLayout->setSpacing(10);
    auto* connectionTitle = new QLabel(QStringLiteral("设备连接"), connectionCard);
    connectionTitle->setObjectName(QStringLiteral("sectionLabel"));
    auto* connectionBody = new QLabel(
        QStringLiteral(
            "连接时会自动完成兼容性检查和必要准备，不会自动执行串口发送或其他设备操作。"),
        connectionCard);
    connectionBody->setObjectName(QStringLiteral("cardBody"));
    connectionBody->setWordWrap(true);
    connectionLayout->addWidget(connectionTitle);
    agentStatusLabel_ = new QLabel(QStringLiteral("调试服务 · 未连接"), connectionCard);
    agentStatusLabel_->setObjectName(QStringLiteral("topStatusNeutral"));
    connectionLayout->addWidget(agentStatusLabel_, 0, Qt::AlignLeft);
    connectionLayout->addWidget(connectionBody);

    agentDeviceValue_ = new QLabel(QStringLiteral("目标设备：未选择"), connectionCard);
    agentDeviceValue_->setObjectName(QStringLiteral("cardBody"));
    connectionLayout->addWidget(agentDeviceValue_);

    auto* actions = new QHBoxLayout;
    deployAgentButton_ = new QPushButton(QStringLiteral("连接调试服务"), connectionCard);
    deployAgentButton_->setObjectName(QStringLiteral("primaryButton"));
    disconnectAgentButton_ = new QPushButton(QStringLiteral("断开调试服务"), connectionCard);
    stopAgentButton_ = new QPushButton(QStringLiteral("重置调试服务"), connectionCard);
    actions->addWidget(deployAgentButton_);
    actions->addWidget(disconnectAgentButton_);
    actions->addWidget(stopAgentButton_);
    actions->addStretch();
    connectionLayout->addLayout(actions);

    agentConnectionDetail_ = new QLabel(QStringLiteral("尚未连接设备"), connectionCard);
    agentConnectionDetail_->setObjectName(QStringLiteral("cardBody"));
    agentConnectionDetail_->setWordWrap(true);
    agentConnectionDetail_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    agentCapabilitiesValue_ = new QLabel(QStringLiteral("可用功能：—"), connectionCard);
    agentCapabilitiesValue_->setObjectName(QStringLiteral("cardBody"));
    agentCapabilitiesValue_->setWordWrap(true);
    agentCapabilitiesValue_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    connectionLayout->addWidget(agentConnectionDetail_);

    auto* versionTitle = new QLabel(QStringLiteral("连接兼容性"), connectionCard);
    versionTitle->setObjectName(QStringLiteral("sectionLabel"));
    agentVersionValue_ = new QLabel(
        QStringLiteral("应用要求：v%1 · 设备：—")
            .arg(QString::fromLatin1(testtools::AdbAgentDeployment::RequiredAgentVersion)),
        connectionCard);
    agentVersionValue_->setObjectName(QStringLiteral("cardBody"));
    agentVersionValue_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    connectionLayout->addWidget(versionTitle);
    connectionLayout->addWidget(agentVersionValue_);

    auto* boardTitle = new QLabel(QStringLiteral("板级信息"), connectionCard);
    boardTitle->setObjectName(QStringLiteral("sectionLabel"));
    connectionLayout->addWidget(boardTitle);
    auto* boardGrid = new QGridLayout;
    boardGrid->setHorizontalSpacing(18);
    boardGrid->setVerticalSpacing(7);
    boardGrid->setColumnMinimumWidth(0, 76);
    boardGrid->setColumnStretch(1, 1);
    const auto addBoardRow = [connectionCard,
                              boardGrid](int row, const QString& name, QLabel*& value) {
        auto* nameLabel = new QLabel(name, connectionCard);
        nameLabel->setObjectName(QStringLiteral("cardEyebrow"));
        value = new QLabel(QStringLiteral("—"), connectionCard);
        value->setObjectName(QStringLiteral("cardBody"));
        value->setWordWrap(true);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        boardGrid->addWidget(nameLabel, row, 0, Qt::AlignTop);
        boardGrid->addWidget(value, row, 1);
    };
    addBoardRow(0, QStringLiteral("板卡型号"), agentModelValue_);
    addBoardRow(1, QStringLiteral("厂商 / 产品"), agentManufacturerValue_);
    addBoardRow(2, QStringLiteral("硬件资源"), agentHardwareValue_);
    addBoardRow(3, QStringLiteral("系统内核"), agentOsValue_);
    addBoardRow(4, QStringLiteral("系统构建"), agentBuildValue_);
    addBoardRow(5, QStringLiteral("连接权限"), agentSecurityValue_);
    connectionLayout->addLayout(boardGrid);

    auto* capabilityTitle = new QLabel(QStringLiteral("可用功能"), connectionCard);
    capabilityTitle->setObjectName(QStringLiteral("sectionLabel"));
    connectionLayout->addWidget(capabilityTitle);
    connectionLayout->addWidget(agentCapabilitiesValue_);
    layout->addWidget(connectionCard);
    layout->addStretch();

    connect(deployAgentButton_, &QPushButton::clicked, this, &MainWindow::deploySelectedAgent);
    connect(disconnectAgentButton_, &QPushButton::clicked, this, [this] {
        disconnectAgent(QStringLiteral("设备连接已断开"));
    });
    connect(stopAgentButton_, &QPushButton::clicked, this, &MainWindow::stopSelectedAgent);
    updateAgentControls();
    return wrapPage(QStringLiteral("设备信息"),
                    QStringLiteral("查看当前设备，并在需要时维护连接"),
                    content);
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
    if (agentConnecting_ || agentCleanupPending_ || agentClient_ || shuttingDown_ ||
        closeRequested_) {
        return;
    }
    const auto device = selectedDevice();
    if (device.serial.empty() || device.state != "device") {
        setAgentStatus(QStringLiteral("调试服务 · 未连接"),
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
    setAgentStatus(
        allowDeploy ? QStringLiteral("调试服务 · 准备中") : QStringLiteral("调试服务 · 检查中"),
        StatusTone::Neutral,
        allowDeploy ? QStringLiteral("正在准备设备连接") : QStringLiteral("正在检查设备连接"));
    updateAgentControls();

    QPointer<MainWindow> guard(this);
    agentTasks_.start([guard, serial, applicationDirectory, generation, allowDeploy] {
        const auto progress = [guard, generation, allowDeploy](const std::string&) {
            QMetaObject::invokeMethod(
                guard,
                [guard, generation, allowDeploy] {
                    if (guard && !guard->shuttingDown_ && generation == guard->agentGeneration_) {
                        guard->setAgentStatus(allowDeploy ? QStringLiteral("调试服务 · 准备中")
                                                          : QStringLiteral("调试服务 · 检查中"),
                                              StatusTone::Neutral,
                                              QStringLiteral("正在准备设备连接"));
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
                    client->close();
                    if (deploymentResult.localPort != 0) {
                        testtools::AdbAgentDeployment().removeForward(serial,
                                                                      deploymentResult.localPort);
                    }
                    return;
                }
                guard->agentConnecting_ = false;
                if (!succeeded) {
                    client->close();
                    guard->setAgentStatus(
                        allowDeploy ? QStringLiteral("调试服务 · 连接失败")
                                    : QStringLiteral("调试服务 · 未连接"),
                        allowDeploy ? StatusTone::Error : StatusTone::Neutral,
                        allowDeploy
                            ? QStringLiteral("连接失败：%1").arg(connectionError)
                            : QStringLiteral("调试服务未连接：%1\n点击“连接调试服务”准备或重试。")
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
                const QJsonObject helloObject = jsonResultObject(hello);
                const QString version = infoText(helloObject, "agent_version");
                const bool systemInfoCapability =
                    hasCapability(capabilities, QStringLiteral("system.info"));
                const bool serialStream =
                    hasCapability(capabilities, QStringLiteral("serial.stream"));
                const bool memoryCapacity =
                    hasCapability(capabilities, QStringLiteral("memory.capacity"));
                const bool storageInfo =
                    hasCapability(capabilities, QStringLiteral("storage.device.info"));
                const bool storageHealth =
                    hasCapability(capabilities, QStringLiteral("storage.device.health"));
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
                guard->agentSummary_ = QStringLiteral("已就绪");
                guard->setAgentStatus(
                    QStringLiteral("调试服务 · %1").arg(guard->agentSummary_),
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
                const QJsonObject info = jsonResultObject(systemInfo);
                const QString model = infoText(info, "model", QStringLiteral("未知型号"));
                const QString manufacturer = infoText(info, "manufacturer");
                const QString brand = infoText(info, "brand");
                const QString product = infoText(info, "product");
                const QString board = infoText(info, "board");
                const QString hardware = infoText(info, "hardware");
                const QString architecture =
                    infoText(info, "architecture", fromUtf8(deploymentResult.target));
                const int cores = info.value(QStringLiteral("cpu_cores")).toInt();
                const qint64 memoryKib =
                    info.value(QStringLiteral("memory_total_kib")).toVariant().toLongLong();
                const QString release = infoText(info, "os_release", QStringLiteral("未知版本"));
                const QString sdk = infoText(info, "android_sdk");
                const QString kernel = infoText(info, "kernel", QStringLiteral("未知内核"));
                const QString systemBuild = infoText(info, "build_id");
                const int uid = info.value(QStringLiteral("effective_uid")).toInt(-1);

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
                    hardwareParts << QStringLiteral("%1 MiB 内存").arg(memoryKib / 1024);
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
                    uid == 0 ? QStringLiteral("完整") : QStringLiteral("受限"));
                for (QLabel* label : {guard->agentModelValue_,
                                      guard->agentManufacturerValue_,
                                      guard->agentHardwareValue_,
                                      guard->agentOsValue_,
                                      guard->agentBuildValue_,
                                      guard->agentSecurityValue_}) {
                    label->setToolTip({});
                }
                guard->setActivity(QStringLiteral("设备连接完成，可开始调试或检查"));
                guard->updateAgentControls();
            },
            Qt::QueuedConnection);
    });
}

void MainWindow::stopSelectedAgent()
{
    if (agentConnecting_ || shuttingDown_) {
        return;
    }
    std::string serial = agentAdbSerial_;
    if (serial.empty()) {
        serial = selectedDevice().serial;
    }
    if (serial.empty()) {
        setAgentStatus(QStringLiteral("调试服务 · 未连接"),
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
                guard->setAgentStatus(stopped ? QStringLiteral("调试服务 · 已重置")
                                              : QStringLiteral("调试服务 · 重置失败"),
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
    ++agentGeneration_;
    agentConnecting_ = false;
    auto client = std::move(agentClient_);
    if (serialWorkspace_)
        serialWorkspace_->setAgentConnection({}, 0);
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
    setAgentStatus(QStringLiteral("调试服务 · 未连接"),
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
    const auto state = serviceState_;
    const QString label = state == State::Ready        ? QStringLiteral("调试服务 · 已连接")
                          : state == State::Connecting ? QStringLiteral("调试服务 · 连接中")
                          : state == State::Failed     ? QStringLiteral("调试服务 · 连接失败")
                                                       : QStringLiteral("调试服务 · 未连接");
    const QString tone = state == State::Ready    ? "ready"
                         : state == State::Failed ? "failed"
                                                  : "neutral";
    topServiceButton_->setText(label);
    topServiceButton_->setToolTip(
        serviceDetail_.isEmpty()
            ? QStringLiteral("设备串口需要调试服务；文件传输、终端和投屏可独立使用。")
            : serviceDetail_);
    topServiceButton_->setEnabled(
        !shuttingDown_ && !closeRequested_ && !agentConnecting_ && !agentCleanupPending_ &&
        (agentClient_ || (!serialBusy_ && selectedDevice().state == "device")) &&
        !qEnvironmentVariableIsSet("TESTTOOLS_OFFLINE"));
    if (topServiceButton_->property("serviceState").toString() != tone) {
        topServiceButton_->setProperty("serviceState", tone);
        topServiceButton_->style()->unpolish(topServiceButton_);
        topServiceButton_->style()->polish(topServiceButton_);
    }
    if (serialWorkspace_)
        serialWorkspace_->setServiceStatus(state, serviceDetail_, agentCleanupPending_);
}

void MainWindow::checkServiceHealth()
{
    agentTasks_.reapFinished();
    if (shuttingDown_ || closeRequested_ || serviceProbeActive_ || agentConnecting_ ||
        !agentClient_ || !agentForwardPort_ || selectedDevice().serial != agentAdbSerial_ ||
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
                    if (!healthy) {
                        guard->disconnectAgent(QStringLiteral("调试服务连接已失效，请重试。"));
                        guard->setAgentStatus(
                            QStringLiteral("调试服务 · 连接失败"),
                            StatusTone::Error,
                            QStringLiteral("调试服务连接已失效：%1").arg(fromUtf8(error)));
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
    const bool online = !device.serial.empty() && device.state == "device";
    deployAgentButton_->setEnabled(online && !agentConnecting_ && !agentCleanupPending_ &&
                                   !connected && !serialBusy_);
    disconnectAgentButton_->setEnabled(connected && !serialBusy_);
    stopAgentButton_->setEnabled(online && !agentConnecting_ && !serialBusy_);
    updateServicePresentation();
    deployAgentButton_->setText(agentConnecting_ ? QStringLiteral("正在连接…")
                                                 : QStringLiteral("连接调试服务"));
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
    if (shuttingDown_ || terminalLaunching_ || qEnvironmentVariableIsSet("TESTTOOLS_OFFLINE") ||
        device.serial.empty() || device.state != "device") {
        return;
    }
    terminalLaunching_ = true;
    updateAdbTerminalControls();
    setActivity(QStringLiteral("正在为 %1 打开 ADB 终端…").arg(fromUtf8(device.serial)));
    QPointer<MainWindow> guard(this);
    // Capture the selection on click; the independent shell keeps this device for its lifetime.
    terminalTasks_.start([guard, device] {
        const auto result = testtools::AdbTerminalService().open(device);
        QMetaObject::invokeMethod(guard, [guard, device, result] {
            if (!guard || guard->shuttingDown_) {
                return;
            }
            guard->terminalLaunching_ = false;
            guard->updateAdbTerminalControls();
            guard->setActivity(result.started
                ? QStringLiteral("已为 %1 打开 ADB 终端（%2）")
                      .arg(fromUtf8(device.serial), fromUtf8(result.host))
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
        button->setEnabled(online && !terminalLaunching_ && !shuttingDown_ && !closeRequested_ &&
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
    if (shuttingDown_ || scanning_ || serialBusy_ || agentConnecting_) {
        return;
    }
    scanning_ = true;
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
    if (serialBusy_ || agentConnecting_) {
        scanning_ = false;
        updateDeviceControls();
        return;
    }
    const QString signature = deviceScanSignature(result);
    if (automatic && signature == lastDeviceScanSignature_) {
        scanning_ = false;
        updateDeviceControls();
        return;
    }
    lastDeviceScanSignature_ = signature;
    if (automatic && !result.error.empty()) {
        scanning_ = false;
        setAdbStatus(QStringLiteral("设备 · 检查失败"), StatusTone::Error);
        setActivity(QStringLiteral("自动检查设备失败；已保留当前选择，可手动刷新重试"));
        updateDeviceControls();
        return;
    }
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
    const QString scanError =
        result.error.empty()
            ? QString{}
            : QStringLiteral("设备检查失败，请确认连接工具可用并重新授权设备");
    if (!result.error.empty()) {
        setAdbStatus(QStringLiteral("设备 · 检查失败"), StatusTone::Error);
        setActivity(scanError);
    } else if (result.devices.empty()) {
        deviceCombo_->addItem(QStringLiteral("未发现设备"));
        deviceCombo_->setCurrentIndex(0);
        setAdbStatus(QStringLiteral("设备 · 未连接"), StatusTone::Neutral);
        setActivity(automatic && !previousSerial.isEmpty()
                        ? QStringLiteral("当前设备已拔出；Windows 本地串口仍可使用")
                        : QStringLiteral("未发现设备；Windows 本地串口仍可使用"));
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
        if (automatic && restoreIndex < 0) {
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
    // Refreshing the selected device also refreshes Agent/serial state and can
    // replace the activity text. Keep the actionable ADB failure visible.
    if (!scanError.isEmpty()) {
        setActivity(scanError);
    }
}

serialdemo::AdbDevice MainWindow::selectedDevice() const
{
    serialdemo::AdbDevice device;
    if (!deviceCombo_ || deviceCombo_->currentData().toString().isEmpty()) {
        return device;
    }
    const QByteArray serial = deviceCombo_->currentData().toString().toUtf8();
    const QByteArray state = deviceCombo_->currentData(Qt::UserRole + 1).toString().toUtf8();
    const QByteArray description = deviceCombo_->currentData(Qt::UserRole + 2).toString().toUtf8();
    device.serial.assign(serial.constData(), static_cast<std::size_t>(serial.size()));
    device.state.assign(state.constData(), static_cast<std::size_t>(state.size()));
    device.description.assign(description.constData(),
                              static_cast<std::size_t>(description.size()));
    return device;
}

void MainWindow::applySelectedDevice()
{
    if (!serialWorkspace_) {
        return;
    }
    const auto device = selectedDevice();
    if (device.state == "device")
        setAdbStatus(QStringLiteral("ADB · 已连接"), StatusTone::Good);
    else
        setAdbStatus(device.serial.empty()
                         ? QStringLiteral("ADB · 未连接")
                         : QStringLiteral("ADB · %1").arg(deviceStateText(device.state)),
                     StatusTone::Neutral);
    if (screenMirrorPanel_) screenMirrorPanel_->setDevice(device);
    if (fileWorkspace_) fileWorkspace_->setDevice(device);
    if (!agentAdbSerial_.empty() &&
        (agentAdbSerial_ != device.serial || device.state != "device")) {
        disconnectAgent(QStringLiteral("已切换设备，原连接已断开"));
    }
    serialWorkspace_->setAdbDevice(device);
    updateAgentControls();
    if (!agentClient_ && !agentConnecting_ && device.state == "device" && !device.serial.empty()) {
        connectSelectedAgent(false);
    }
}

void MainWindow::showPage(int pageIndex)
{
    if (!pages_ || pageIndex < 0 || pageIndex >= pages_->count()) {
        return;
    }
    pages_->setCurrentIndex(pageIndex);
    if (pageIndex == FileTransferPage || pageIndex == MirrorPage) {
        lastAdbPage_ = pageIndex; setAdbExpanded(true);
    }
    if (pageIndex == SerialPage) setSerialExpanded(true);
    updateNavigation();
}

void MainWindow::setSerialExpanded(bool expanded)
{
    serialChildren_->setVisible(expanded);
    serialExpandButton_->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    serialExpandButton_->setToolTip(expanded ? QStringLiteral("收起串口子页面") : QStringLiteral("展开串口子页面"));
}

void MainWindow::updateNavigation()
{
    if (!pages_) return;
    serialGroupButton_->setChecked(pages_->currentIndex() == SerialPage);
    adbGroupButton_->setChecked(pages_->currentIndex() == FileTransferPage ||
                                pages_->currentIndex() == MirrorPage);
    for (auto* button : navigationButtons_) {
        const auto mode = button->property("serialMode").toString();
        button->setChecked(button->property("pageIndex").toInt() == pages_->currentIndex() &&
            (mode.isEmpty() || (serialWorkspace_ && mode == serialWorkspace_->currentWorkspaceMode())));
    }
}

void MainWindow::setAdbStatus(const QString& text, StatusTone tone)
{
    adbStatusLabel_->setText(text);
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
        activityLabel_->setText(message);
    }
}

void MainWindow::updateDeviceControls()
{
    updateServicePresentation();
    updateAdbTerminalControls();
    const bool enabled = !qEnvironmentVariableIsSet("TESTTOOLS_OFFLINE") && !scanning_ &&
                         !serialBusy_ && !agentConnecting_;
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
