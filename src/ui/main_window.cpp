#include "main_window.h"

#include "application/agent/agent_connection_service.h"
#include "application/agent/adb_agent_deployment.h"
#include "theme.h"
#include "ui/pages/serial_workspace.h"

#include <QtCore/QCoreApplication>
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
#include <QtWidgets/QVBoxLayout>

#include <utility>

namespace {

QString fromUtf8(const std::string& text)
{
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

QJsonObject jsonResultObject(const std::string& json)
{
    QJsonParseError error;
    const QJsonDocument document =
        QJsonDocument::fromJson(QByteArray(json.data(), static_cast<qsizetype>(json.size())), &error);
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

QFrame*
makeSummaryCard(QWidget* parent, const QString& eyebrow, QLabel*& valueLabel, const QString& body)
{
    auto* card = new QFrame(parent);
    card->setObjectName(QStringLiteral("summaryCard"));
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(5);

    auto* eyebrowLabel = new QLabel(eyebrow, card);
    eyebrowLabel->setObjectName(QStringLiteral("cardEyebrow"));
    valueLabel = new QLabel(QStringLiteral("—"), card);
    valueLabel->setObjectName(QStringLiteral("cardValue"));
    valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* bodyLabel = new QLabel(body, card);
    bodyLabel->setObjectName(QStringLiteral("cardBody"));
    bodyLabel->setWordWrap(true);

    layout->addWidget(eyebrowLabel);
    layout->addWidget(valueLabel);
    layout->addWidget(bodyLabel);
    return card;
}

QFrame* makeFeatureCard(QWidget* parent,
                        const QString& title,
                        const QString& status,
                        const QString& description,
                        bool available)
{
    auto* card = new QFrame(parent);
    card->setObjectName(QStringLiteral("featureCard"));
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(7);

    auto* top = new QHBoxLayout;
    auto* titleLabel = new QLabel(title, card);
    titleLabel->setObjectName(QStringLiteral("sectionLabel"));
    auto* statusLabel = new QLabel(status, card);
    statusLabel->setObjectName(available ? QStringLiteral("topStatusGood")
                                         : QStringLiteral("topStatusWarn"));
    top->addWidget(titleLabel);
    top->addStretch();
    top->addWidget(statusLabel);

    auto* body = new QLabel(description, card);
    body->setObjectName(QStringLiteral("cardBody"));
    body->setWordWrap(true);
    layout->addLayout(top);
    layout->addWidget(body);
    layout->addStretch();
    return card;
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
    refreshDevices();
}

MainWindow::~MainWindow()
{
    prepareForShutdown();
}

void MainWindow::prepareForShutdown()
{
    if (shuttingDown_) {
        return;
    }
    shuttingDown_ = true;
    const std::string selectedSerial = selectedDevice().serial;
    if (serialWorkspace_) {
        serialWorkspace_->prepareForShutdown();
    }
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
    if (normalized == QStringLiteral("overview")) {
        showPage(OverviewPage);
    } else if (normalized == QStringLiteral("serial")) {
        showPage(SerialPage);
    } else if (normalized == QStringLiteral("tests")) {
        showPage(TestCenterPage);
    } else if (normalized == QStringLiteral("history")) {
        showPage(HistoryPage);
    } else if (normalized == QStringLiteral("settings")) {
        showPage(SettingsPage);
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
    pages_->addWidget(buildOverviewPage());
    pages_->addWidget(buildSerialPage());
    pages_->addWidget(buildTestCenterPage());
    pages_->addWidget(buildEmptyPage(
        QStringLiteral("运行记录"),
        QStringLiteral("按设备和任务回看参数、结果与产物"),
        QStringLiteral("尚无运行记录"),
        QStringLiteral(
            "Agent 任务模型接入后，这里会保存每次运行的设备快照、参数、结果和产物索引。")));
    pages_->addWidget(buildSettingsPage());
    bodyLayout->addWidget(pages_, 1);
    root->addWidget(body, 1);

    auto* activity = new QFrame(surface);
    activity->setObjectName(QStringLiteral("activityBar"));
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
    showPage(OverviewPage);
}

QWidget* MainWindow::buildTopBar()
{
    auto* bar = new QFrame(this);
    bar->setObjectName(QStringLiteral("topBar"));
    auto* layout = new QHBoxLayout(bar);
    layout->setContentsMargins(18, 11, 16, 11);
    layout->setSpacing(12);

    auto* brand = new QHBoxLayout;
    brand->setSpacing(8);
    auto* title = new QLabel(QStringLiteral("TestTools"), bar);
    title->setObjectName(QStringLiteral("appTitle"));
    auto* subtitle = new QLabel(QStringLiteral("板卡调试与测试工作台"), bar);
    subtitle->setObjectName(QStringLiteral("appSubtitle"));
    brand->addWidget(title);
    brand->addWidget(subtitle);
    layout->addLayout(brand);
    layout->addSpacing(20);

    auto* caption = new QLabel(QStringLiteral("设备"), bar);
    caption->setObjectName(QStringLiteral("topCaption"));
    deviceCombo_ = new QComboBox(bar);
    deviceCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    deviceCombo_->setMinimumContentsLength(28);
    adbStatusLabel_ = new QLabel(QStringLiteral("ADB · 检测中"), bar);
    adbStatusLabel_->setObjectName(QStringLiteral("topStatusNeutral"));
    agentStatusLabel_ = new QLabel(QStringLiteral("Agent · 未连接"), bar);
    agentStatusLabel_->setObjectName(QStringLiteral("topStatusNeutral"));
    agentActionButton_ = new QPushButton(QStringLiteral("Agent 管理"), bar);
    agentActionButton_->setObjectName(QStringLiteral("topGhostButton"));
    refreshButton_ = new QPushButton(QStringLiteral("刷新设备"), bar);
    refreshButton_->setObjectName(QStringLiteral("topGhostButton"));

    layout->addWidget(caption);
    layout->addWidget(deviceCombo_, 1);
    layout->addWidget(adbStatusLabel_);
    layout->addWidget(agentStatusLabel_);
    layout->addWidget(agentActionButton_);
    layout->addWidget(refreshButton_);

    connect(refreshButton_, &QPushButton::clicked, this, &MainWindow::refreshDevices);
    connect(agentActionButton_, &QPushButton::clicked, this, [this] { showPage(SettingsPage); });
    connect(deviceCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::applySelectedDevice);
    return bar;
}

QWidget* MainWindow::buildNavigation()
{
    auto* rail = new QFrame(this);
    rail->setObjectName(QStringLiteral("navigationRail"));
    rail->setFixedWidth(164);
    auto* layout = new QVBoxLayout(rail);
    layout->setContentsMargins(10, 16, 10, 12);
    layout->setSpacing(4);

    layout->addWidget(addNavigationButton(QStringLiteral("概览"), OverviewPage));
    layout->addWidget(addNavigationButton(QStringLiteral("串口工作台"), SerialPage));
    layout->addWidget(addNavigationButton(QStringLiteral("测试中心"), TestCenterPage));
    layout->addWidget(addNavigationButton(QStringLiteral("运行记录"), HistoryPage));
    layout->addStretch();
    layout->addWidget(addNavigationButton(QStringLiteral("设置"), SettingsPage));
    return rail;
}

QPushButton* MainWindow::addNavigationButton(const QString& text, int pageIndex)
{
    auto* button = new QPushButton(text, this);
    button->setObjectName(QStringLiteral("navigationButton"));
    button->setCheckable(true);
    button->setCursor(Qt::PointingHandCursor);
    connect(button, &QPushButton::clicked, this, [this, pageIndex] { showPage(pageIndex); });
    navigationButtons_.push_back(button);
    return button;
}

QWidget* MainWindow::buildOverviewPage()
{
    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(14);

    auto* contextPanel = new QFrame(content);
    contextPanel->setObjectName(QStringLiteral("contextPanel"));
    auto* summaries = new QHBoxLayout(contextPanel);
    summaries->setContentsMargins(4, 0, 4, 0);
    summaries->setSpacing(0);
    const auto addDivider = [contextPanel, summaries] {
        auto* divider = new QFrame(contextPanel);
        divider->setObjectName(QStringLiteral("summaryDivider"));
        divider->setFixedWidth(1);
        divider->setMinimumHeight(54);
        summaries->addWidget(divider);
    };
    summaries->addWidget(makeSummaryCard(contextPanel,
                                         QStringLiteral("当前设备"),
                                         overviewDeviceValue_,
                                         QStringLiteral("所有操作都会作用于这台板卡")),
                         1);
    addDivider();
    summaries->addWidget(makeSummaryCard(contextPanel,
                                         QStringLiteral("ADB"),
                                         overviewAdbValue_,
                                         QStringLiteral("负责发现、部署和安全隧道")),
                         1);
    addDivider();
    summaries->addWidget(makeSummaryCard(contextPanel,
                                         QStringLiteral("板端 Agent"),
                                         overviewAgentValue_,
                                         QStringLiteral("后续测试能力的统一入口")),
                         1);
    layout->addWidget(contextPanel);

    auto* quickStart = new QFrame(content);
    quickStart->setObjectName(QStringLiteral("featureCard"));
    auto* quickLayout = new QVBoxLayout(quickStart);
    quickLayout->setContentsMargins(18, 16, 18, 16);
    auto* quickTitle = new QLabel(QStringLiteral("快速开始"), quickStart);
    quickTitle->setObjectName(QStringLiteral("sectionLabel"));
    auto* quickBody = new QLabel(QStringLiteral("选择设备后进入串口工作台，默认连接一个串口即可调试"
                                                "；需要链路验证或压测时再进入双端调试。"),
                                 quickStart);
    quickBody->setObjectName(QStringLiteral("cardBody"));
    quickBody->setWordWrap(true);
    auto* quickActions = new QHBoxLayout;
    auto* openSerial = new QPushButton(QStringLiteral("打开串口工作台"), quickStart);
    openSerial->setObjectName(QStringLiteral("primaryButton"));
    auto* openTests = new QPushButton(QStringLiteral("查看测试演进"), quickStart);
    quickActions->addWidget(openSerial);
    quickActions->addWidget(openTests);
    quickActions->addStretch();
    quickLayout->addWidget(quickTitle);
    quickLayout->addWidget(quickBody);
    quickLayout->addSpacing(5);
    quickLayout->addLayout(quickActions);
    layout->addWidget(quickStart);
    layout->addStretch();

    connect(openSerial, &QPushButton::clicked, this, [this] { showPage(SerialPage); });
    connect(openTests, &QPushButton::clicked, this, [this] { showPage(TestCenterPage); });
    return wrapPage(QStringLiteral("设备概览"),
                    QStringLiteral("先确认操作对象和连接健康，再进入具体调试任务"),
                    content);
}

QWidget* MainWindow::buildSerialPage()
{
    serialWorkspace_ = new SerialWorkspace(this);
    connect(serialWorkspace_, &SerialWorkspace::activityChanged, this, &MainWindow::setActivity);
    connect(serialWorkspace_, &SerialWorkspace::busyChanged, this, [this](bool busy) {
        serialBusy_ = busy;
        updateDeviceControls();
    });
    return wrapPage(QStringLiteral("串口工作台"),
                    QStringLiteral("连接一个串口开始调试，需要链路验证或压测时切换到双端调试"),
                    serialWorkspace_,
                    false);
}

QWidget* MainWindow::buildTestCenterPage()
{
    auto* content = new QWidget(this);
    auto* layout = new QGridLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setHorizontalSpacing(12);
    layout->setVerticalSpacing(12);

    layout->addWidget(
        makeFeatureCard(content,
                        QStringLiteral("串口"),
                        QStringLiteral("可用"),
                        QStringLiteral("Windows 本地串口与板端串口调试、双端验证和可靠性压测。"),
                        true),
        0,
        0);
    layout->addWidget(
        makeFeatureCard(content,
                        QStringLiteral("DDR 稳定性"),
                        QStringLiteral("需要 Agent"),
                        QStringLiteral("容量、固定/动态频率压力、休眠、自刷新和重启恢复。"),
                        false),
        0,
        1);
    layout->addWidget(makeFeatureCard(content,
                                      QStringLiteral("存储"),
                                      QStringLiteral("需要 Agent"),
                                      QStringLiteral("eMMC/UFS 身份、寿命、性能和数据完整性验证。"),
                                      false),
                      1,
                      0);
    layout->addWidget(makeFeatureCard(content,
                                      QStringLiteral("音频与摄像头"),
                                      QStringLiteral("需要 Agent"),
                                      QStringLiteral("设备枚举、采集、回放、波形和图像产物。"),
                                      false),
                      1,
                      1);
    layout->setRowStretch(2, 1);
    layout->setColumnStretch(0, 1);
    layout->setColumnStretch(1, 1);
    return wrapPage(QStringLiteral("测试中心"),
                    QStringLiteral("能力将由板端 Agent 发现；复杂测试保留专用交互"),
                    content);
}

QWidget* MainWindow::buildSettingsPage()
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
    auto* connectionTitle = new QLabel(QStringLiteral("板端 Agent"), connectionCard);
    connectionTitle->setObjectName(QStringLiteral("sectionLabel"));
    auto* connectionBody = new QLabel(
        QStringLiteral(
            "主动连接时会核对 Agent 版本；版本不一致或未安装时自动同步。后台发现保持只读。"),
        connectionCard);
    connectionBody->setObjectName(QStringLiteral("cardBody"));
    connectionBody->setWordWrap(true);
    connectionLayout->addWidget(connectionTitle);
    connectionLayout->addWidget(connectionBody);

    agentDeviceValue_ = new QLabel(QStringLiteral("目标设备：未选择"), connectionCard);
    agentDeviceValue_->setObjectName(QStringLiteral("cardBody"));
    connectionLayout->addWidget(agentDeviceValue_);

    auto* actions = new QHBoxLayout;
    deployAgentButton_ = new QPushButton(QStringLiteral("连接并同步 Agent"), connectionCard);
    deployAgentButton_->setObjectName(QStringLiteral("primaryButton"));
    disconnectAgentButton_ = new QPushButton(QStringLiteral("断开本机连接"), connectionCard);
    stopAgentButton_ = new QPushButton(QStringLiteral("停止板端 Agent"), connectionCard);
    actions->addWidget(deployAgentButton_);
    actions->addWidget(disconnectAgentButton_);
    actions->addWidget(stopAgentButton_);
    actions->addStretch();
    connectionLayout->addLayout(actions);

    agentConnectionDetail_ = new QLabel(QStringLiteral("尚未连接真实板端 Agent"), connectionCard);
    agentConnectionDetail_->setObjectName(QStringLiteral("cardBody"));
    agentConnectionDetail_->setWordWrap(true);
    agentConnectionDetail_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    agentCapabilitiesValue_ = new QLabel(QStringLiteral("已发现能力：—"), connectionCard);
    agentCapabilitiesValue_->setObjectName(QStringLiteral("cardBody"));
    agentCapabilitiesValue_->setWordWrap(true);
    agentCapabilitiesValue_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    connectionLayout->addWidget(agentConnectionDetail_);

    auto* versionTitle = new QLabel(QStringLiteral("版本状态"), connectionCard);
    versionTitle->setObjectName(QStringLiteral("sectionLabel"));
    agentVersionValue_ = new QLabel(
        QStringLiteral("本地要求：v%1 · 板端：—")
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
    const auto addBoardRow = [connectionCard, boardGrid](int row,
                                                         const QString& name,
                                                         QLabel*& value) {
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
    addBoardRow(5, QStringLiteral("运行权限"), agentSecurityValue_);
    connectionLayout->addLayout(boardGrid);

    auto* capabilityTitle = new QLabel(QStringLiteral("Agent 能力"), connectionCard);
    capabilityTitle->setObjectName(QStringLiteral("sectionLabel"));
    connectionLayout->addWidget(capabilityTitle);
    connectionLayout->addWidget(agentCapabilitiesValue_);
    layout->addWidget(connectionCard);
    layout->addStretch();

    connect(deployAgentButton_, &QPushButton::clicked, this, &MainWindow::deploySelectedAgent);
    connect(disconnectAgentButton_, &QPushButton::clicked, this, [this] {
        disconnectAgent(QStringLiteral("Agent 已断开"));
    });
    connect(stopAgentButton_, &QPushButton::clicked, this, &MainWindow::stopSelectedAgent);
    updateAgentControls();
    return wrapPage(
        QStringLiteral("设置"), QStringLiteral("管理真实板端 Agent、协议版本和设备能力"), content);
}

QWidget* MainWindow::buildEmptyPage(const QString& title,
                                    const QString& subtitle,
                                    const QString& heading,
                                    const QString& body)
{
    auto* content = new QFrame(this);
    content->setObjectName(QStringLiteral("featureCard"));
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(20, 18, 20, 18);
    auto* headingLabel = new QLabel(heading, content);
    headingLabel->setObjectName(QStringLiteral("sectionLabel"));
    auto* bodyLabel = new QLabel(body, content);
    bodyLabel->setObjectName(QStringLiteral("cardBody"));
    bodyLabel->setWordWrap(true);
    layout->addWidget(headingLabel);
    layout->addWidget(bodyLabel);
    layout->addStretch();
    return wrapPage(title, subtitle, content);
}

QWidget* MainWindow::wrapPage(const QString& title,
                              const QString& subtitle,
                              QWidget* content,
                              bool scrollable)
{
    auto* page = new QWidget(this);
    page->setObjectName(QStringLiteral("pageSurface"));
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(13);

    auto* titleLabel = new QLabel(title, page);
    titleLabel->setObjectName(QStringLiteral("pageTitle"));
    auto* subtitleLabel = new QLabel(subtitle, page);
    subtitleLabel->setObjectName(QStringLiteral("pageSubtitle"));
    subtitleLabel->setWordWrap(true);
    layout->addWidget(titleLabel);
    layout->addWidget(subtitleLabel);

    if (scrollable) {
        auto* scroll = new QScrollArea(page);
        scroll->setObjectName(QStringLiteral("pageScroll"));
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
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
    if (agentConnecting_ || agentClient_ || shuttingDown_) {
        return;
    }
    const auto device = selectedDevice();
    if (device.serial.empty() || device.state != "device") {
        setAgentStatus(QStringLiteral("Agent · 无可用设备"),
                       StatusTone::Error,
                       QStringLiteral("请先在顶部选择在线的 ADB 设备"));
        return;
    }

    const std::string serial = device.serial;
    const QByteArray applicationDirectoryBytes = QCoreApplication::applicationDirPath().toUtf8();
    const std::string applicationDirectory(
        applicationDirectoryBytes.constData(),
        static_cast<std::size_t>(applicationDirectoryBytes.size()));
    const std::uint64_t generation = ++agentGeneration_;
    agentConnecting_ = true;
    setAgentStatus(allowDeploy ? QStringLiteral("Agent · 连接中")
                               : QStringLiteral("Agent · 正在发现"),
                   StatusTone::Neutral,
                   allowDeploy ? QStringLiteral("正在探测或部署真实板端 Agent")
                               : QStringLiteral("正在探测板端已运行的 Agent"));
    updateAgentControls();

    QPointer<MainWindow> guard(this);
    agentTasks_.start([guard, serial, applicationDirectory, generation, allowDeploy] {
        testtools::AdbAgentDeployment deployment;
        const auto progress = [guard, generation, allowDeploy](const std::string& message) {
            QMetaObject::invokeMethod(
                guard,
                [guard, generation, message, allowDeploy] {
                    if (guard && !guard->shuttingDown_ && generation == guard->agentGeneration_) {
                        guard->setAgentStatus(allowDeploy ? QStringLiteral("Agent · 连接中")
                                                          : QStringLiteral("Agent · 正在发现"),
                                              StatusTone::Neutral,
                                              fromUtf8(message));
                    }
                },
                Qt::QueuedConnection);
        };
        auto deploymentResult = deployment.reconnect(serial, progress);
        auto client = testtools::AgentConnectionService::createClient();
        std::string error;
        std::string hello;
        std::string capabilities;
        std::string systemInfo;
        QString observedVersion;
        const auto connectAndDiscover = [&] {
            if (!deploymentResult.succeeded) {
                error = deploymentResult.error;
                return false;
            }
            error.clear();
            hello.clear();
            capabilities.clear();
            systemInfo.clear();
            observedVersion.clear();
            client = testtools::AgentConnectionService::createClient();
            if (!client->connect("127.0.0.1", deploymentResult.localPort, error) ||
                !client->request("{\"protocol\":1,\"request_id\":\"ui-hello\","
                                 "\"method\":\"session.hello\",\"params\":{}}",
                                 hello,
                                 error)) {
                return false;
            }
            observedVersion = infoText(jsonResultObject(hello), "agent_version");
            if (observedVersion !=
                QString::fromLatin1(testtools::AdbAgentDeployment::RequiredAgentVersion)) {
                error = std::string("板端 Agent 版本不匹配，需要 ") +
                        testtools::AdbAgentDeployment::RequiredAgentVersion;
                return false;
            }
            return client->request("{\"protocol\":1,\"request_id\":\"ui-capabilities\","
                                   "\"method\":\"capability.list\",\"params\":{}}",
                                   capabilities,
                                   error) &&
                   client->request("{\"protocol\":1,\"request_id\":\"ui-system-info\","
                                   "\"method\":\"system.info\",\"params\":{}}",
                                   systemInfo,
                                   error);
        };
        bool succeeded = connectAndDiscover();
        if (!succeeded && allowDeploy) {
            client->close();
            if (deploymentResult.localPort != 0) {
                deployment.removeForward(serial, deploymentResult.localPort);
            }
            progress("未发现可用 Agent，正在部署当前版本...");
            deploymentResult = deployment.deployAndForward(serial, applicationDirectory, progress);
            succeeded = connectAndDiscover();
        }
        if (!succeeded && deploymentResult.localPort != 0) {
            deployment.removeForward(serial, deploymentResult.localPort);
            deploymentResult.localPort = 0;
        }
        QMetaObject::invokeMethod(
            guard,
            [guard,
             client = std::move(client),
             hello = std::move(hello),
             capabilities = std::move(capabilities),
             systemInfo = std::move(systemInfo),
             observedVersion = std::move(observedVersion),
             error = std::move(error),
             deploymentResult = std::move(deploymentResult),
             succeeded,
             serial,
             generation,
             allowDeploy]() mutable {
                if (!guard || guard->shuttingDown_ || generation != guard->agentGeneration_) {
                    client->close();
                    if (deploymentResult.localPort != 0) {
                        testtools::AdbAgentDeployment().removeForward(
                            serial, deploymentResult.localPort);
                    }
                    return;
                }
                guard->agentConnecting_ = false;
                if (!succeeded) {
                    client->close();
                    if (deploymentResult.localPort != 0) {
                        testtools::AdbAgentDeployment().removeForward(
                            serial, deploymentResult.localPort);
                    }
                    guard->setAgentStatus(
                        allowDeploy ? QStringLiteral("Agent · 连接失败")
                                    : QStringLiteral("Agent · 未发现"),
                        allowDeploy ? StatusTone::Error : StatusTone::Neutral,
                        allowDeploy
                            ? fromUtf8(error)
                            : QStringLiteral(
                                  "未发现兼容版本的板端 Agent；点击“连接并同步 Agent”安装或升级"));
                    if (guard->agentVersionValue_) {
                        guard->agentVersionValue_->setText(
                            QStringLiteral("本地要求：v%1 · 板端：%2")
                                .arg(QString::fromLatin1(
                                         testtools::AdbAgentDeployment::RequiredAgentVersion),
                                     observedVersion.isEmpty()
                                         ? QStringLiteral("未发现")
                                         : QStringLiteral("v%1 · 需要升级")
                                               .arg(observedVersion)));
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
                guard->agentSummary_ = version.isEmpty()
                                           ? QStringLiteral("已连接")
                                           : QStringLiteral("v%1 · 已同步").arg(version);
                guard->setAgentStatus(QStringLiteral("Agent · %1").arg(guard->agentSummary_),
                                      StatusTone::Good,
                                      QStringLiteral("%1 · %2 · 本地转发端口 %3")
                                          .arg(fromUtf8(serial), fromUtf8(deploymentResult.target))
                                          .arg(deploymentResult.localPort));
                guard->agentVersionValue_->setText(
                    QStringLiteral("本地要求：v%1 · 板端：v%2 · 已同步")
                        .arg(QString::fromLatin1(
                                 testtools::AdbAgentDeployment::RequiredAgentVersion),
                             version.isEmpty() ? QStringLiteral("未知") : version));
                guard->agentCapabilitiesValue_->setText(
                    QStringLiteral("已发现能力：%1%2")
                        .arg(systemInfoCapability ? QStringLiteral("system.info（只读）")
                                                  : QStringLiteral("—"),
                             serialStream ? QStringLiteral("、serial.stream（设备 I/O）")
                                          : QString{}));
                guard->agentCapabilitiesValue_->setToolTip(fromUtf8(capabilities));
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
                const QString selinux = infoText(info, "selinux", QStringLiteral("未知"));

                guard->agentModelValue_->setText(
                    board.isEmpty() ? model : QStringLiteral("%1 · %2").arg(model, board));
                QStringList manufacturerParts;
                for (const QString& part : {manufacturer, brand, product}) {
                    if (!part.isEmpty()) {
                        manufacturerParts << part;
                    }
                }
                const QString manufacturerLine =
                    manufacturerParts.join(QStringLiteral(" · "));
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
                guard->agentBuildValue_->setText(
                    systemBuild.isEmpty() ? QStringLiteral("—") : systemBuild);
                guard->agentSecurityValue_->setText(
                    QStringLiteral("UID %1 · SELinux %2")
                        .arg(uid < 0 ? QStringLiteral("未知") : QString::number(uid), selinux));
                for (QLabel* label : {guard->agentModelValue_,
                                      guard->agentManufacturerValue_,
                                      guard->agentHardwareValue_,
                                      guard->agentOsValue_,
                                      guard->agentBuildValue_,
                                      guard->agentSecurityValue_}) {
                    label->setToolTip(fromUtf8(systemInfo));
                }
                guard->setActivity(QStringLiteral("真实板端 Agent 连接、握手与能力发现完成"));
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
        setAgentStatus(QStringLiteral("Agent · 无可用设备"),
                       StatusTone::Error,
                       QStringLiteral("请先选择 ADB 设备"));
        return;
    }
    disconnectAgent(QStringLiteral("正在停止板端 Agent"));
    const std::uint64_t generation = ++agentGeneration_;
    agentConnecting_ = true;
    updateAgentControls();
    QPointer<MainWindow> guard(this);
    agentTasks_.start([guard, serial, generation] {
        std::string error;
        const bool stopped = testtools::AdbAgentDeployment().stopRemote(serial, error);
        QMetaObject::invokeMethod(
            guard,
            [guard, stopped, error = std::move(error), generation] {
                if (!guard || guard->shuttingDown_ || generation != guard->agentGeneration_) {
                    return;
                }
                guard->agentConnecting_ = false;
                guard->setAgentStatus(
                    stopped ? QStringLiteral("Agent · 已停止") : QStringLiteral("Agent · 停止失败"),
                    stopped ? StatusTone::Neutral : StatusTone::Error,
                    stopped ? QStringLiteral("板端 Agent 已停止") : fromUtf8(error));
                guard->updateAgentControls();
            },
            Qt::QueuedConnection);
    });
}

void MainWindow::disconnectAgent(const QString& reason)
{
    ++agentGeneration_;
    agentConnecting_ = false;
    if (agentClient_) {
        agentClient_->close();
        agentClient_.reset();
    }
    if (serialWorkspace_) {
        serialWorkspace_->setAgentConnection({}, 0);
    }
    if (!agentAdbSerial_.empty() && agentForwardPort_ != 0) {
        testtools::AdbAgentDeployment().removeForward(agentAdbSerial_, agentForwardPort_);
    }
    agentAdbSerial_.clear();
    agentForwardPort_ = 0;
    agentSummary_ = QStringLiteral("未连接");
    setAgentStatus(QStringLiteral("Agent · 未连接"),
                   StatusTone::Neutral,
                   reason.isEmpty() ? QStringLiteral("尚未连接 Agent") : reason);
    if (agentCapabilitiesValue_) {
        agentCapabilitiesValue_->setText(QStringLiteral("已发现能力：—"));
        agentCapabilitiesValue_->setToolTip({});
    }
    if (agentVersionValue_) {
        agentVersionValue_->setText(
            QStringLiteral("本地要求：v%1 · 板端：—")
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
    if (!agentStatusLabel_) {
        return;
    }
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
    updateOverview();
}

void MainWindow::updateAgentControls()
{
    if (!deployAgentButton_) {
        return;
    }
    const bool connected = static_cast<bool>(agentClient_);
    const auto device = selectedDevice();
    const bool online = !device.serial.empty() && device.state == "device";
    deployAgentButton_->setEnabled(online && !agentConnecting_ && !connected && !serialBusy_);
    disconnectAgentButton_->setEnabled(connected && !serialBusy_);
    stopAgentButton_->setEnabled(online && !agentConnecting_ && !serialBusy_);
    deployAgentButton_->setText(agentConnecting_ ? QStringLiteral("正在处理...")
                                                 : QStringLiteral("连接并同步 Agent"));
    if (agentDeviceValue_) {
        agentDeviceValue_->setText(
            online ? QStringLiteral("目标设备：%1").arg(deviceDisplayName(device))
                   : QStringLiteral("目标设备：未选择在线 ADB 设备"));
        agentDeviceValue_->setToolTip(online ? fromUtf8(device.description) : QString{});
    }
}

void MainWindow::refreshDevices()
{
    if (shuttingDown_ || scanning_ || serialBusy_) {
        return;
    }
    scanning_ = true;
    setAdbStatus(QStringLiteral("ADB · 检测中"), StatusTone::Neutral);
    setActivity(QStringLiteral("正在扫描 ADB 设备"));
    updateDeviceControls();

    QPointer<MainWindow> guard(this);
    scanTasks_.start([guard] {
        testtools::DeviceDiscovery discovery;
        auto result = discovery.scanAdbDevices();
        QMetaObject::invokeMethod(
            guard,
            [guard, result = std::move(result)]() mutable {
                if (guard && !guard->shuttingDown_) {
                    guard->applyDeviceScan(std::move(result));
                }
            },
            Qt::QueuedConnection);
    });
}

void MainWindow::applyDeviceScan(testtools::DeviceScanResult result)
{
    if (shuttingDown_) {
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
            label += QStringLiteral(" [%1]").arg(fromUtf8(device.state));
        }
        deviceCombo_->addItem(label, fromUtf8(device.serial));
        const int index = deviceCombo_->count() - 1;
        deviceCombo_->setItemData(index, fromUtf8(device.description), Qt::ToolTipRole);
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
    const QString scanError = result.error.empty()
                                   ? QString{}
                                   : fromUtf8(result.error)
                                         + QStringLiteral("；请检查 ADB 安装、路径和设备授权");
    if (!result.error.empty()) {
        setAdbStatus(QStringLiteral("ADB · 异常"), StatusTone::Error);
        setActivity(scanError);
    } else if (result.devices.empty()) {
        deviceCombo_->addItem(QStringLiteral("未发现 ADB 设备"));
        deviceCombo_->setCurrentIndex(0);
        setAdbStatus(QStringLiteral("ADB · 无设备"), StatusTone::Neutral);
        setActivity(QStringLiteral("未发现 ADB 设备；Windows 本地串口仍可使用"));
    } else if (onlineCount == 0) {
        deviceCombo_->setCurrentIndex(restoreIndex >= 0 ? restoreIndex : 0);
        setAdbStatus(QStringLiteral("ADB · 待授权"), StatusTone::Warning);
        setActivity(QStringLiteral("已发现设备，但没有 device 状态的目标"));
    } else {
        deviceCombo_->setCurrentIndex(restoreIndex >= 0 ? restoreIndex : firstOnline);
        setAdbStatus(QStringLiteral("ADB · %1 台").arg(onlineCount));
        setActivity(QStringLiteral("ADB 设备扫描完成"));
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
    if (!agentAdbSerial_.empty() && agentAdbSerial_ != device.serial) {
        disconnectAgent(QStringLiteral("已切换设备，原 Agent 连接已断开"));
    }
    serialWorkspace_->setAdbDevice(device);
    updateOverview();
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
    for (int index = 0; index < static_cast<int>(navigationButtons_.size()); ++index) {
        navigationButtons_[index]->setChecked(index == pageIndex);
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
    updateOverview();
}

void MainWindow::setActivity(const QString& message)
{
    if (activityLabel_) {
        activityLabel_->setText(message);
    }
}

void MainWindow::updateOverview()
{
    if (!overviewDeviceValue_) {
        return;
    }
    const auto device = selectedDevice();
    overviewDeviceValue_->setText(device.serial.empty() ? QStringLiteral("未选择")
                                                        : deviceDisplayName(device));
    overviewAdbValue_->setText(adbStatusLabel_ ? adbStatusLabel_->text()
                                               : QStringLiteral("检测中"));
    overviewAgentValue_->setText(agentSummary_.isEmpty() ? QStringLiteral("未连接")
                                                         : agentSummary_);
}

void MainWindow::updateDeviceControls()
{
    const bool enabled = !scanning_ && !serialBusy_;
    refreshButton_->setEnabled(enabled);
    deviceCombo_->setEnabled(enabled && !deviceCombo_->currentData().toString().isEmpty());
}
