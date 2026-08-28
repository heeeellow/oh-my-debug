#include "main_window.h"

#include "core/agent_client.h"
#include "theme.h"
#include "ui/pages/serial_workspace.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QMetaObject>
#include <QtCore/QPointer>
#include <QtCore/QProcess>
#include <QtCore/QSignalBlocker>
#include <QtGui/QIcon>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFrame>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QStyle>
#include <QtWidgets/QVBoxLayout>

#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

QString fromUtf8(const std::string &text)
{
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

QString jsonStringValue(const std::string &json, const std::string &key)
{
    const std::string marker = "\"" + key + "\":\"";
    const std::size_t start = json.find(marker);
    if (start == std::string::npos) {
        return {};
    }
    const std::size_t valueStart = start + marker.size();
    const std::size_t end = json.find('"', valueStart);
    return end == std::string::npos ? QString{} : fromUtf8(json.substr(valueStart, end - valueStart));
}

QString deviceDisplayName(const serialdemo::AdbDevice &device)
{
    const QString description = fromUtf8(device.description);
    const QStringList fields = description.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString &field : fields) {
        if (field.startsWith(QStringLiteral("model:"))) {
            QString model = field.mid(6);
            model.replace(QLatin1Char('_'), QLatin1Char(' '));
            return QStringLiteral("%1 · %2").arg(model, fromUtf8(device.serial));
        }
    }
    return fromUtf8(device.serial);
}

QFrame *makeSummaryCard(QWidget *parent, const QString &eyebrow, QLabel *&valueLabel,
                        const QString &body)
{
    auto *card = new QFrame(parent);
    card->setObjectName(QStringLiteral("summaryCard"));
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(5);

    auto *eyebrowLabel = new QLabel(eyebrow, card);
    eyebrowLabel->setObjectName(QStringLiteral("cardEyebrow"));
    valueLabel = new QLabel(QStringLiteral("—"), card);
    valueLabel->setObjectName(QStringLiteral("cardValue"));
    valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *bodyLabel = new QLabel(body, card);
    bodyLabel->setObjectName(QStringLiteral("cardBody"));
    bodyLabel->setWordWrap(true);

    layout->addWidget(eyebrowLabel);
    layout->addWidget(valueLabel);
    layout->addWidget(bodyLabel);
    return card;
}

QFrame *makeFeatureCard(QWidget *parent, const QString &title, const QString &status,
                        const QString &description, bool available)
{
    auto *card = new QFrame(parent);
    card->setObjectName(QStringLiteral("featureCard"));
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(7);

    auto *top = new QHBoxLayout;
    auto *titleLabel = new QLabel(title, card);
    titleLabel->setObjectName(QStringLiteral("sectionLabel"));
    auto *statusLabel = new QLabel(status, card);
    statusLabel->setObjectName(available ? QStringLiteral("topStatusGood")
                                         : QStringLiteral("topStatusWarn"));
    top->addWidget(titleLabel);
    top->addStretch();
    top->addWidget(statusLabel);

    auto *body = new QLabel(description, card);
    body->setObjectName(QStringLiteral("cardBody"));
    body->setWordWrap(true);
    layout->addLayout(top);
    layout->addWidget(body);
    layout->addStretch();
    return card;
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    buildUi();
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
            this, &MainWindow::prepareForShutdown, Qt::DirectConnection);
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
    if (serialWorkspace_) {
        serialWorkspace_->prepareForShutdown();
    }
    ++agentGeneration_;
    agentConnecting_ = false;
    if (agentClient_) {
        agentClient_->close();
        agentClient_.reset();
    }
    if (fakeAgentProcess_ && fakeAgentProcess_->state() != QProcess::NotRunning) {
        fakeAgentProcess_->terminate();
        if (!fakeAgentProcess_->waitForFinished(1000)) {
            fakeAgentProcess_->kill();
            fakeAgentProcess_->waitForFinished(1000);
        }
    }
    for (auto &thread : scanThreads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    scanThreads_.clear();
    for (auto &thread : agentThreads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    agentThreads_.clear();
    QCoreApplication::removePostedEvents(this);
}

bool MainWindow::navigateTo(const QString &pageName)
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

void MainWindow::startAgentDemo()
{
    startLocalFakeAgent();
}

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("TestTools"));
    resize(1440, 900);
    setMinimumSize(1120, 720);
    setStyleSheet(testtools::ui::applicationStyleSheet());

    auto *surface = new QWidget(this);
    surface->setObjectName(QStringLiteral("appSurface"));
    auto *root = new QVBoxLayout(surface);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(buildTopBar());

    auto *body = new QWidget(surface);
    auto *bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);
    bodyLayout->addWidget(buildNavigation());

    pages_ = new QStackedWidget(body);
    pages_->addWidget(buildOverviewPage());
    pages_->addWidget(buildSerialPage());
    pages_->addWidget(buildTestCenterPage());
    pages_->addWidget(buildEmptyPage(QStringLiteral("运行记录"),
                                     QStringLiteral("按设备和任务回看参数、结果与产物"),
                                     QStringLiteral("尚无运行记录"),
                                     QStringLiteral("Agent 任务模型接入后，这里会保存每次运行的设备快照、参数、结果和产物索引。")));
    pages_->addWidget(buildSettingsPage());
    bodyLayout->addWidget(pages_, 1);
    root->addWidget(body, 1);

    auto *activity = new QFrame(surface);
    activity->setObjectName(QStringLiteral("activityBar"));
    auto *activityLayout = new QHBoxLayout(activity);
    activityLayout->setContentsMargins(18, 9, 18, 9);
    auto *activityTitle = new QLabel(QStringLiteral("活动"), activity);
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

