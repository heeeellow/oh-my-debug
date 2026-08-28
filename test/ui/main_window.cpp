#include "main_window.h"

#include "theme.h"
#include "ui/pages/serial_workspace.h"

#include <QtCore/QCoreApplication>
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

QString fromUtf8(const std::string &text)
{
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
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
    for (auto &thread : scanThreads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    scanThreads_.clear();
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
    pages_->addWidget(buildEmptyPage(QStringLiteral("设置"),
                                     QStringLiteral("连接、Agent、日志保留和外观"),
                                     QStringLiteral("设置项将在需要时出现"),
                                     QStringLiteral("第一阶段沿用现有 ADB 和串口配置，避免提前增加没有实际作用的选项。")));
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
    refreshButton_ = new QPushButton(QStringLiteral("刷新设备"), bar);
    refreshButton_->setObjectName(QStringLiteral("topGhostButton"));

    layout->addWidget(caption);
    layout->addWidget(deviceCombo_, 1);
    layout->addWidget(adbStatusLabel_);
    layout->addWidget(agentStatusLabel_);
    layout->addWidget(refreshButton_);

    connect(refreshButton_, &QPushButton::clicked, this, &MainWindow::refreshDevices);
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
    overviewAgentValue_->setText(QStringLiteral("未连接"));
}

void MainWindow::updateDeviceControls()
{
    const bool enabled = !scanning_ && !serialBusy_;
    refreshButton_->setEnabled(enabled);
    deviceCombo_->setEnabled(enabled && !deviceCombo_->currentData().toString().isEmpty());
}