QWidget *MainWindow::buildTopBar()
{
    auto *bar = new QFrame(this);
    bar->setObjectName(QStringLiteral("topBar"));
    auto *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(18, 11, 16, 11);
    layout->setSpacing(12);

    auto *brand = new QHBoxLayout;
    brand->setSpacing(8);
    auto *title = new QLabel(QStringLiteral("TestTools"), bar);
    title->setObjectName(QStringLiteral("appTitle"));
    auto *subtitle = new QLabel(QStringLiteral("板卡调试与测试工作台"), bar);
    subtitle->setObjectName(QStringLiteral("appSubtitle"));
    brand->addWidget(title);
    brand->addWidget(subtitle);
    layout->addLayout(brand);
    layout->addSpacing(20);

    auto *caption = new QLabel(QStringLiteral("设备"), bar);
    caption->setObjectName(QStringLiteral("topCaption"));
    deviceCombo_ = new QComboBox(bar);
    deviceCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    deviceCombo_->setMinimumContentsLength(28);
    adbStatusLabel_ = new QLabel(QStringLiteral("ADB · 检测中"), bar);
    adbStatusLabel_->setObjectName(QStringLiteral("topStatusNeutral"));
    agentStatusLabel_ = new QLabel(QStringLiteral("Agent · 未连接"), bar);
    agentStatusLabel_->setObjectName(QStringLiteral("topStatusNeutral"));
    agentActionButton_ = new QPushButton(QStringLiteral("管理"), bar);
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

QWidget *MainWindow::buildNavigation()
{
    auto *rail = new QFrame(this);
    rail->setObjectName(QStringLiteral("navigationRail"));
    rail->setFixedWidth(164);
    auto *layout = new QVBoxLayout(rail);
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

QPushButton *MainWindow::addNavigationButton(const QString &text, int pageIndex)
{
    auto *button = new QPushButton(text, this);
    button->setObjectName(QStringLiteral("navigationButton"));
    button->setCheckable(true);
    button->setCursor(Qt::PointingHandCursor);
    connect(button, &QPushButton::clicked, this, [this, pageIndex] { showPage(pageIndex); });
    navigationButtons_.push_back(button);
    return button;
}

QWidget *MainWindow::buildOverviewPage()
{
    auto *content = new QWidget(this);
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(14);

    auto *contextPanel = new QFrame(content);
    contextPanel->setObjectName(QStringLiteral("contextPanel"));
    auto *summaries = new QHBoxLayout(contextPanel);
    summaries->setContentsMargins(4, 0, 4, 0);
    summaries->setSpacing(0);
    const auto addDivider = [contextPanel, summaries] {
        auto *divider = new QFrame(contextPanel);
        divider->setObjectName(QStringLiteral("summaryDivider"));
        divider->setFixedWidth(1);
        divider->setMinimumHeight(54);
        summaries->addWidget(divider);
    };
    summaries->addWidget(makeSummaryCard(contextPanel, QStringLiteral("当前设备"), overviewDeviceValue_,
                                         QStringLiteral("所有操作都会作用于这台板卡")), 1);
    addDivider();
    summaries->addWidget(makeSummaryCard(contextPanel, QStringLiteral("ADB"), overviewAdbValue_,
                                         QStringLiteral("负责发现、部署和安全隧道")), 1);
    addDivider();
    summaries->addWidget(makeSummaryCard(contextPanel, QStringLiteral("板端 Agent"), overviewAgentValue_,
                                         QStringLiteral("后续测试能力的统一入口")), 1);
    layout->addWidget(contextPanel);

    auto *quickStart = new QFrame(content);
    quickStart->setObjectName(QStringLiteral("featureCard"));
    auto *quickLayout = new QVBoxLayout(quickStart);
    quickLayout->setContentsMargins(18, 16, 18, 16);
    auto *quickTitle = new QLabel(QStringLiteral("快速开始"), quickStart);
    quickTitle->setObjectName(QStringLiteral("sectionLabel"));
    auto *quickBody = new QLabel(QStringLiteral("现有串口能力已经可用。选择设备后进入串口工作台，配置两个端点并进行双向验证。"), quickStart);
    quickBody->setObjectName(QStringLiteral("cardBody"));
    quickBody->setWordWrap(true);
    auto *quickActions = new QHBoxLayout;
    auto *openSerial = new QPushButton(QStringLiteral("打开串口工作台"), quickStart);
    openSerial->setObjectName(QStringLiteral("primaryButton"));
    auto *openTests = new QPushButton(QStringLiteral("查看测试演进"), quickStart);
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
                    QStringLiteral("先确认操作对象和连接健康，再进入具体调试任务"), content);
}

QWidget *MainWindow::buildSerialPage()
{
    serialWorkspace_ = new SerialWorkspace(this);
    connect(serialWorkspace_, &SerialWorkspace::activityChanged, this, &MainWindow::setActivity);
    connect(serialWorkspace_, &SerialWorkspace::busyChanged, this, [this](bool busy) {
        serialBusy_ = busy;
        updateDeviceControls();
    });
    return wrapPage(QStringLiteral("串口工作台"),
                    QStringLiteral("配置两个端点，进行收发监视和双向链路验证"),
                    serialWorkspace_, false);
}

QWidget *MainWindow::buildTestCenterPage()
{
    auto *content = new QWidget(this);
    auto *layout = new QGridLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setHorizontalSpacing(12);
    layout->setVerticalSpacing(12);

    layout->addWidget(makeFeatureCard(content, QStringLiteral("串口"), QStringLiteral("可用"),
                                      QStringLiteral("Windows 本地串口与 ADB 板端设备节点收发、监视和双向验证。"), true), 0, 0);
    layout->addWidget(makeFeatureCard(content, QStringLiteral("DDR 稳定性"), QStringLiteral("需要 Agent"),
                                      QStringLiteral("容量、固定/动态频率压力、休眠、自刷新和重启恢复。"), false), 0, 1);
    layout->addWidget(makeFeatureCard(content, QStringLiteral("存储"), QStringLiteral("需要 Agent"),
                                      QStringLiteral("eMMC/UFS 身份、寿命、性能和数据完整性验证。"), false), 1, 0);
    layout->addWidget(makeFeatureCard(content, QStringLiteral("音频与摄像头"), QStringLiteral("需要 Agent"),
                                      QStringLiteral("设备枚举、采集、回放、波形和图像产物。"), false), 1, 1);
    layout->setRowStretch(2, 1);
    layout->setColumnStretch(0, 1);
    layout->setColumnStretch(1, 1);
    return wrapPage(QStringLiteral("测试中心"),
                    QStringLiteral("能力将由板端 Agent 发现；复杂测试保留专用交互"), content);
}

QWidget *MainWindow::buildSettingsPage()
{
    auto *content = new QWidget(this);
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    auto *experienceCard = new QFrame(content);
    experienceCard->setObjectName(QStringLiteral("featureCard"));
    auto *experienceLayout = new QVBoxLayout(experienceCard);
    experienceLayout->setContentsMargins(18, 16, 18, 16);
    experienceLayout->setSpacing(8);
    auto *experienceTitle = new QLabel(QStringLiteral("快速体验 · 本机假 Agent"), experienceCard);
    experienceTitle->setObjectName(QStringLiteral("sectionLabel"));
    auto *experienceBody = new QLabel(
        QStringLiteral("启动随程序构建的无 Qt 假服务，验证 Agent v1 握手和能力发现。它不操作板卡，也不执行 shell。"),
        experienceCard);
    experienceBody->setObjectName(QStringLiteral("cardBody"));
    experienceBody->setWordWrap(true);
    startFakeAgentButton_ = new QPushButton(QStringLiteral("启动并连接假 Agent"), experienceCard);
    startFakeAgentButton_->setObjectName(QStringLiteral("primaryButton"));
    auto *experienceActions = new QHBoxLayout;
    experienceActions->addWidget(startFakeAgentButton_);
    experienceActions->addStretch();
    experienceLayout->addWidget(experienceTitle);
    experienceLayout->addWidget(experienceBody);
    experienceLayout->addLayout(experienceActions);
    layout->addWidget(experienceCard);

    auto *connectionCard = new QFrame(content);
    connectionCard->setObjectName(QStringLiteral("featureCard"));
    auto *connectionLayout = new QVBoxLayout(connectionCard);
    connectionLayout->setContentsMargins(18, 16, 18, 16);
    connectionLayout->setSpacing(10);
    auto *connectionTitle = new QLabel(QStringLiteral("Agent 连接"), connectionCard);
    connectionTitle->setObjectName(QStringLiteral("sectionLabel"));
    auto *connectionBody = new QLabel(
        QStringLiteral("当前仅允许连接 127.0.0.1。真实板端 Agent 应先通过 ADB forward 映射到本机端口。"),
        connectionCard);
    connectionBody->setObjectName(QStringLiteral("cardBody"));
    connectionBody->setWordWrap(true);
    connectionLayout->addWidget(connectionTitle);
    connectionLayout->addWidget(connectionBody);

    auto *form = new QFormLayout;
    agentHostEdit_ = new QLineEdit(QStringLiteral("127.0.0.1"), connectionCard);
    agentHostEdit_->setPlaceholderText(QStringLiteral("127.0.0.1"));
    agentPortSpin_ = new QSpinBox(connectionCard);
    agentPortSpin_->setRange(1, 65535);
    agentPortSpin_->setValue(27183);
    form->addRow(QStringLiteral("主机"), agentHostEdit_);
    form->addRow(QStringLiteral("端口"), agentPortSpin_);
    connectionLayout->addLayout(form);

    auto *actions = new QHBoxLayout;
    connectAgentButton_ = new QPushButton(QStringLiteral("连接并发现能力"), connectionCard);
    connectAgentButton_->setObjectName(QStringLiteral("primaryButton"));
    disconnectAgentButton_ = new QPushButton(QStringLiteral("断开"), connectionCard);
    actions->addWidget(connectAgentButton_);
    actions->addWidget(disconnectAgentButton_);
    actions->addStretch();
    connectionLayout->addLayout(actions);

    agentConnectionDetail_ = new QLabel(QStringLiteral("尚未连接 Agent"), connectionCard);
    agentConnectionDetail_->setObjectName(QStringLiteral("cardBody"));
    agentConnectionDetail_->setWordWrap(true);
    agentConnectionDetail_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    agentCapabilitiesValue_ = new QLabel(QStringLiteral("已发现能力：—"), connectionCard);
    agentCapabilitiesValue_->setObjectName(QStringLiteral("cardBody"));
    agentCapabilitiesValue_->setWordWrap(true);
    agentCapabilitiesValue_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    connectionLayout->addWidget(agentConnectionDetail_);
    connectionLayout->addWidget(agentCapabilitiesValue_);
    layout->addWidget(connectionCard);
    layout->addStretch();

    fakeAgentProcess_ = new QProcess(this);
#ifdef Q_OS_WIN
    fakeAgentProcess_->setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *arguments) { arguments->flags |= CREATE_NO_WINDOW; });
#endif
    connect(fakeAgentProcess_, &QProcess::readyReadStandardOutput, this, [this] {
        fakeAgentOutput_ += QString::fromUtf8(fakeAgentProcess_->readAllStandardOutput());
        if (fakeAgentOutput_.contains(QStringLiteral("LISTENING"))
            && !agentConnecting_ && !agentClient_) {
            connectAgentEndpoint();
        }
    });
    connect(fakeAgentProcess_, &QProcess::readyReadStandardError, this, [this] {
        const QString error = QString::fromUtf8(fakeAgentProcess_->readAllStandardError()).trimmed();
        if (!error.isEmpty() && agentConnectionDetail_) {
            agentConnectionDetail_->setText(QStringLiteral("假 Agent：%1").arg(error));
        }
    });
    connect(fakeAgentProcess_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        if (!shuttingDown_) {
            setAgentStatus(QStringLiteral("Agent · 启动失败"), StatusTone::Error,
                           fakeAgentProcess_->errorString());
            updateAgentControls();
        }
    });
    connect(fakeAgentProcess_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus) {
        if (!shuttingDown_ && agentClient_) {
            disconnectAgent(QStringLiteral("本机假 Agent 已退出"));
        } else if (!shuttingDown_ && agentStatusLabel_
                   && agentStatusLabel_->text().contains(QStringLiteral("启动中"))) {
            setAgentStatus(QStringLiteral("Agent · 启动失败"), StatusTone::Error,
                           QStringLiteral("假 Agent 已退出，退出码 %1").arg(exitCode));
        }
        updateAgentControls();
    });

    connect(startFakeAgentButton_, &QPushButton::clicked, this, &MainWindow::startLocalFakeAgent);
    connect(connectAgentButton_, &QPushButton::clicked, this, &MainWindow::connectAgentEndpoint);
    connect(disconnectAgentButton_, &QPushButton::clicked, this, [this] {
        disconnectAgent(QStringLiteral("Agent 已断开"));
    });
    updateAgentControls();
    return wrapPage(QStringLiteral("设置"),
                    QStringLiteral("连接 Agent，查看协议版本和设备能力"), content);
}

QWidget *MainWindow::buildEmptyPage(const QString &title, const QString &subtitle,
                                    const QString &heading, const QString &body)
{
    auto *content = new QFrame(this);
    content->setObjectName(QStringLiteral("featureCard"));
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(20, 18, 20, 18);
    auto *headingLabel = new QLabel(heading, content);
    headingLabel->setObjectName(QStringLiteral("sectionLabel"));
    auto *bodyLabel = new QLabel(body, content);
    bodyLabel->setObjectName(QStringLiteral("cardBody"));
    bodyLabel->setWordWrap(true);
    layout->addWidget(headingLabel);
    layout->addWidget(bodyLabel);
    layout->addStretch();
    return wrapPage(title, subtitle, content);
}

QWidget *MainWindow::wrapPage(const QString &title, const QString &subtitle, QWidget *content,
                              bool scrollable)
{
    auto *page = new QWidget(this);
    page->setObjectName(QStringLiteral("pageSurface"));
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(13);

    auto *titleLabel = new QLabel(title, page);
    titleLabel->setObjectName(QStringLiteral("pageTitle"));
    auto *subtitleLabel = new QLabel(subtitle, page);
    subtitleLabel->setObjectName(QStringLiteral("pageSubtitle"));
    subtitleLabel->setWordWrap(true);
    layout->addWidget(titleLabel);
    layout->addWidget(subtitleLabel);

    if (scrollable) {
        auto *scroll = new QScrollArea(page);
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

void MainWindow::startLocalFakeAgent()
{
    if (!fakeAgentProcess_ || agentConnecting_ || agentClient_) {
        return;
    }
    if (fakeAgentProcess_->state() != QProcess::NotRunning) {
        connectAgentEndpoint();
        return;
    }

    const QString executable = QDir(QCoreApplication::applicationDirPath())
                                   .filePath(QStringLiteral("testtools-agent.exe"));
    if (!QFileInfo(executable).isFile()) {
        setAgentStatus(QStringLiteral("Agent · 缺少程序"), StatusTone::Error,
                       QStringLiteral("未找到 %1，请先构建完整解决方案").arg(executable));
        updateAgentControls();
        return;
    }

    agentHostEdit_->setText(QStringLiteral("127.0.0.1"));
    fakeAgentOutput_.clear();
    fakeAgentProcess_->setProgram(executable);
    fakeAgentProcess_->setArguments({QStringLiteral("--port"),
                                     QString::number(agentPortSpin_->value())});
    fakeAgentProcess_->setWorkingDirectory(QCoreApplication::applicationDirPath());
    setAgentStatus(QStringLiteral("Agent · 启动中"), StatusTone::Neutral,
                   QStringLiteral("正在启动本机假 Agent"));
    fakeAgentProcess_->start();
    updateAgentControls();
}

void MainWindow::connectAgentEndpoint()
{
    if (agentConnecting_ || agentClient_ || shuttingDown_) {
        return;
    }
    QString host = agentHostEdit_->text().trimmed().toLower();
    if (host == QStringLiteral("localhost") || host == QStringLiteral("::1")) {
        host = QStringLiteral("127.0.0.1");
        agentHostEdit_->setText(host);
    }
    if (host != QStringLiteral("127.0.0.1")) {
        setAgentStatus(QStringLiteral("Agent · 地址被拒绝"), StatusTone::Error,
                       QStringLiteral("当前安全策略只允许本机或 ADB 转发地址 127.0.0.1"));
        return;
    }

    const std::string nativeHost = host.toStdString();
    const std::uint16_t port = static_cast<std::uint16_t>(agentPortSpin_->value());
    const std::uint64_t generation = ++agentGeneration_;
    agentConnecting_ = true;
    setAgentStatus(QStringLiteral("Agent · 连接中"), StatusTone::Neutral,
                   QStringLiteral("正在进行协议协商与能力发现"));
    updateAgentControls();

    QPointer<MainWindow> guard(this);
    agentThreads_.emplace_back([guard, nativeHost, port, generation] {
        auto client = std::make_shared<testtools::AgentClient>();
        std::string error;
        std::string hello;
        std::string capabilities;
        bool succeeded = client->connect(nativeHost, port, error);
        if (succeeded) {
            succeeded = client->request(
                "{\"protocol\":1,\"request_id\":\"ui-hello\","
                "\"method\":\"session.hello\",\"params\":{}}",
                hello, error);
        }
        if (succeeded) {
            succeeded = client->request(
                "{\"protocol\":1,\"request_id\":\"ui-capabilities\","
                "\"method\":\"capability.list\",\"params\":{}}",
                capabilities, error);
        }
        QMetaObject::invokeMethod(guard,
            [guard, client = std::move(client), hello = std::move(hello),
             capabilities = std::move(capabilities), error = std::move(error),
             succeeded, nativeHost, port, generation]() mutable {
                if (!guard || guard->shuttingDown_ || generation != guard->agentGeneration_) {
                    client->close();
                    return;
                }
                guard->agentConnecting_ = false;
                if (!succeeded) {
                    client->close();
                    guard->setAgentStatus(QStringLiteral("Agent · 连接失败"), StatusTone::Error,
                                          fromUtf8(error));
                    guard->updateAgentControls();
                    return;
                }

                guard->agentClient_ = std::move(client);
                const QString version = jsonStringValue(hello, "agent_version");
                const bool systemInfo = capabilities.find("\"id\":\"system.info\"")
                                        != std::string::npos;
                guard->agentSummary_ = version.isEmpty()
                    ? QStringLiteral("已连接")
                    : QStringLiteral("v%1 · 已连接").arg(version);
                guard->setAgentStatus(QStringLiteral("Agent · %1").arg(guard->agentSummary_),
                                      StatusTone::Good,
                                      QStringLiteral("%1:%2，协议 v1 协商成功")
                                          .arg(fromUtf8(nativeHost)).arg(port));
                guard->agentCapabilitiesValue_->setText(
                    systemInfo ? QStringLiteral("已发现能力：system.info（只读）")
                               : QStringLiteral("已发现能力：Agent 未返回已知能力"));
                guard->agentCapabilitiesValue_->setToolTip(fromUtf8(capabilities));
                guard->setActivity(QStringLiteral("Agent v1 握手与能力发现完成"));
                guard->updateAgentControls();
            }, Qt::QueuedConnection);
    });
}

void MainWindow::disconnectAgent(const QString &reason)
{
    ++agentGeneration_;
    agentConnecting_ = false;
    if (agentClient_) {
        agentClient_->close();
        agentClient_.reset();
    }
    if (fakeAgentProcess_ && fakeAgentProcess_->state() != QProcess::NotRunning) {
        fakeAgentProcess_->terminate();
    }
    agentSummary_ = QStringLiteral("未连接");
    setAgentStatus(QStringLiteral("Agent · 未连接"), StatusTone::Neutral,
                   reason.isEmpty() ? QStringLiteral("尚未连接 Agent") : reason);
    if (agentCapabilitiesValue_) {
        agentCapabilitiesValue_->setText(QStringLiteral("已发现能力：—"));
        agentCapabilitiesValue_->setToolTip({});
    }
    if (!reason.isEmpty()) {
        setActivity(reason);
    }
    updateAgentControls();
}

void MainWindow::setAgentStatus(const QString &text, StatusTone tone, const QString &detail)
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
    if (!connectAgentButton_) {
        return;
    }
    const bool connected = static_cast<bool>(agentClient_);
    const bool fakeRunning = fakeAgentProcess_
                             && fakeAgentProcess_->state() != QProcess::NotRunning;
    agentHostEdit_->setEnabled(!agentConnecting_ && !connected && !fakeRunning);
    agentPortSpin_->setEnabled(!agentConnecting_ && !connected && !fakeRunning);
    connectAgentButton_->setEnabled(!agentConnecting_ && !connected && !fakeRunning);
    disconnectAgentButton_->setEnabled(agentConnecting_ || connected || fakeRunning);
    startFakeAgentButton_->setEnabled(!agentConnecting_ && !connected && !fakeRunning);
    startFakeAgentButton_->setText(connected && fakeRunning ? QStringLiteral("假 Agent 已连接")
                                       : fakeRunning ? QStringLiteral("假 Agent 启动中")
                                                     : QStringLiteral("启动并连接假 Agent"));
    startFakeAgentButton_->setObjectName(connected || fakeRunning
                                             ? QStringLiteral("ghostButton")
                                             : QStringLiteral("primaryButton"));
    startFakeAgentButton_->style()->unpolish(startFakeAgentButton_);
    startFakeAgentButton_->style()->polish(startFakeAgentButton_);
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
    scanThreads_.emplace_back([guard] {
        testtools::DeviceDiscovery discovery;
        auto result = discovery.scanAdbDevices();
        QMetaObject::invokeMethod(guard, [guard, result = std::move(result)]() mutable {
            if (guard && !guard->shuttingDown_) {
                guard->applyDeviceScan(std::move(result));
            }
        }, Qt::QueuedConnection);
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
    for (const auto &device : result.devices) {
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
    if (!result.error.empty()) {
        setAdbStatus(QStringLiteral("ADB · 异常"), StatusTone::Error);
        setActivity(fromUtf8(result.error));
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
    device.description.assign(description.constData(), static_cast<std::size_t>(description.size()));
    return device;
}

void MainWindow::applySelectedDevice()
{
    if (!serialWorkspace_) {
        return;
    }
    serialWorkspace_->setAdbDevice(selectedDevice());
    updateOverview();
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

void MainWindow::setAdbStatus(const QString &text, StatusTone tone)
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

void MainWindow::setActivity(const QString &message)
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
    overviewDeviceValue_->setText(device.serial.empty() ? QStringLiteral("未选择") : fromUtf8(device.serial));
    overviewAdbValue_->setText(adbStatusLabel_ ? adbStatusLabel_->text() : QStringLiteral("检测中"));
    overviewAgentValue_->setText(agentSummary_.isEmpty() ? QStringLiteral("未连接") : agentSummary_);
}

void MainWindow::updateDeviceControls()
{
    const bool enabled = !scanning_ && !serialBusy_;
    refreshButton_->setEnabled(enabled);
    deviceCombo_->setEnabled(enabled && !deviceCombo_->currentData().toString().isEmpty());
}
