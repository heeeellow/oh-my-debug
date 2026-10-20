#include "serial_workspace.h"

#include "application/serial/serial_transport_service.h"
#include "core/serial/codec.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QEvent>
#include <QtCore/QFile>
#include <QtCore/QMetaObject>
#include <QtCore/QPointer>
#include <QtCore/QSignalBlocker>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtGui/QKeyEvent>
#include <QtGui/QTextBlock>
#include <QtGui/QTextCursor>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QStyle>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <functional>
#include <utility>

using namespace serialdemo;

namespace {

QString fromUtf8(const std::string &text)
{
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

std::string toUtf8(const QString &text)
{
    const QByteArray bytes = text.toUtf8();
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

QString channelName(int index)
{
    return index == 0 ? QStringLiteral("端点 A") : QStringLiteral("端点 B");
}

LineEnding lineEndingFromIndex(int index)
{
    switch (index) {
    case 1:
        return LineEnding::Lf;
    case 2:
        return LineEnding::Cr;
    case 3:
        return LineEnding::CrLf;
    default:
        return LineEnding::None;
    }
}

testtools::AtCommandLineEnding storedLineEndingFromIndex(int index)
{
    switch (index) {
    case 1:
        return testtools::AtCommandLineEnding::Lf;
    case 2:
        return testtools::AtCommandLineEnding::Cr;
    case 3:
        return testtools::AtCommandLineEnding::CrLf;
    default:
        return testtools::AtCommandLineEnding::None;
    }
}

int storedLineEndingIndex(testtools::AtCommandLineEnding ending)
{
    switch (ending) {
    case testtools::AtCommandLineEnding::Lf:
        return 1;
    case testtools::AtCommandLineEnding::Cr:
        return 2;
    case testtools::AtCommandLineEnding::CrLf:
        return 3;
    default:
        return 0;
    }
}

void setVisualRole(QLabel *label, const QString &objectName)
{
    label->setObjectName(objectName);
    label->style()->unpolish(label);
    label->style()->polish(label);
}

} // namespace

SerialWorkspace::SerialWorkspace(QWidget *parent)
    : QWidget(parent)
{
    const auto configuredStorage = qEnvironmentVariable("TESTTOOLS_DATA_DIR");
    const auto storageRoot = configuredStorage.isEmpty()
                                 ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
                                 : configuredStorage;
    atCommandStorePath_ =
        std::filesystem::path(storageRoot.toStdWString()) / L"at-commands.json";
    std::error_code existsError;
    const bool commandFileExists = std::filesystem::exists(atCommandStorePath_, existsError);
    std::string commandStoreError;
    const bool loaded = !existsError &&
                        testtools::AtCommandStore::load(
                            atCommandStorePath_, atCommandLibrary_, commandStoreError);
    if (loaded && !commandFileExists)
        testtools::AtCommandStore::save(
            atCommandStorePath_, atCommandLibrary_, commandStoreError);

    preferencesStorePath_ =
        std::filesystem::path(storageRoot.toStdWString()) / L"serial-preferences.json";
    std::error_code prefExistsError;
    const bool prefFileExists = std::filesystem::exists(preferencesStorePath_, prefExistsError);
    std::string prefStoreError;
    preferencesLoaded_ = !prefExistsError && prefFileExists &&
                         testtools::SerialPreferencesStore::load(
                             preferencesStorePath_, preferences_, prefStoreError);
    if (!preferencesLoaded_) {
        preferences_.channels[1].lineEnding =
            storedLineEndingIndex(atCommandLibrary_.lineEnding());
    }

    buildUi();
    applyPreferencesToUi();
    refreshAtCommandChoices();
    for (int index = 0; index < static_cast<int>(channels_.size()); ++index) {
        attachSessionCallbacks(index);
    }
    refreshTargets();
    if (!commandStoreError.empty()) {
        const QString message = QStringLiteral("AT \u6307\u4ee4\u5e93\u672a\u80fd\u4ece\u672c\u5730\u6587\u4ef6\u6062\u590d\uff1a%1")
                                    .arg(fromUtf8(commandStoreError));
        QTimer::singleShot(0, this, [this, message] { emit activityChanged(message); });
    }
}

SerialWorkspace::~SerialWorkspace()
{
    prepareForShutdown();
}

bool SerialWorkspace::setWorkspaceModeForDiagnostics(const QString &modeName)
{
    const QString normalized = modeName.trimmed().toLower();
    WorkspaceMode mode = WorkspaceMode::Single;
    if (normalized == QStringLiteral("dual")) {
        mode = WorkspaceMode::Dual;
    } else if (normalized != QStringLiteral("single")) {
        return false;
    }
    setWorkspaceMode(mode);
    return workspaceState_.mode() == mode;
}

void SerialWorkspace::prepareForShutdown()
{
    if (shuttingDown_) {
        return;
    }
    shuttingDown_ = true;
    if (scheduleTimer_) {
        scheduleTimer_->stop();
    }
    if (stressTestRunning_) {
        stopStressTest(QStringLiteral("应用正在退出"));
    }
    disconnectAll();
    scanTasks_.wait();
    QCoreApplication::removePostedEvents(this);
}

void SerialWorkspace::buildUi()
{
    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(8);

    auto *modeBar = new QFrame(this);
    modeBar->setObjectName(QStringLiteral("taskModeBar"));
    auto *modeLayout = new QHBoxLayout(modeBar);
    modeLayout->setContentsMargins(2, 0, 2, 2);
    modeLayout->setSpacing(12);
    auto *pageTitle = new QLabel(QStringLiteral("串口调试"), modeBar);
    pageTitle->setObjectName(QStringLiteral("pageTitle"));
    modeLayout->addWidget(pageTitle);

    auto *segmentedBar = new QFrame(modeBar);
    segmentedBar->setObjectName(QStringLiteral("segmentedBar"));
    auto *segmentedLayout = new QHBoxLayout(segmentedBar);
    segmentedLayout->setContentsMargins(3, 3, 3, 3);
    segmentedLayout->setSpacing(2);

    auto *modeGroup = new QButtonGroup(this);
    modeGroup->setExclusive(true);
    const std::array<QString, 2> modeNames{
        QStringLiteral("单串口调试"),
        QStringLiteral("双端调试"),
    };
    for (int index = 0; index < static_cast<int>(modeButtons_.size()); ++index) {
        auto *button = new QPushButton(modeNames[static_cast<std::size_t>(index)], segmentedBar);
        button->setObjectName(QStringLiteral("modeButton"));
        button->setCheckable(true);
        button->setChecked(index == 0);
        modeGroup->addButton(button, index);
        modeButtons_[static_cast<std::size_t>(index)] = button;
        segmentedLayout->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, index] {
            setWorkspaceMode(static_cast<WorkspaceMode>(index));
        });
    }
    modeLayout->addWidget(segmentedBar);
    modeDescriptionLabel_ = new QLabel(modeBar);
    modeDescriptionLabel_->setObjectName(QStringLiteral("modeDescription"));
    modeDescriptionLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    modeLayout->addStretch();
    modeLayout->addWidget(modeDescriptionLabel_);
    rootLayout->addWidget(modeBar);

    linkBar_ = new QFrame(this);
    linkBar_->setObjectName(QStringLiteral("linkBar"));
    auto *linkLayout = new QHBoxLayout(linkBar_);
    linkLayout->setContentsMargins(10, 4, 10, 5);
    linkLayout->setSpacing(6);
    auto *routeArea = new QVBoxLayout;
    routeArea->setContentsMargins(0, 0, 0, 0);
    routeArea->setSpacing(2);
    routeLabel_ = new QLabel(QStringLiteral("A  未选择   ⇄   B  未选择"), linkBar_);
    routeLabel_->setObjectName(QStringLiteral("routeSummary"));
    testResultLabel_ = new QLabel(QStringLiteral("双向收发验证 · 未运行"), linkBar_);
    testResultLabel_->setObjectName(QStringLiteral("testStatusNeutral"));
    stressResultLabel_ = new QLabel(QStringLiteral("可靠性压测 · 未运行"), linkBar_);
    stressResultLabel_->setObjectName(QStringLiteral("testStatusNeutral"));
    stressResultLabel_->setToolTip(
        QStringLiteral("BER 统计已成功解析帧的有效载荷；丢帧和格式错误单独显示"));

    auto *testStatusLayout = new QHBoxLayout;
    testStatusLayout->setContentsMargins(0, 0, 0, 0);
    testStatusLayout->setSpacing(12);
    testStatusLayout->addWidget(testResultLabel_);
    testStatusLayout->addWidget(stressResultLabel_);
    testStatusLayout->addStretch();

    routeArea->addWidget(routeLabel_);
    routeArea->addLayout(testStatusLayout);
    linkLayout->addLayout(routeArea, 1);

    connectBothButton_ = new QPushButton(QStringLiteral("连接两端"), linkBar_);
    connectBothButton_->setObjectName(QStringLiteral("primaryButton"));
    disconnectBothButton_ = new QPushButton(QStringLiteral("断开两端"), linkBar_);
    bidirectionalTestButton_ = new QPushButton(QStringLiteral("双向收发验证"), linkBar_);
    bidirectionalTestButton_->setObjectName(QStringLiteral("verifyButton"));
    bidirectionalTestButton_->setToolTip(
        QStringLiteral("需要先连接端点 A 和 B；会向两端各发送一条临时测试帧"));
    stressTestButton_ = new QPushButton(QStringLiteral("可靠性压测…"), linkBar_);
    stressTestButton_->setObjectName(QStringLiteral("verifyButton"));
    stressTestButton_->setToolTip(
        QStringLiteral("需要先连接端点 A 和 B；打开参数面板后才会开始发送测试帧"));
    linkLayout->addWidget(connectBothButton_);
    linkLayout->addWidget(disconnectBothButton_);
    linkLayout->addWidget(bidirectionalTestButton_);
    linkLayout->addWidget(stressTestButton_);
    rootLayout->addWidget(linkBar_);

    channelSplitter_ = new QSplitter(Qt::Horizontal, this);
    channelSplitter_->setChildrenCollapsible(false);
    channelSplitter_->addWidget(buildChannelPanel(0));
    channelSplitter_->addWidget(buildChannelPanel(1));
    channelSplitter_->setStretchFactor(0, 1);
    channelSplitter_->setStretchFactor(1, 1);
    rootLayout->addWidget(channelSplitter_, 1);

    connect(connectBothButton_, &QPushButton::clicked, this, [this] {
        QString error;
        for (int index = 0; index < 2; ++index) {
            if (!channelActive(index) && !validateChannel(index, error)) {
                QMessageBox::warning(this, QStringLiteral("无法连接两路"), error);
                return;
            }
        }
        if (!channelActive(0) && !channelActive(1)) {
            auto first = endpointSelection(0);
            auto second = endpointSelection(1);
            first.active = true;
            second.active = true;
            const auto conflict = testtools::SerialWorkspaceState::endpointConflict(first, second);
            if (conflict == testtools::SerialEndpointConflict::AgentSingleSession) {
                QMessageBox::information(
                    this,
                    QStringLiteral("当前为单板串口会话"),
                    QStringLiteral("当前设备连接一次只能使用一个设备串口。可同时使用一路 Windows "
                                   "串口和一路设备串口。"));
                return;
            }
            if (conflict == testtools::SerialEndpointConflict::SameResource) {
                QMessageBox::warning(this,
                                     QStringLiteral("无法连接两路"),
                                     QStringLiteral("端点 A 和 B 不能同时打开同一个串口。"));
                return;
            }
        }
        for (int index = 0; index < 2; ++index) {
            if (!channelActive(index)) {
                connectChannel(index);
            }
        }
    });
    connect(disconnectBothButton_, &QPushButton::clicked, this, &SerialWorkspace::disconnectAll);
    connect(bidirectionalTestButton_,
            &QPushButton::clicked,
            this,
            &SerialWorkspace::runBidirectionalTest);
    connect(stressTestButton_, &QPushButton::clicked, this, [this] {
        if (stressTestRunning_) {
            stopStressTest(QStringLiteral("用户已停止可靠性压测"));
        } else {
            showStressDialog();
        }
    });

    scheduleTimer_ = new QTimer(this);
    scheduleTimer_->setInterval(10);
    connect(scheduleTimer_, &QTimer::timeout, this, &SerialWorkspace::processScheduledSends);
    scheduleTimer_->start();

    updateRouteSummary();
    applyWorkspaceMode();
    updateGlobalControls();
}

void SerialWorkspace::setWorkspaceMode(WorkspaceMode mode)
{
    if (mode == workspaceState_.mode()) {
        applyWorkspaceMode();
        return;
    }
    if (!canSwitchWorkspaceMode(mode)) {
        modeButtons_[static_cast<std::size_t>(workspaceState_.mode())]->setChecked(true);
        return;
    }
    testtools::SerialWorkspaceActivity activity;
    workspaceState_.applyMode(mode, activity);
    applyWorkspaceMode();
    updateGlobalControls();
}

bool SerialWorkspace::canSwitchWorkspaceMode(WorkspaceMode mode)
{
    testtools::SerialWorkspaceActivity activity;
    for (int index = 0; index < 2; ++index) {
        activity.channelActive[static_cast<std::size_t>(index)] = channelActive(index);
        activity.scheduledSendRunning[static_cast<std::size_t>(index)] =
            channels_[index].session.snapshot().scheduledSend.running;
    }
    activity.bidirectionalTestRunning = bidirectionalTestRunning_;
    activity.reliabilityTestRunning = stressTestRunning_;
    const auto decision = workspaceState_.evaluateModeSwitch(mode, activity);
    if (decision.blockedBy == testtools::SerialModeSwitchBlock::TaskRunning) {
        QMessageBox::information(
            this,
            QStringLiteral("任务正在运行"),
            QStringLiteral("请先停止当前的计划发送、双向收发验证或可靠性压测，再切换调试方式。"));
        return false;
    }
    if (decision.blockedBy == testtools::SerialModeSwitchBlock::HiddenChannelActive) {
        QMessageBox::information(
            this, QStringLiteral("端点 A 仍在使用"),
            QStringLiteral("单串口模式不会隐藏活动连接。请先断开端点 A，再切换到单串口调试。"));
        return false;
    }
    return true;
}

void SerialWorkspace::applyWorkspaceMode()
{
    if (!channels_[0].group || !channels_[1].group || !linkBar_) {
        return;
    }

    const bool single = workspaceState_.mode() == WorkspaceMode::Single;
    modeButtons_[static_cast<std::size_t>(workspaceState_.mode())]->setChecked(true);

    channels_[0].group->setVisible(!single);
    channels_[1].group->setVisible(true);
    channels_[0].manualSendArea->setVisible(true);
    channels_[1].manualSendArea->setVisible(true);
    channels_[0].atCommandArea->hide();
    channels_[0].toggleAtBarButton->hide();
    channels_[1].toggleAtBarButton->setVisible(single);
    channels_[1].atCommandArea->setVisible(single && channels_[1].toggleAtBarButton->isChecked());
    channels_[0].sendShortcutHint->setVisible(single);
    channels_[1].sendShortcutHint->setVisible(single);
    for (auto &channel : channels_) {
        channel.sendEdit->setFixedHeight(single ? 56 : 44);
        channel.receiveHexCheck->setText(single ? QStringLiteral("HEX 显示")
                                                : QStringLiteral("HEX"));
        channel.autoScrollCheck->setText(single ? QStringLiteral("自动滚动")
                                                : QStringLiteral("滚动"));
        channel.resetCounterButton->setText(single ? QStringLiteral("计数清零")
                                                   : QStringLiteral("清零"));
        channel.saveLogButton->setText(single ? QStringLiteral("保存日志…")
                                              : QStringLiteral("保存…"));
    }
    linkBar_->setVisible(!single);
    testResultLabel_->setVisible(!single);
    bidirectionalTestButton_->setVisible(!single);
    stressResultLabel_->setVisible(!single);
    stressTestButton_->setVisible(!single);

    if (single) {
        channels_[1].headerLayout->removeWidget(channels_[1].actionsContainer);
        if (channels_[1].settingsRowLayout->indexOf(channels_[1].actionsContainer) < 0) {
            channels_[1].settingsRowLayout->addWidget(channels_[1].actionsContainer);
        }
        channels_[1].headerWidget->hide();
        channels_[1].refreshPortsButton->setText(QStringLiteral("刷新串口"));
        channels_[1].portCombo->setMaximumWidth(320);
        modeDescriptionLabel_->setText(QStringLiteral("连接一个串口，查看 TX / RX 并发送数据"));
    } else {
        for (int index = 0; index < 2; ++index) {
            channels_[index].settingsRowLayout->removeWidget(channels_[index].actionsContainer);
            if (channels_[index].headerLayout->indexOf(channels_[index].actionsContainer) < 0) {
                channels_[index].headerLayout->addWidget(channels_[index].actionsContainer);
            }
            channels_[index].headerWidget->show();
            channels_[index].badgeLabel->show();
            channels_[index].titleLabel->setText(channelName(index));
            channels_[index].refreshPortsButton->setText(QStringLiteral("刷新"));
            channels_[index].portCombo->setMaximumWidth(250);
        }
        modeDescriptionLabel_->setText(
            QStringLiteral("配置端点 A / B；独立收发、双向验证和可靠性压测按需使用"));
    }
    updateCounter(0);
    updateCounter(1);
}

QWidget *SerialWorkspace::buildChannelPanel(int index)
{
    auto &channel = channels_[index];
    channel.group = new QGroupBox(this);
    channel.group->setObjectName(QStringLiteral("endpointCard"));
    auto *layout = new QVBoxLayout(channel.group);
    layout->setContentsMargins(14, 10, 14, 12);
    layout->setSpacing(8);

    channel.headerWidget = new QWidget(channel.group);
    channel.headerLayout = new QHBoxLayout(channel.headerWidget);
    channel.headerLayout->setContentsMargins(0, 0, 0, 0);
    channel.headerLayout->setSpacing(6);
    channel.badgeLabel = new QLabel(
        index == 0 ? QStringLiteral("A") : QStringLiteral("B"), channel.headerWidget);
    channel.badgeLabel->setAlignment(Qt::AlignCenter);
    channel.badgeLabel->setFixedSize(22, 22);
    channel.badgeLabel->setObjectName(index == 0 ? QStringLiteral("endpointBadgeA")
                                                 : QStringLiteral("endpointBadgeB"));
    channel.titleLabel = new QLabel(channelName(index), channel.headerWidget);
    channel.titleLabel->setObjectName(QStringLiteral("endpointTitle"));
    channel.headerLayout->addWidget(channel.badgeLabel);
    channel.headerLayout->addWidget(channel.titleLabel);
    channel.headerLayout->addStretch();

    channel.actionsContainer = new QWidget(channel.group);
    auto *actionsLayout = new QHBoxLayout(channel.actionsContainer);
    actionsLayout->setContentsMargins(0, 0, 0, 0);
    actionsLayout->setSpacing(6);
    channel.refreshPortsButton =
        new QPushButton(QStringLiteral("刷新串口"), channel.actionsContainer);
    channel.refreshPortsButton->setObjectName(QStringLiteral("ghostButton"));
    channel.refreshPortsButton->setToolTip(QStringLiteral("重新扫描本地与设备串口列表"));
    channel.advancedSettingsButton =
        new QPushButton(QStringLiteral("更多参数"), channel.actionsContainer);
    channel.advancedSettingsButton->setObjectName(QStringLiteral("ghostButton"));
    channel.advancedSettingsButton->setCheckable(true);
    channel.statusLabel = new QLabel(QStringLiteral("● 未连接"), channel.actionsContainer);
    channel.statusLabel->setObjectName(QStringLiteral("connectionStatusIdle"));
    channel.statusLabel->setAlignment(Qt::AlignCenter);
    channel.connectButton = new QPushButton(QStringLiteral("连接"), channel.actionsContainer);
    channel.connectButton->setMinimumWidth(78);
    actionsLayout->addWidget(channel.refreshPortsButton);
    actionsLayout->addWidget(channel.advancedSettingsButton);
    actionsLayout->addWidget(channel.statusLabel);
    actionsLayout->addWidget(channel.connectButton);

    channel.headerLayout->addWidget(channel.actionsContainer);
    layout->addWidget(channel.headerWidget);

    auto *connectionRow = new QFrame(channel.group);
    connectionRow->setObjectName(QStringLiteral("connectionRow"));
    channel.settingsRowLayout = new QHBoxLayout(connectionRow);
    channel.settingsRowLayout->setContentsMargins(0, 0, 0, 8);
    channel.settingsRowLayout->setSpacing(6);
    channel.sourceCombo = new QComboBox(connectionRow);
    channel.sourceCombo->addItem(QStringLiteral("Windows 本地串口"), false);
    channel.sourceCombo->addItem(QStringLiteral("设备串口"), true);
    channel.sourceCombo->setMinimumWidth(148);
    channel.sourceCombo->setMaximumWidth(176);

    channel.portCombo = new QComboBox(connectionRow);
    channel.portCombo->setEditable(true);
    channel.portCombo->setMinimumContentsLength(12);
    channel.portCombo->setMaximumWidth(320);

    channel.baudCombo = new QComboBox(connectionRow);
    channel.baudCombo->setEditable(true);
    channel.baudCombo->addItems({QStringLiteral("9600"), QStringLiteral("19200"),
                                 QStringLiteral("38400"), QStringLiteral("57600"),
                                 QStringLiteral("115200"), QStringLiteral("921600")});
    channel.baudCombo->setMinimumWidth(106);
    channel.baudCombo->setMaximumWidth(128);
    channel.baudCombo->setCurrentText(QStringLiteral("115200"));
    channel.baudCombo->setToolTip(
        QStringLiteral("物理 UART 使用该波特率；ttyUSB/ttyACM 是 USB 虚拟串口，驱动可能忽略该数值，"
                       "它不代表 USB 链路速率。"));
    channel.baudCombo->lineEdit()->setCursorPosition(0);

    const auto fieldLabel = [&channel](const QString &text) {
        auto *label = new QLabel(text, channel.group);
        label->setObjectName(QStringLiteral("fieldLabel"));
        return label;
    };
    channel.settingsRowLayout->addWidget(fieldLabel(QStringLiteral("位置")));
    channel.settingsRowLayout->addWidget(channel.sourceCombo);
    channel.settingsRowLayout->addWidget(fieldLabel(QStringLiteral("串口")));
    channel.settingsRowLayout->addWidget(channel.portCombo, 1);
    channel.settingsRowLayout->addWidget(fieldLabel(QStringLiteral("波特率")));
    channel.settingsRowLayout->addWidget(channel.baudCombo);
    channel.settingsRowLayout->addStretch();
    layout->addWidget(connectionRow);

    channel.dataBitsCombo = new QComboBox(channel.group);
    channel.dataBitsCombo->addItems({QStringLiteral("5"), QStringLiteral("6"), QStringLiteral("7"), QStringLiteral("8")});
    channel.dataBitsCombo->setCurrentText(QStringLiteral("8"));
    channel.parityCombo = new QComboBox(channel.group);
    channel.parityCombo->addItems({QStringLiteral("无校验"), QStringLiteral("奇校验"), QStringLiteral("偶校验")});
    channel.stopBitsCombo = new QComboBox(channel.group);
    channel.stopBitsCombo->addItems({QStringLiteral("1 停止位"), QStringLiteral("2 停止位")});
    channel.flowControlCombo = new QComboBox(channel.group);
    channel.flowControlCombo->addItems({QStringLiteral("无流控"), QStringLiteral("硬件流控"), QStringLiteral("软件流控")});

    channel.advancedSettingsArea = new QWidget(channel.group);
    auto *advancedSettings = new QHBoxLayout(channel.advancedSettingsArea);
    advancedSettings->setContentsMargins(0, 0, 0, 2);
    advancedSettings->setSpacing(6);
    channel.dataBitsCombo->setMinimumWidth(68);
    channel.parityCombo->setMinimumWidth(92);
    channel.stopBitsCombo->setMinimumWidth(92);
    channel.flowControlCombo->setMinimumWidth(92);
    advancedSettings->addWidget(fieldLabel(QStringLiteral("数据位")));
    advancedSettings->addWidget(channel.dataBitsCombo);
    advancedSettings->addWidget(fieldLabel(QStringLiteral("校验")));
    advancedSettings->addWidget(channel.parityCombo);
    advancedSettings->addWidget(fieldLabel(QStringLiteral("停止位")));
    advancedSettings->addWidget(channel.stopBitsCombo);
    advancedSettings->addWidget(fieldLabel(QStringLiteral("流控")));
    advancedSettings->addWidget(channel.flowControlCombo);
    advancedSettings->addStretch();
    channel.advancedSettingsArea->hide();
    layout->addWidget(channel.advancedSettingsArea);

    auto *terminalFrame = new QFrame(channel.group);
    terminalFrame->setObjectName(QStringLiteral("terminalFrame"));
    auto *terminalLayout = new QVBoxLayout(terminalFrame);
    terminalLayout->setContentsMargins(0, 0, 0, 0);
    terminalLayout->setSpacing(0);

    auto *terminalHeader = new QFrame(terminalFrame);
    terminalHeader->setObjectName(QStringLiteral("terminalHeader"));
    auto *receiveHeader = new QHBoxLayout(terminalHeader);
    receiveHeader->setContentsMargins(12, 5, 10, 5);
    receiveHeader->setSpacing(8);

    auto *receiveTitle = new QLabel(QStringLiteral("收发记录"), terminalHeader);
    receiveTitle->setObjectName(QStringLiteral("sectionLabel"));

    channel.receiveHexCheck = new QCheckBox(QStringLiteral("HEX 显示"), terminalHeader);
    channel.timestampCheck = new QCheckBox(QStringLiteral("时间戳"), terminalHeader);
    channel.autoScrollCheck = new QCheckBox(QStringLiteral("自动滚动"), terminalHeader);
    channel.autoScrollCheck->setChecked(true);
    channel.autoScrollCheck->setToolTip(
        QStringLiteral("收到新数据时自动滚动到底部；取消勾选后可固定视图查看或复制历史日志"));

    channel.counterLabel = new QLabel(QStringLiteral("RX: 0 B  |  TX: 0 B"), terminalHeader);
    channel.counterLabel->setObjectName(QStringLiteral("mutedLabel"));
    channel.counterLabel->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);

    channel.resetCounterButton = new QPushButton(QStringLiteral("计数清零"), terminalHeader);
    channel.resetCounterButton->setObjectName(QStringLiteral("clearButton"));
    channel.resetCounterButton->setToolTip(QStringLiteral("将当前端点的 RX / TX 字节计数归零"));

    channel.saveLogButton = new QPushButton(QStringLiteral("保存日志…"), terminalHeader);
    channel.saveLogButton->setObjectName(QStringLiteral("clearButton"));
    channel.saveLogButton->setToolTip(QStringLiteral("将当前收发记录导出为文本或日志文件"));

    channel.clearReceiveButton = new QPushButton(QStringLiteral("清空"), terminalHeader);
    channel.clearReceiveButton->setObjectName(QStringLiteral("clearButton"));
    channel.clearReceiveButton->setToolTip(QStringLiteral("清空当前收发记录显示区"));

    receiveHeader->addWidget(receiveTitle);
    receiveHeader->addWidget(channel.receiveHexCheck);
    receiveHeader->addWidget(channel.timestampCheck);
    receiveHeader->addWidget(channel.autoScrollCheck);
    receiveHeader->addStretch();
    receiveHeader->addWidget(channel.counterLabel);
    receiveHeader->addWidget(channel.resetCounterButton);
    receiveHeader->addWidget(channel.saveLogButton);
    receiveHeader->addWidget(channel.clearReceiveButton);
    terminalLayout->addWidget(terminalHeader);

    channel.receiveEdit = new QPlainTextEdit(terminalFrame);
    channel.receiveEdit->setObjectName(QStringLiteral("receiveConsole"));
    channel.receiveEdit->setReadOnly(true);
    channel.receiveEdit->setMaximumBlockCount(5000);
    channel.receiveEdit->setPlaceholderText(
        QStringLiteral("等待数据；发送成功显示 TX，接收数据显示 RX"));
    terminalLayout->addWidget(channel.receiveEdit, 1);
    layout->addWidget(terminalFrame, 1);

    channel.manualSendArea = new QWidget(channel.group);
    auto *sendAreaLayout = new QVBoxLayout(channel.manualSendArea);
    sendAreaLayout->setContentsMargins(0, 0, 0, 0);
    sendAreaLayout->setSpacing(6);

    channel.atCommandArea = new QFrame(channel.manualSendArea);
    channel.atCommandArea->setObjectName(QStringLiteral("atCommandBar"));
    auto *atCommandLayout = new QHBoxLayout(channel.atCommandArea);
    atCommandLayout->setContentsMargins(8, 5, 8, 5);
    atCommandLayout->setSpacing(6);
    auto *atCommandLabel = new QLabel(QStringLiteral("快捷指令"), channel.atCommandArea);
    atCommandLabel->setObjectName(QStringLiteral("shortcutTag"));
    channel.atCommandCombo = new QComboBox(channel.atCommandArea);
    channel.atCommandCombo->setMinimumContentsLength(18);
    channel.atCommandCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    auto *manageAtCommands = new QPushButton(QStringLiteral("管理指令…"), channel.atCommandArea);
    manageAtCommands->setObjectName(QStringLiteral("clearButton"));
    channel.fillAtCommandButton = new QPushButton(QStringLiteral("填入输入"), channel.atCommandArea);
    channel.fillAtCommandButton->setObjectName(QStringLiteral("clearButton"));
    channel.fillAtCommandButton->setToolTip(
        QStringLiteral("将所选快捷指令填入下方发送输入框，便于修改参数后再发送"));
    channel.sendAtCommandButton = new QPushButton(QStringLiteral("发送指令"), channel.atCommandArea);
    channel.sendAtCommandButton->setObjectName(QStringLiteral("verifyButton"));
    atCommandLayout->addWidget(atCommandLabel);
    atCommandLayout->addWidget(channel.atCommandCombo, 1);
    atCommandLayout->addWidget(manageAtCommands);
    atCommandLayout->addWidget(channel.fillAtCommandButton);
    atCommandLayout->addWidget(channel.sendAtCommandButton);
    sendAreaLayout->addWidget(channel.atCommandArea);

    auto *composerFrame = new QFrame(channel.manualSendArea);
    composerFrame->setObjectName(QStringLiteral("composerFrame"));
    auto *composerLayout = new QVBoxLayout(composerFrame);
    composerLayout->setContentsMargins(0, 0, 0, 0);
    composerLayout->setSpacing(0);

    channel.sendEdit = new QPlainTextEdit(composerFrame);
    channel.sendEdit->setObjectName(QStringLiteral("sendEditor"));
    channel.sendEdit->setFixedHeight(52);
    channel.sendEdit->setPlaceholderText(QStringLiteral("输入要发送的数据"));
    channel.sendEdit->installEventFilter(this);
    composerLayout->addWidget(channel.sendEdit);

    auto *composerFooter = new QFrame(composerFrame);
    composerFooter->setObjectName(QStringLiteral("composerFooter"));
    auto *sendTools = new QHBoxLayout(composerFooter);
    sendTools->setContentsMargins(10, 5, 10, 5);
    sendTools->setSpacing(8);
    channel.sendHexCheck = new QCheckBox(QStringLiteral("HEX 发送"), composerFooter);
    channel.lineEndingCombo = new QComboBox(composerFooter);
    channel.lineEndingCombo->addItems({QStringLiteral("无"), QStringLiteral("LF"),
                                       QStringLiteral("CR"), QStringLiteral("CRLF")});
    if (index == 1)
        channel.lineEndingCombo->setCurrentIndex(
            storedLineEndingIndex(atCommandLibrary_.lineEnding()));
    channel.lineEndingCombo->setToolTip(
        QStringLiteral("手动发送和文本快捷指令共用此行尾；切换端口不会改变选择。"));
    channel.lineEndingCombo->setMinimumWidth(84);

    auto *endingLabel = new QLabel(QStringLiteral("行尾"), composerFooter);
    endingLabel->setObjectName(QStringLiteral("fieldLabel"));

    channel.sendShortcutHint = new QLabel(QStringLiteral("Enter 发送 · Shift+Enter 换行"),
                                          composerFooter);
    channel.sendShortcutHint->setObjectName(QStringLiteral("fieldLabel"));

    channel.toggleAtBarButton =
        new QPushButton(QStringLiteral("收起快捷指令"), composerFooter);
    channel.toggleAtBarButton->setObjectName(QStringLiteral("ghostButton"));
    channel.toggleAtBarButton->setCheckable(true);
    channel.toggleAtBarButton->setChecked(true);
    channel.toggleAtBarButton->setToolTip(QStringLiteral("显示或隐藏上方快捷指令栏"));

    channel.clearSendButton = new QPushButton(QStringLiteral("清空"), composerFooter);
    channel.clearSendButton->setObjectName(QStringLiteral("clearButton"));
    channel.clearSendButton->setToolTip(QStringLiteral("清空上方发送输入框内容"));

    channel.sendButton = new QPushButton(QStringLiteral("发送"), composerFooter);
    channel.sendButton->setObjectName(QStringLiteral("primaryButton"));
    channel.sendButton->setEnabled(false);
    channel.sendButton->setMinimumWidth(72);

    channel.autoSendButton = new QPushButton(QStringLiteral("定时 / 次数…"), composerFooter);
    channel.autoSendButton->setObjectName(QStringLiteral("clearButton"));
    channel.autoSendButton->setEnabled(false);

    sendTools->addWidget(channel.sendHexCheck);
    sendTools->addWidget(endingLabel);
    sendTools->addWidget(channel.lineEndingCombo);
    sendTools->addWidget(channel.sendShortcutHint);
    sendTools->addStretch();
    sendTools->addWidget(channel.toggleAtBarButton);
    sendTools->addWidget(channel.clearSendButton);
    sendTools->addWidget(channel.autoSendButton);
    sendTools->addWidget(channel.sendButton);
    composerLayout->addWidget(composerFooter);

    sendAreaLayout->addWidget(composerFrame);
    layout->addWidget(channel.manualSendArea);

    connect(channel.sourceCombo, &QComboBox::currentIndexChanged, this, [this, index] {
        applyPorts(index);
        updateRouteSummary();
        updateGlobalControls();
        savePreferences();
    });
    connect(channel.portCombo, &QComboBox::currentTextChanged, this, [this, index] {
        updateRouteSummary();
        updateGlobalControls();
        savePreferences();
    });
    connect(channel.baudCombo, &QComboBox::currentTextChanged, this, [this] {
        updateGlobalControls();
        savePreferences();
    });
    connect(channel.dataBitsCombo, &QComboBox::currentIndexChanged, this, [this] {
        savePreferences();
    });
    connect(channel.parityCombo, &QComboBox::currentIndexChanged, this, [this] {
        savePreferences();
    });
    connect(channel.stopBitsCombo, &QComboBox::currentIndexChanged, this, [this] {
        savePreferences();
    });
    connect(channel.flowControlCombo, &QComboBox::currentIndexChanged, this, [this] {
        savePreferences();
    });
    connect(channel.refreshPortsButton, &QPushButton::clicked, this, [this] {
        refreshTargets();
    });
    connect(
        channel.advancedSettingsButton, &QPushButton::toggled, this, [this, index](bool visible) {
            channels_[index].advancedSettingsArea->setVisible(visible);
            channels_[index].advancedSettingsButton->setText(visible ? QStringLiteral("收起参数")
                                                                     : QStringLiteral("更多参数"));
        });
    connect(channel.connectButton, &QPushButton::clicked, this, [this, index] {
        if (channelActive(index)) {
            disconnectChannel(index);
        } else {
            connectChannel(index);
        }
    });
    connect(channel.receiveHexCheck, &QCheckBox::toggled, this, [this, index] {
        renderReceiveLog(index);
        savePreferences();
    });
    connect(channel.timestampCheck, &QCheckBox::toggled, this, [this, index] {
        renderReceiveLog(index);
        savePreferences();
    });
    connect(channel.autoScrollCheck, &QCheckBox::toggled, this, [this, index](bool checked) {
        if (checked && channels_[index].receiveEdit->verticalScrollBar()) {
            channels_[index].receiveEdit->verticalScrollBar()->setValue(
                channels_[index].receiveEdit->verticalScrollBar()->maximum());
        }
        savePreferences();
    });
    connect(channel.resetCounterButton, &QPushButton::clicked, this, [this, index] {
        resetChannelCounters(index);
    });
    connect(channel.saveLogButton, &QPushButton::clicked, this, [this, index] {
        saveChannelLog(index);
    });
    connect(channel.clearReceiveButton, &QPushButton::clicked, this, [this, index] {
        channels_[index].logBuffer.clear();
        channels_[index].receiveEdit->clear();
    });
    connect(channel.toggleAtBarButton, &QPushButton::toggled, this, [this, index](bool visible) {
        channels_[index].toggleAtBarButton->setText(visible ? QStringLiteral("收起快捷指令")
                                                            : QStringLiteral("展开快捷指令"));
        if (workspaceState_.mode() == WorkspaceMode::Single && index == 1) {
            channels_[index].atCommandArea->setVisible(visible);
        }
        savePreferences();
    });
    connect(channel.sendButton, &QPushButton::clicked, this, [this, index] { sendManual(index); });
    connect(channel.fillAtCommandButton,
            &QPushButton::clicked,
            this,
            [this, index] { fillAtCommandToInput(index); });
    connect(channel.sendAtCommandButton,
            &QPushButton::clicked,
            this,
            [this, index] { sendAtCommand(index); });
    connect(manageAtCommands,
            &QPushButton::clicked,
            this,
            &SerialWorkspace::showAtCommandManager);
    connect(channel.sendHexCheck, &QCheckBox::toggled, this, [this] { savePreferences(); });
    connect(channel.lineEndingCombo,
            &QComboBox::currentIndexChanged,
            this,
            [this, index](int selected) {
                if (index == 1) {
                    atCommandLibrary_.setLineEnding(storedLineEndingFromIndex(selected));
                    saveAtCommands();
                }
                savePreferences();
            });
    connect(channel.autoSendButton, &QPushButton::clicked, this, [this, index] {
        toggleAutoSend(index);
    });
    connect(channel.clearSendButton,
            &QPushButton::clicked,
            channel.sendEdit,
            &QPlainTextEdit::clear);

    setChannelStatus(index, QStringLiteral("未连接"));
    return channel.group;
}

bool SerialWorkspace::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        const bool enter = keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter;
        if (enter && !(keyEvent->modifiers() & Qt::ShiftModifier)) {
            for (int index = 0; index < 2; ++index) {
                if (watched == channels_[index].sendEdit) {
                    sendManual(index);
                    return true;
                }
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

bool SerialWorkspace::channelUsesAgent(int index) const
{
    return channels_[index].sourceCombo->currentData().toBool();
}

bool SerialWorkspace::selectedAgentIsOnline() const
{
    return currentAdbDevice_.state == "device" && !currentAdbDevice_.serial.empty() &&
           agentControlPort_ != 0 && agentAdbSerial_ == currentAdbDevice_.serial;
}

void SerialWorkspace::refreshTargets()
{
    if (shuttingDown_ || channelActive(0) || channelActive(1) || scanning_) {
        return;
    }

    localPorts_ = testtools::SerialTransportService::localPorts();
    applyPorts(0);
    applyPorts(1);
    refreshAgentPorts();
}

void SerialWorkspace::refreshAgentPorts()
{
    agentPorts_.clear();
    applyPorts(0);
    applyPorts(1);

    if (!selectedAgentIsOnline()) {
        scanning_ = false;
        emit activityChanged(currentAdbDevice_.serial.empty()
                                 ? QStringLiteral("未选择设备，设备串口列表为空")
                                 : QStringLiteral("请先在设备信息中连接当前设备"));
        updateGlobalControls();
        return;
    }

    scanning_ = true;
    emit activityChanged(QStringLiteral("正在读取 %1 的设备串口")
                             .arg(fromUtf8(currentAdbDevice_.serial)));
    updateGlobalControls();
    const std::uint16_t controlPort = agentControlPort_;
    QPointer<SerialWorkspace> guard(this);
    scanTasks_.start([guard, controlPort] {
        std::string error;
        auto ports = testtools::SerialTransportService::agentPorts(controlPort, error);
        QMetaObject::invokeMethod(
            guard,
            [guard, ports = std::move(ports), error = std::move(error)] {
                if (!guard || guard->shuttingDown_) {
                    return;
                }
                guard->scanning_ = false;
                guard->agentPorts_ = std::move(ports);
                guard->applyPorts(0);
                guard->applyPorts(1);
                if (!error.empty()) {
                    emit guard->activityChanged(fromUtf8(error));
                } else {
                    emit guard->activityChanged(QStringLiteral("已发现 %1 个设备串口")
                                                    .arg(guard->agentPorts_.size()));
                }
                guard->updateGlobalControls();
            },
            Qt::QueuedConnection);
    });
}

void SerialWorkspace::setAdbDevice(const AdbDevice& device)
{
    if (hasActiveConnections()) {
        return;
    }
    currentAdbDevice_ = device;
    refreshTargets();
    updateRouteSummary();
}

void SerialWorkspace::setAgentConnection(const std::string& adbSerial,
                                         std::uint16_t localControlPort)
{
    if (hasActiveConnections()) {
        return;
    }
    agentAdbSerial_ = adbSerial;
    agentControlPort_ = localControlPort;
    refreshTargets();
    updateRouteSummary();
}

bool SerialWorkspace::hasActiveConnections() const
{
    return channelActive(0) || channelActive(1);
}

void SerialWorkspace::applyPorts(int index)
{
    auto& channel = channels_[index];
    if (!channel.portCombo || channelActive(index)) {
        return;
    }
    const QString previousData = channel.portCombo->currentData().toString();
    const QString previousText = channel.portCombo->currentText();
    const QString preferredEndpoint =
        fromUtf8(preferences_.channels[static_cast<std::size_t>(index)].endpoint);
    const QSignalBlocker blocker(channel.portCombo);
    channel.portCombo->clear();

    if (channelUsesAgent(index)) {
        for (const auto& port : agentPorts_) {
            QString detail = fromUtf8(port.interfaceName);
            if (!port.occupiedBy.empty()) {
                detail = QStringLiteral("占用中：%1").arg(fromUtf8(port.occupiedBy));
            } else if (!port.readable || !port.writable) {
                detail = QStringLiteral("权限不足");
            }
            const QString endpoint = fromUtf8(port.endpoint);
            channel.portCombo->addItem(detail.isEmpty()
                                           ? endpoint : endpoint + QStringLiteral(" — ") + detail,
                                       endpoint);
        }
        channel.portCombo->setToolTip(QStringLiteral("当前连接设备提供的串口节点"));
    } else {
        for (const auto &port : localPorts_) {
            const QString name = fromUtf8(port.name);
            const QString description = fromUtf8(port.description);
            channel.portCombo->addItem(description.isEmpty() ? name : name + QStringLiteral(" — ") + description,
                                       name);
        }
        channel.portCombo->setToolTip(QStringLiteral("Windows COM 口，例如 COM3"));
    }

    int restoreIndex = channel.portCombo->findData(previousData);
    if (restoreIndex < 0) {
        restoreIndex = channel.portCombo->findData(previousText);
    }
    if (restoreIndex < 0 && !preferredEndpoint.isEmpty()) {
        restoreIndex = channel.portCombo->findData(preferredEndpoint);
    }
    if (restoreIndex >= 0) {
        channel.portCombo->setCurrentIndex(restoreIndex);
    } else if (!preferredEndpoint.isEmpty() && channel.portCombo->count() == 0) {
        channel.portCombo->setEditText(preferredEndpoint);
    }
    if (channel.portCombo->lineEdit()) {
        channel.portCombo->lineEdit()->setCursorPosition(0);
    }
    updateRouteSummary();
}

void SerialWorkspace::applyPreferencesToUi()
{
    applyingPreferences_ = true;
    for (int index = 0; index < 2; ++index) {
        auto& channel = channels_[static_cast<std::size_t>(index)];
        const auto& pref = preferences_.channels[static_cast<std::size_t>(index)];
        const QSignalBlocker b1(channel.sourceCombo);
        const QSignalBlocker b2(channel.portCombo);
        const QSignalBlocker b3(channel.baudCombo);
        const QSignalBlocker b4(channel.dataBitsCombo);
        const QSignalBlocker b5(channel.parityCombo);
        const QSignalBlocker b6(channel.stopBitsCombo);
        const QSignalBlocker b7(channel.flowControlCombo);
        const QSignalBlocker b8(channel.lineEndingCombo);
        const QSignalBlocker b9(channel.receiveHexCheck);
        const QSignalBlocker b10(channel.timestampCheck);
        const QSignalBlocker b11(channel.autoScrollCheck);
        const QSignalBlocker b12(channel.sendHexCheck);

        channel.sourceCombo->setCurrentIndex(pref.useAgent ? 1 : 0);
        channel.baudCombo->setCurrentText(QString::number(pref.baudRate));
        if (channel.baudCombo->lineEdit()) {
            channel.baudCombo->lineEdit()->setCursorPosition(0);
        }
        channel.dataBitsCombo->setCurrentText(QString::number(pref.dataBits));
        channel.parityCombo->setCurrentIndex(std::clamp(pref.parity, 0, 2));
        channel.stopBitsCombo->setCurrentIndex(std::clamp(pref.stopBits, 0, 1));
        channel.flowControlCombo->setCurrentIndex(std::clamp(pref.flowControl, 0, 2));
        channel.lineEndingCombo->setCurrentIndex(std::clamp(pref.lineEnding, 0, 3));
        channel.receiveHexCheck->setChecked(pref.receiveHex);
        channel.timestampCheck->setChecked(pref.timestamp);
        channel.autoScrollCheck->setChecked(pref.autoScroll);
        channel.sendHexCheck->setChecked(pref.sendHex);
        if (!pref.endpoint.empty()) {
            channel.portCombo->setEditText(fromUtf8(pref.endpoint));
            if (channel.portCombo->lineEdit()) {
                channel.portCombo->lineEdit()->setCursorPosition(0);
            }
        }
    }
    if (channels_[1].toggleAtBarButton) {
        const QSignalBlocker bToggle(channels_[1].toggleAtBarButton);
        channels_[1].toggleAtBarButton->setChecked(preferences_.shortcutBarVisible);
        channels_[1].toggleAtBarButton->setText(preferences_.shortcutBarVisible
                                                    ? QStringLiteral("收起快捷指令")
                                                    : QStringLiteral("展开快捷指令"));
    }
    applyingPreferences_ = false;
    applyWorkspaceMode();
}

void SerialWorkspace::savePreferences()
{
    if (applyingPreferences_ || shuttingDown_ || preferencesStorePath_.empty()) {
        return;
    }
    for (int index = 0; index < 2; ++index) {
        const auto& channel = channels_[static_cast<std::size_t>(index)];
        auto& pref = preferences_.channels[static_cast<std::size_t>(index)];
        pref.useAgent = channelUsesAgent(index);
        const QString portData = channel.portCombo->currentData().toString();
        const QString endpointText =
            portData.isEmpty() ? channel.portCombo->currentText().trimmed() : portData;
        if (!endpointText.isEmpty()) {
            pref.endpoint = toUtf8(endpointText);
        }
        const int baud = channel.baudCombo->currentText().toInt();
        if (baud > 0) {
            pref.baudRate = baud;
        }
        pref.dataBits = channel.dataBitsCombo->currentText().toInt();
        pref.parity = channel.parityCombo->currentIndex();
        pref.stopBits = channel.stopBitsCombo->currentIndex();
        pref.flowControl = channel.flowControlCombo->currentIndex();
        pref.lineEnding = channel.lineEndingCombo->currentIndex();
        pref.receiveHex = channel.receiveHexCheck->isChecked();
        pref.timestamp = channel.timestampCheck->isChecked();
        pref.autoScroll = channel.autoScrollCheck->isChecked();
        pref.sendHex = channel.sendHexCheck->isChecked();
    }
    if (channels_[1].toggleAtBarButton) {
        preferences_.shortcutBarVisible = channels_[1].toggleAtBarButton->isChecked();
    }
    std::string ignoredError;
    testtools::SerialPreferencesStore::save(preferencesStorePath_, preferences_, ignoredError);
}

SerialConfig SerialWorkspace::channelConfig(int index) const
{
    const auto &channel = channels_[index];
    SerialConfig config;
    const QString portData = channel.portCombo->currentData().toString();
    config.endpoint = toUtf8(portData.isEmpty() ? channel.portCombo->currentText() : portData);
    config.adbSerial = currentAdbDevice_.serial;
    config.allowAdbRoot = false;
    config.baudRate = channel.baudCombo->currentText().toInt();
    config.dataBits = channel.dataBitsCombo->currentText().toInt();
    config.parity = channel.parityCombo->currentIndex() == 1 ? Parity::Odd
                    : channel.parityCombo->currentIndex() == 2 ? Parity::Even : Parity::None;
    config.stopBits = channel.stopBitsCombo->currentIndex() == 1 ? StopBits::Two : StopBits::One;
    config.flowControl = channel.flowControlCombo->currentIndex() == 1 ? FlowControl::Hardware
                         : channel.flowControlCombo->currentIndex() == 2 ? FlowControl::Software : FlowControl::None;
    return config;
}

bool SerialWorkspace::validateChannel(int index, QString &error) const
{
    const auto config = channelConfig(index);
    if (config.endpoint.empty()) {
        error = channelDisplayName(index) + QStringLiteral("：尚未选择串口");
        return false;
    }
    if (config.baudRate <= 0) {
        error = channelDisplayName(index) + QStringLiteral("：波特率无效");
        return false;
    }
    if (channelUsesAgent(index) && !selectedAgentIsOnline()) {
        error = QStringLiteral("请先在设备信息中连接当前设备，再连接") + channelDisplayName(index);
        return false;
    }

    auto candidate = endpointSelection(index);
    candidate.active = true;
    const auto conflict = testtools::SerialWorkspaceState::endpointConflict(
        candidate, endpointSelection(1 - index));
    if (conflict == testtools::SerialEndpointConflict::AgentSingleSession) {
        error = QStringLiteral("当前设备连接一次只能使用一个设备串口");
        return false;
    }
    if (conflict == testtools::SerialEndpointConflict::SameResource) {
        error = QStringLiteral("端点 A 和 B 不能同时打开同一个串口");
        return false;
    }
    return true;
}

testtools::SerialEndpointSelection SerialWorkspace::endpointSelection(int index) const
{
    testtools::SerialEndpointSelection selection;
    selection.active = channelActive(index);
    selection.kind = channelUsesAgent(index) ? testtools::SerialEndpointKind::Agent
                                             : testtools::SerialEndpointKind::Local;
    const auto config = channelConfig(index);
    selection.endpoint = config.endpoint;
    selection.deviceSerial = config.adbSerial;
    return selection;
}

void SerialWorkspace::connectChannel(int index)
{
    auto& channel = channels_[index];
    if (channelActive(index)) {
        return;
    }

    QString error;
    if (!validateChannel(index, error)) {
        QMessageBox::warning(this, QStringLiteral("无法连接"), error);
        return;
    }

    const bool agent = channelUsesAgent(index);
    std::unique_ptr<ISerialTransport> transport;
    if (agent) {
        transport =
            testtools::SerialTransportService::createAgent(agentAdbSerial_, agentControlPort_);
    } else {
        transport = testtools::SerialTransportService::createLocal();
    }
    channel.connectButton->setText(QStringLiteral("取消 / 断开"));
    setChannelControlsEnabled(index, false);
    setChannelStatus(index, QStringLiteral("正在连接..."));
    updateGlobalControls();
    channel.session.open(std::move(transport), channelConfig(index));
    updateGlobalControls();
}

void SerialWorkspace::attachSessionCallbacks(int index)
{
    QPointer<SerialWorkspace> guard(this);
    channels_[index].session.setDataCallback(
        [guard, index](std::uint64_t generation, const ByteBuffer& data) {
            QMetaObject::invokeMethod(
                guard,
                [guard, index, generation, data] {
                    if (guard && !guard->shuttingDown_ &&
                        guard->channels_[index].session.generation() == generation) {
                        guard->appendReceived(index, data);
                    }
                },
                Qt::QueuedConnection);
        });
    channels_[index].session.setStatusCallback(
        [guard, index](std::uint64_t generation, TransportState state, const std::string& message) {
            QMetaObject::invokeMethod(
                guard,
                [guard, index, generation, state, message] {
                    if (!guard || guard->shuttingDown_) {
                        return;
                    }
                    if (guard->channels_[index].session.generation() != generation) {
                        return;
                    }
                    auto& channel = guard->channels_[index];
                    const QString status = fromUtf8(message);
                    emit guard->activityChanged(status);
                    const bool connected = state == TransportState::Connected;
                    const bool progress = state == TransportState::Progress;
                    const bool error = state == TransportState::Error;
                    guard->setChannelStatus(index, status, error);
                    if (connected) {
                        channel.connectButton->setText(QStringLiteral("断开"));
                        channel.sendButton->setEnabled(true);
                    } else if (!progress) {
                        if (!channel.session.isOpen()) {
                            guard->disconnectChannel(index, false);
                            guard->setChannelStatus(index, status, error);
                        }
                    }
                    guard->updateGlobalControls();
                },
                Qt::QueuedConnection);
        });
    channels_[index].session.setSendCallback([guard, index](std::uint64_t generation,
                                                            const ByteBuffer& data,
                                                            std::uint64_t tag,
                                                            bool succeeded,
                                                            bool scheduledSendStopped) {
        QMetaObject::invokeMethod(
            guard,
            [guard, index, generation, data, tag, succeeded, scheduledSendStopped] {
                if (!guard || guard->shuttingDown_) {
                    return;
                }
                auto& current = guard->channels_[index];
                if (current.session.generation() != generation) {
                    return;
                }
                if (!succeeded) {
                    guard->setChannelStatus(
                        index, QStringLiteral("发送失败，连接可能已经断开"), true);
                    guard->stopAutoSend(index, false);
                    emit guard->activityChanged(
                        scheduledSendStopped
                            ? QStringLiteral("%1 发送失败；计划发送已停止")
                                  .arg(guard->channelDisplayName(index))
                            : QStringLiteral("%1 发送失败").arg(guard->channelDisplayName(index)));
                    if (guard->stressTestRunning_) {
                        guard->stopStressTest(QStringLiteral("可靠性压测发送失败"));
                    }
                    guard->updateGlobalControls();
                    return;
                }
                if ((tag & 1u) != 0u) {
                    guard->appendTransmitted(index, data);
                }
                guard->updateCounter(index);
            },
            Qt::QueuedConnection);
    });
}

void SerialWorkspace::disconnectChannel(int index, bool showClosedStatus)
{
    auto& channel = channels_[index];
    const auto snapshot = channel.session.snapshot();
    if (channel.disconnectPending ||
        snapshot.state == testtools::SerialSessionState::Disconnecting) {
        return;
    }
    stopAutoSend(index);
    if (stressTestRunning_) {
        stopStressTest(QStringLiteral("串口连接已断开"));
    }
    channel.expectedFrame.clear();
    channel.detectionBuffer.clear();
    bidirectionalTestRunning_ = false;
    ++testGeneration_;

    if (!snapshot.active) {
        completeChannelDisconnect(index, channel.session.generation(), showClosedStatus);
        return;
    }

    channel.connectButton->setText(QStringLiteral("正在断开…"));
    channel.connectButton->setEnabled(false);
    channel.sendButton->setEnabled(false);
    channel.disconnectPending = true;
    setChannelControlsEnabled(index, false);
    if (showClosedStatus) {
        setChannelStatus(index, QStringLiteral("正在断开..."));
    }
    QPointer<SerialWorkspace> guard(this);
    channel.session.closeAsync([guard, index, showClosedStatus](std::uint64_t generation) {
        QMetaObject::invokeMethod(
            guard,
            [guard, index, generation, showClosedStatus] {
                if (guard && !guard->shuttingDown_) {
                    guard->completeChannelDisconnect(index, generation, showClosedStatus);
                }
            },
            Qt::QueuedConnection);
    });
    updateGlobalControls();
}

void SerialWorkspace::completeChannelDisconnect(int index,
                                                std::uint64_t generation,
                                                bool showClosedStatus)
{
    auto& channel = channels_[index];
    if (channel.session.generation() != generation || channel.session.isActive()) {
        return;
    }
    channel.disconnectPending = false;
    channel.connectButton->setText(QStringLiteral("连接"));
    channel.sendButton->setEnabled(false);
    setChannelControlsEnabled(index, true);
    if (showClosedStatus) {
        setChannelStatus(index, QStringLiteral("已断开"));
    }
    updateGlobalControls();
}

void SerialWorkspace::disconnectAll()
{
    disconnectChannel(0, channelActive(0));
    disconnectChannel(1, channelActive(1));
}

void SerialWorkspace::setChannelControlsEnabled(int index, bool enabled)
{
    auto& channel = channels_[index];
    channel.sourceCombo->setEnabled(enabled);
    channel.portCombo->setEnabled(enabled);
    channel.baudCombo->setEnabled(enabled);
    channel.dataBitsCombo->setEnabled(enabled);
    channel.parityCombo->setEnabled(enabled);
    channel.stopBitsCombo->setEnabled(enabled);
    channel.flowControlCombo->setEnabled(enabled);
}

void SerialWorkspace::updateGlobalControls()
{
    const auto firstSnapshot = channels_[0].session.snapshot();
    const auto secondSnapshot = channels_[1].session.snapshot();
    const bool firstActive = channelActive(0);
    const bool secondActive = channelActive(1);
    const bool anyActive = firstActive || secondActive;
    const bool bothOpen = firstSnapshot.open && secondSnapshot.open;
    const bool anyAutoSend =
        firstSnapshot.scheduledSend.running || secondSnapshot.scheduledSend.running;
    const auto resourceConflict = [this] {
        auto first = endpointSelection(0);
        auto second = endpointSelection(1);
        first.active = true;
        second.active = true;
        return !first.endpoint.empty() && !second.endpoint.empty() &&
               testtools::SerialWorkspaceState::endpointConflict(first, second) !=
                   testtools::SerialEndpointConflict::None;
    };
    const auto channelReady = [this, &resourceConflict](int index) {
        const auto& channel = channels_[index];
        const bool basicReady = !channel.portCombo->currentText().trimmed().isEmpty() &&
                                channel.baudCombo->currentText().toInt() > 0 &&
                                (!channelUsesAgent(index) || selectedAgentIsOnline());
        const int otherIndex = 1 - index;
        const bool conflictsWithOpenChannel = channelActive(otherIndex) && resourceConflict();
        return basicReady && !conflictsWithOpenChannel;
    };
    for (int index = 0; index < 2; ++index) {
        const auto session = channels_[index].session.snapshot();
        const bool open = session.open;
        const bool hasShortcut = channels_[index].atCommandCombo &&
                                 channels_[index].atCommandCombo->currentData().toInt() >= 0;
        if (channels_[index].refreshPortsButton) {
            channels_[index].refreshPortsButton->setEnabled(!scanning_ && !anyActive);
        }
        if (channels_[index].disconnectPending ||
            session.state == testtools::SerialSessionState::Disconnecting) {
            channels_[index].connectButton->setEnabled(false);
        } else if (channelActive(index)) {
            channels_[index].connectButton->setEnabled(true);
        } else {
            channels_[index].connectButton->setEnabled(!scanning_ && channelReady(index));
        }
        channels_[index].sendButton->setEnabled(open && !stressTestRunning_ &&
                                                !session.scheduledSend.running);
        if (channels_[index].fillAtCommandButton) {
            channels_[index].fillAtCommandButton->setEnabled(
                !stressTestRunning_ && !session.scheduledSend.running && hasShortcut);
        }
        channels_[index].sendAtCommandButton->setEnabled(
            open && !stressTestRunning_ && !session.scheduledSend.running && hasShortcut);
        channels_[index].autoSendButton->setEnabled(open && !stressTestRunning_);
    }
    const bool missingChannelsReady =
        (firstActive || channelReady(0)) && (secondActive || channelReady(1));
    const bool pairCompatible = firstActive || secondActive || !resourceConflict();
    const bool transitioning =
        firstSnapshot.state == testtools::SerialSessionState::Connecting ||
        secondSnapshot.state == testtools::SerialSessionState::Connecting ||
        firstSnapshot.state == testtools::SerialSessionState::Disconnecting ||
        secondSnapshot.state == testtools::SerialSessionState::Disconnecting ||
        channels_[0].disconnectPending || channels_[1].disconnectPending;
    connectBothButton_->setEnabled(!scanning_ && !bothOpen && !transitioning &&
                                   missingChannelsReady && pairCompatible);
    connectBothButton_->setText(bothOpen    ? QStringLiteral("两端已连接")
                                : anyActive ? QStringLiteral("连接另一端")
                                            : QStringLiteral("连接两端"));
    disconnectBothButton_->setEnabled(anyActive && !transitioning);
    bidirectionalTestButton_->setEnabled(bothOpen && !bidirectionalTestRunning_ &&
                                         !stressTestRunning_ && !anyAutoSend);
    bidirectionalTestButton_->setText(bidirectionalTestRunning_ ? QStringLiteral("正在验证…")
                                                                : QStringLiteral("双向收发验证"));
    stressTestButton_->setEnabled(stressTestRunning_ ||
                                  (bothOpen && !bidirectionalTestRunning_ && !anyAutoSend));
    stressTestButton_->setText(stressTestRunning_ ? QStringLiteral("停止可靠性压测")
                                                  : QStringLiteral("可靠性压测…"));
    emit busyChanged(anyActive);
}

void SerialWorkspace::setChannelStatus(int index, const QString& message, bool error)
{
    auto* label = channels_[index].statusLabel;
    QString display = message;
    QString role = QStringLiteral("connectionStatusIdle");
    if (message.startsWith(QStringLiteral("正在断开"))) {
        display = QStringLiteral("断开中");
        role = QStringLiteral("connectionStatusWorking");
    } else if (message.startsWith(QStringLiteral("正在"))) {
        display = QStringLiteral("连接中");
        role = QStringLiteral("connectionStatusWorking");
    } else if (!error && message.contains(QStringLiteral("已连接"))) {
        display = QStringLiteral("已连接");
        role = QStringLiteral("connectionStatusGood");
    } else if (error) {
        display = QStringLiteral("异常");
        role = QStringLiteral("connectionStatusError");
    }
    label->setText(QStringLiteral("● ") + display);
    label->setToolTip(message);
    setVisualRole(label, role);
}

void SerialWorkspace::updateRouteSummary()
{
    if (!routeLabel_ || !channels_[0].sourceCombo || !channels_[1].sourceCombo) {
        return;
    }
    const auto endpoint = [this](int index) {
        const auto& channel = channels_[index];
        const QString type =
            channelUsesAgent(index) ? QStringLiteral("设备") : QStringLiteral("Windows");
        const QString data = channel.portCombo->currentData().toString();
        const QString port = data.isEmpty() ? channel.portCombo->currentText() : data;
        return QStringLiteral("%1 · %2").arg(type,
                                             port.isEmpty() ? QStringLiteral("未选择") : port);
    };
    routeLabel_->setText(QStringLiteral("A  %1     ⇄     B  %2").arg(endpoint(0), endpoint(1)));
}

QString SerialWorkspace::channelDisplayName(int index) const
{
    if (workspaceState_.mode() == WorkspaceMode::Single && index == 1) {
        return QStringLiteral("当前串口");
    }
    return channelName(index);
}

bool SerialWorkspace::channelActive(int index) const
{
    return channels_[index].disconnectPending || channels_[index].session.isActive();
}

ByteBuffer SerialWorkspace::manualPayload(int index, bool& ok)
{
    auto& channel = channels_[index];
    ByteBuffer data;
    ok = true;
    const QString input = channel.sendEdit->toPlainText();
    if ((!channel.sendHexCheck->isChecked() && input.isEmpty())
        || (channel.sendHexCheck->isChecked() && input.trimmed().isEmpty())) {
        return {};
    }
    if (channel.sendHexCheck->isChecked()) {
        std::string error;
        if (!parseHex(toUtf8(input), data, error)) {
            QMessageBox::warning(this, QStringLiteral("HEX 输入错误"), fromUtf8(error));
            ok = false;
            return {};
        }
    } else {
        const QByteArray text = input.toUtf8();
        data.assign(reinterpret_cast<const std::uint8_t*>(text.constData()),
                    reinterpret_cast<const std::uint8_t*>(text.constData()) + text.size());
    }
    appendLineEnding(data, lineEndingFromIndex(channel.lineEndingCombo->currentIndex()));
    return data;
}

bool SerialWorkspace::sendBytes(int index, const ByteBuffer& data, bool echo)
{
    auto& channel = channels_[index];
    if (!channel.session.isOpen()) {
        setChannelStatus(index, QStringLiteral("尚未连接，无法发送"), true);
        return false;
    }
    if (!channel.session.send(data, echo ? 1u : 0u)) {
        setChannelStatus(index, QStringLiteral("发送队列已满或不可用"), true);
        return false;
    }
    return true;
}

void SerialWorkspace::sendManual(int index)
{
    bool ok = false;
    const auto data = manualPayload(index, ok);
    if (!ok) {
        return;
    }
    if (data.empty()) {
        QMessageBox::information(
            this, QStringLiteral("没有发送内容"), QStringLiteral("请输入要发送的数据。"));
        return;
    }
    if (!channels_[index].session.isOpen()) {
        QMessageBox::information(
            this, QStringLiteral("尚未连接"), QStringLiteral("请先连接当前串口。"));
        return;
    }
    sendBytes(index, data);
}

void SerialWorkspace::sendAtCommand(int index)
{
    const int commandIndex = channels_[index].atCommandCombo->currentData().toInt();
    const auto& commands = atCommandLibrary_.commands();
    if (commandIndex < 0 || commandIndex >= static_cast<int>(commands.size())) {
        QMessageBox::information(
            this, QStringLiteral("未选择指令"), QStringLiteral("请先选择要发送的快捷指令。"));
        return;
    }
    if (!channels_[index].session.isOpen()) {
        QMessageBox::information(
            this, QStringLiteral("尚未连接"), QStringLiteral("请先连接当前串口。"));
        return;
    }
    const auto& command = commands[static_cast<std::size_t>(commandIndex)];
    ByteBuffer data;
    if (command.hex) {
        std::string error;
        if (!parseHex(command.command, data, error)) {
            QMessageBox::warning(this, QStringLiteral("HEX 指令格式错误"), fromUtf8(error));
            return;
        }
    } else {
        data.assign(command.command.begin(), command.command.end());
        appendLineEnding(data, lineEndingFromIndex(channels_[index].lineEndingCombo->currentIndex()));
    }
    if (sendBytes(index, data)) {
        emit activityChanged(QStringLiteral("%1 已发送快捷指令：%2")
                                 .arg(channelDisplayName(index), fromUtf8(command.name)));
    }
}

void SerialWorkspace::fillAtCommandToInput(int index)
{
    const int commandIndex = channels_[index].atCommandCombo->currentData().toInt();
    const auto& commands = atCommandLibrary_.commands();
    if (commandIndex < 0 || commandIndex >= static_cast<int>(commands.size())) {
        QMessageBox::information(
            this, QStringLiteral("未选择指令"), QStringLiteral("请先选择要填入的快捷指令。"));
        return;
    }
    const auto& command = commands[static_cast<std::size_t>(commandIndex)];
    auto& channel = channels_[index];
    channel.sendHexCheck->setChecked(command.hex);
    channel.sendEdit->setPlainText(fromUtf8(command.command));
    channel.sendEdit->setFocus();
    channel.sendEdit->moveCursor(QTextCursor::End);
    emit activityChanged(
        QStringLiteral("已将快捷指令“%1”填入发送输入框").arg(fromUtf8(command.name)));
}

void SerialWorkspace::refreshAtCommandChoices()
{
    for (auto& channel : channels_) {
        if (!channel.atCommandCombo)
            continue;
        const int previousCommandIndex = channel.atCommandCombo->currentData().toInt();
        const QSignalBlocker blocker(channel.atCommandCombo);
        channel.atCommandCombo->clear();
        const auto& commands = atCommandLibrary_.commands();
        if (commands.empty()) {
            channel.atCommandCombo->addItem(QStringLiteral("尚未保存指令"), -1);
        } else {
            for (int index = 0; index < static_cast<int>(commands.size()); ++index) {
                const auto& command = commands[static_cast<std::size_t>(index)];
                QString preview = fromUtf8(command.command).simplified();
                if (preview.size() > 36) {
                    preview = preview.left(34) + QStringLiteral("…");
                }
                const QString label =
                    command.hex
                        ? QStringLiteral("%1 — [HEX] %2").arg(fromUtf8(command.name), preview)
                        : QStringLiteral("%1 — %2").arg(fromUtf8(command.name), preview);
                channel.atCommandCombo->addItem(label, index);
                channel.atCommandCombo->setItemData(
                    index,
                    fromUtf8(command.command),
                    Qt::ToolTipRole);
            }
            const int restored = channel.atCommandCombo->findData(previousCommandIndex);
            channel.atCommandCombo->setCurrentIndex(restored >= 0 ? restored : 0);
        }
    }
    updateGlobalControls();
}

bool SerialWorkspace::saveAtCommands()
{
    std::string error;
    if (!testtools::AtCommandStore::save(atCommandStorePath_, atCommandLibrary_, error)) {
        QMessageBox::warning(
            this,
            QStringLiteral("\u6307\u4ee4\u672a\u4fdd\u5b58"),
            QStringLiteral("%1\n\u672c\u6b21\u4fee\u6539\u4ecd\u53ef\u5728\u5f53\u524d\u4f1a\u8bdd\u4f7f\u7528\u3002").arg(fromUtf8(error)));
        return false;
    }
    return true;
}

void SerialWorkspace::showAtCommandManager()
{
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("atCommandDialog"));
    dialog.setWindowTitle(QStringLiteral("管理快捷指令"));
    dialog.resize(660, 420);
    auto *root = new QVBoxLayout(&dialog);
    root->setContentsMargins(16, 16, 16, 14);
    root->setSpacing(12);

    auto *body = new QHBoxLayout;
    body->setSpacing(14);
    auto *commandList = new QListWidget(&dialog);
    commandList->setMinimumWidth(190);
    body->addWidget(commandList, 1);

    auto *editor = new QWidget(&dialog);
    auto *editorLayout = new QVBoxLayout(editor);
    editorLayout->setContentsMargins(0, 0, 0, 0);
    editorLayout->setSpacing(10);
    auto *form = new QFormLayout;
    form->setHorizontalSpacing(12);
    auto *nameEdit = new QLineEdit(editor);
    nameEdit->setMaxLength(40);
    nameEdit->setPlaceholderText(QStringLiteral("例如：信号质量 / 握手帧"));
    auto *hexCheck = new QCheckBox(QStringLiteral("HEX 十六进制格式（按原始字节发送，不附加行尾）"), editor);
    auto *commandEdit = new QPlainTextEdit(editor);
    commandEdit->setFixedHeight(96);
    commandEdit->setPlaceholderText(QStringLiteral("例如：AT+CSQ 或 55 AA 01 0D 0A"));
    form->addRow(QStringLiteral("名称"), nameEdit);
    form->addRow(QStringLiteral("格式"), hexCheck);
    form->addRow(QStringLiteral("指令内容"), commandEdit);
    editorLayout->addLayout(form);
    auto *hint = new QLabel(
        QStringLiteral("文本指令发送时使用主页发送数据区的行尾设置；HEX 指令直接发送原始字节。"), editor);
    hint->setObjectName(QStringLiteral("fieldLabel"));
    hint->setWordWrap(true);
    editorLayout->addWidget(hint);
    editorLayout->addStretch();

    auto *actions = new QHBoxLayout;
    auto *newButton = new QPushButton(QStringLiteral("新建"), editor);
    auto *deleteButton = new QPushButton(QStringLiteral("删除"), editor);
    auto *saveButton = new QPushButton(QStringLiteral("保存指令"), editor);
    saveButton->setObjectName(QStringLiteral("primaryButton"));
    actions->addWidget(newButton);
    actions->addWidget(deleteButton);
    actions->addStretch();
    actions->addWidget(saveButton);
    editorLayout->addLayout(actions);
    body->addWidget(editor, 2);
    root->addLayout(body, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("完成"));
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    std::function<void(int)> populateForm = [&](int row) {
        const auto& commands = atCommandLibrary_.commands();
        if (row < 0 || row >= static_cast<int>(commands.size())) {
            nameEdit->clear();
            hexCheck->setChecked(false);
            commandEdit->clear();
            deleteButton->setEnabled(false);
            return;
        }
        const auto& command = commands[static_cast<std::size_t>(row)];
        nameEdit->setText(fromUtf8(command.name));
        hexCheck->setChecked(command.hex);
        commandEdit->setPlainText(fromUtf8(command.command));
        deleteButton->setEnabled(true);
    };
    std::function<void(int)> refreshList = [&](int selectedRow) {
        const QSignalBlocker blocker(commandList);
        commandList->clear();
        for (const auto& command : atCommandLibrary_.commands()) {
            const QString itemLabel =
                command.hex
                    ? QStringLiteral("[HEX] %1").arg(fromUtf8(command.name))
                    : fromUtf8(command.name);
            commandList->addItem(itemLabel);
        }
        if (selectedRow >= 0 && selectedRow < commandList->count())
            commandList->setCurrentRow(selectedRow);
        else
            commandList->setCurrentRow(commandList->count() > 0 ? 0 : -1);
        populateForm(commandList->currentRow());
    };

    connect(commandList,
            &QListWidget::currentRowChanged,
            &dialog,
            [&](int row) { populateForm(row); });
    connect(newButton, &QPushButton::clicked, &dialog, [&] {
        commandList->setCurrentRow(-1);
        populateForm(-1);
        nameEdit->setFocus();
    });
    connect(saveButton, &QPushButton::clicked, &dialog, [&] {
        testtools::AtCommand command;
        command.name = toUtf8(nameEdit->text().trimmed());
        command.command = toUtf8(commandEdit->toPlainText().trimmed());
        command.hex = hexCheck->isChecked();
        std::string error;
        const int row = commandList->currentRow();
        const bool saved = row >= 0
                               ? atCommandLibrary_.update(
                                     static_cast<std::size_t>(row), std::move(command), error)
                               : atCommandLibrary_.add(std::move(command), error);
        if (!saved) {
            QMessageBox::information(&dialog,
                                     QStringLiteral("无法保存指令"),
                                     fromUtf8(error));
            return;
        }
        const int selected = row >= 0 ? row
                                      : static_cast<int>(atCommandLibrary_.commands().size()) - 1;
        saveAtCommands();
        refreshAtCommandChoices();
        refreshList(selected);
    });
    connect(deleteButton, &QPushButton::clicked, &dialog, [&] {
        const int row = commandList->currentRow();
        if (row < 0)
            return;
        if (QMessageBox::question(
                &dialog,
                QStringLiteral("删除快捷指令"),
                QStringLiteral("确定删除“%1”吗？").arg(commandList->currentItem()->text())) !=
            QMessageBox::Yes) {
            return;
        }
        atCommandLibrary_.remove(static_cast<std::size_t>(row));
        saveAtCommands();
        refreshAtCommandChoices();
        refreshList(std::min(row, commandList->count() - 1));
    });

    const int selected = channels_[1].atCommandCombo
                             ? channels_[1].atCommandCombo->currentData().toInt()
                             : 0;
    refreshList(selected);
    dialog.exec();
}

void SerialWorkspace::toggleAutoSend(int index)
{
    auto& channel = channels_[index];
    if (channel.session.snapshot().scheduledSend.running) {
        stopAutoSend(index, true);
        return;
    }
    bool ok = false;
    auto payload = manualPayload(index, ok);
    if (!ok) {
        return;
    }
    if (payload.empty()) {
        QMessageBox::information(
            this, QStringLiteral("没有发送内容"), QStringLiteral("请输入要发送的数据。"));
        return;
    }
    if (!channel.session.isOpen()) {
        QMessageBox::information(
            this, QStringLiteral("尚未连接"), QStringLiteral("需要先连接该端点。"));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(channelDisplayName(index) + QStringLiteral(" · 定时 / 次数发送"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    auto* intervalSpin = new QSpinBox(&dialog);
    intervalSpin->setRange(10, 60000);
    intervalSpin->setValue(channel.autoIntervalMs);
    intervalSpin->setSuffix(QStringLiteral(" ms"));
    auto* countSpin = new QSpinBox(&dialog);
    countSpin->setRange(0, 1000000);
    countSpin->setValue(static_cast<int>(channel.autoTargetCount));
    countSpin->setSpecialValueText(QStringLiteral("持续"));
    auto* echoCheck = new QCheckBox(QStringLiteral("在收发监视中记录每次 TX"), &dialog);
    echoCheck->setChecked(channel.autoEcho);
    echoCheck->setToolTip(QStringLiteral("高频或长时间发送时可关闭，减少界面刷新开销"));
    form->addRow(QStringLiteral("发送间隔"), intervalSpin);
    form->addRow(QStringLiteral("发送次数"), countSpin);
    form->addRow(QString(), echoCheck);
    layout->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("开始"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    channel.autoIntervalMs = intervalSpin->value();
    channel.autoTargetCount = static_cast<std::uint64_t>(countSpin->value());
    channel.autoEcho = echoCheck->isChecked();

    PeriodicSendConfig config;
    config.interval = std::chrono::milliseconds(channel.autoIntervalMs);
    config.targetCount = channel.autoTargetCount;
    if (!channel.session.startScheduledSend(
            std::move(payload), config, channel.autoEcho ? 1u : 0u)) {
        QMessageBox::warning(
            this, QStringLiteral("参数无效"), QStringLiteral("发送间隔必须大于 0。"));
        return;
    }
    channel.autoSendButton->setText(QStringLiteral("停止计划发送"));
    channel.sendEdit->setEnabled(false);
    channel.sendHexCheck->setEnabled(false);
    channel.lineEndingCombo->setEnabled(false);
    emit activityChanged(QStringLiteral("%1 已开始定时 / 次数发送").arg(channelDisplayName(index)));
    updateGlobalControls();
}

void SerialWorkspace::stopAutoSend(int index, bool announce)
{
    auto& channel = channels_[index];
    const auto summary = channel.session.stopScheduledSend();
    if (channel.autoSendButton) {
        channel.autoSendButton->setText(QStringLiteral("定时 / 次数…"));
        channel.sendEdit->setEnabled(true);
        channel.sendHexCheck->setEnabled(true);
        channel.lineEndingCombo->setEnabled(true);
    }
    if (announce && (summary.running || summary.attempts != 0)) {
        emit activityChanged(QStringLiteral("%1 计划发送结束：已提交 %2 / 尝试 %3")
                                 .arg(channelDisplayName(index))
                                 .arg(summary.submitted)
                                 .arg(summary.attempts));
    }
    updateGlobalControls();
}

void SerialWorkspace::processScheduledSends()
{
    if (shuttingDown_) {
        return;
    }
    const auto now = PeriodicSendPlan::Clock::now();
    for (int index = 0; index < 2; ++index) {
        const auto result = channels_[index].session.processScheduledSend(now);
        if (result == testtools::ScheduledSendTick::Failed) {
            setChannelStatus(index, QStringLiteral("计划发送失败，连接或发送队列不可用"), true);
            stopAutoSend(index, true);
        } else if (result == testtools::ScheduledSendTick::Completed) {
            stopAutoSend(index, true);
        }
    }

    if (!stressTestRunning_ || stressAwaitingReceive_) {
        return;
    }
    bool allSent = true;
    bool queuesDrained = true;
    for (auto& direction : stressDirections_) {
        if (!direction.active) {
            continue;
        }
        if (direction.sendPlan.isDue(now)) {
            const auto frame = encodeIntegrityFrame(direction.nextSequence, direction.payloadSize);
            const bool sent = sendBytes(direction.transmitter, frame, false);
            direction.sendPlan.recordAttempt(sent, now);
            if (!sent) {
                stopStressTest(QStringLiteral("可靠性压测发送失败"));
                return;
            }
            ++direction.nextSequence;
        }
        allSent = allSent && !direction.sendPlan.isRunning();
        queuesDrained =
            queuesDrained && channels_[direction.transmitter].session.snapshot().pendingWrites == 0;
    }

    if (allSent && queuesDrained) {
        stressAwaitingReceive_ = true;
        const std::uint64_t generation = stressGeneration_;
        emit activityChanged(QStringLiteral("压测帧已发送完成，正在等待链路尾部数据"));
        QTimer::singleShot(1500, this, [this, generation] { finishStressTest(generation); });
    }
}

void SerialWorkspace::appendTransmitted(int index, const ByteBuffer &data)
{
    if (data.empty()) {
        return;
    }
    auto &channel = channels_[index];
    const auto now = QDateTime::currentDateTime();
    const auto result = channel.logBuffer.appendTx(
        data,
        now.toMSecsSinceEpoch(),
        toUtf8(now.toString(QStringLiteral("HH:mm:ss.zzz"))));
    applyLogAppendResult(index, result);
}

void SerialWorkspace::appendReceived(int index, const ByteBuffer& data)
{
    auto& channel = channels_[index];

    if (stressTestRunning_) {
        for (auto& direction : stressDirections_) {
            if (direction.active && direction.receiver == index) {
                direction.verifier.push(data);
            }
        }
        updateCounter(index);
        return;
    }

    if (bidirectionalTestRunning_ && !channel.expectedFrame.empty()) {
        channel.detectionBuffer.append(reinterpret_cast<const char*>(data.data()), data.size());
        if (channel.detectionBuffer.find(channel.expectedFrame) != std::string::npos) {
            channel.expectedFrame.clear();
            if (index == 0) {
                bToAPassed_ = true;
            } else {
                aToBPassed_ = true;
            }
            updateBidirectionalResult();
        } else if (channel.detectionBuffer.size() > 8192) {
            channel.detectionBuffer.erase(0, channel.detectionBuffer.size() - 8192);
        }
    }

    const auto now = QDateTime::currentDateTime();
    const auto result = channel.logBuffer.appendRx(
        data,
        now.toMSecsSinceEpoch(),
        toUtf8(now.toString(QStringLiteral("HH:mm:ss.zzz"))));
    applyLogAppendResult(index, result);
    updateCounter(index);
}

void SerialWorkspace::appendNotice(int index, const QString& message)
{
    auto& channel = channels_[index];
    const auto now = QDateTime::currentDateTime();
    const auto result = channel.logBuffer.appendNotice(
        toUtf8(message),
        now.toMSecsSinceEpoch(),
        toUtf8(now.toString(QStringLiteral("HH:mm:ss.zzz"))));
    applyLogAppendResult(index, result);
}

void SerialWorkspace::renderReceiveLog(int index)
{
    auto& channel = channels_[index];
    auto* edit = channel.receiveEdit;
    if (!edit) {
        return;
    }
    auto* vbar = edit->verticalScrollBar();
    const int previousScroll = vbar ? vbar->value() : 0;
    const bool atBottom = vbar ? (previousScroll >= vbar->maximum() - 4) : true;
    const bool autoScroll = !channel.autoScrollCheck || channel.autoScrollCheck->isChecked();
    const bool hex = channel.receiveHexCheck && channel.receiveHexCheck->isChecked();
    const bool showTimestamp = channel.timestampCheck && channel.timestampCheck->isChecked();

    edit->setPlainText(fromUtf8(channel.logBuffer.formatAll(hex, showTimestamp)));
    if (vbar) {
        if (autoScroll || atBottom) {
            vbar->setValue(vbar->maximum());
        } else {
            vbar->setValue(std::min(previousScroll, vbar->maximum()));
        }
    }
}

void SerialWorkspace::applyLogAppendResult(int index,
                                           const testtools::SerialLogAppendResult& result)
{
    if (!result.updatedLastEntry && result.addedEntries == 0) {
        return;
    }
    if (result.trimmedOldest) {
        renderReceiveLog(index);
        return;
    }
    auto& channel = channels_[index];
    auto* edit = channel.receiveEdit;
    auto* vbar = edit->verticalScrollBar();
    const int previousScroll = vbar ? vbar->value() : 0;
    const bool autoScroll = !channel.autoScrollCheck || channel.autoScrollCheck->isChecked();
    const bool hex = channel.receiveHexCheck && channel.receiveHexCheck->isChecked();
    const bool showTimestamp = channel.timestampCheck && channel.timestampCheck->isChecked();

    const auto& entries = channel.logBuffer.entries();
    QTextCursor cursor(edit->document());
    if (result.updatedLastEntry && entries.size() > result.addedEntries) {
        const std::size_t updatedIndex = entries.size() - 1 - result.addedEntries;
        QString line = fromUtf8(
            testtools::SerialLogBuffer::formatEntry(entries[updatedIndex], hex, showTimestamp));
        cursor.movePosition(QTextCursor::End);
        if (edit->document()->blockCount() > 1) {
            if (line.endsWith(QLatin1Char('\n'))) {
                line.chop(1);
            }
            cursor.movePosition(QTextCursor::PreviousBlock, QTextCursor::MoveAnchor);
            cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            cursor.insertText(line);
        } else {
            cursor.insertText(line);
        }
    }
    const std::size_t startAdded =
        entries.size() >= result.addedEntries ? (entries.size() - result.addedEntries) : 0;
    for (std::size_t i = startAdded; i < entries.size(); ++i) {
        const QString line =
            fromUtf8(testtools::SerialLogBuffer::formatEntry(entries[i], hex, showTimestamp));
        cursor.movePosition(QTextCursor::End);
        cursor.insertText(line);
    }
    if (vbar) {
        if (autoScroll) {
            vbar->setValue(vbar->maximum());
        } else {
            vbar->setValue(previousScroll);
        }
    }
}

void SerialWorkspace::updateCounter(int index)
{
    auto& channel = channels_[index];
    if (!channel.counterLabel) {
        return;
    }
    const auto snapshot = channel.session.snapshot();
    if (workspaceState_.mode() == WorkspaceMode::Single) {
        channel.counterLabel->setText(
            QStringLiteral("RX: %1 B  |  TX: %2 B").arg(snapshot.receivedBytes).arg(snapshot.sentBytes));
    } else {
        channel.counterLabel->setText(
            QStringLiteral("RX %1 · TX %2 B").arg(snapshot.receivedBytes).arg(snapshot.sentBytes));
    }
}

void SerialWorkspace::resetChannelCounters(int index)
{
    channels_[index].session.resetCounters();
    updateCounter(index);
    emit activityChanged(
        QStringLiteral("%1 的 RX / TX 计数已清零").arg(channelDisplayName(index)));
}

void SerialWorkspace::saveChannelLog(int index)
{
    auto& channel = channels_[index];
    const QString text = channel.receiveEdit->toPlainText();
    if (text.trimmed().isEmpty()) {
        QMessageBox::information(
            this, QStringLiteral("暂无日志"), QStringLiteral("当前收发记录为空，无需导出。"));
        return;
    }
    const QString defaultName =
        QStringLiteral("serial-%1-%2.log")
            .arg(index == 0 ? QStringLiteral("A") : QStringLiteral("B"),
                 QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
    const QString filePath = QFileDialog::getSaveFileName(
        this,
        channelDisplayName(index) + QStringLiteral(" · 保存收发日志"),
        defaultName,
        QStringLiteral("日志文件 (*.log *.txt);;所有文件 (*.*)"));
    if (filePath.isEmpty()) {
        return;
    }
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        QMessageBox::warning(
            this, QStringLiteral("保存失败"), QStringLiteral("无法写入文件：%1").arg(filePath));
        return;
    }
    const QByteArray utf8 = text.toUtf8();
    if (file.write(utf8) != utf8.size()) {
        QMessageBox::warning(
            this, QStringLiteral("保存失败"), QStringLiteral("写入日志文件不完整：%1").arg(filePath));
        return;
    }
    file.close();
    emit activityChanged(QStringLiteral("已保存收发日志：%1").arg(filePath));
}

void SerialWorkspace::runBidirectionalTest()
{
    if (!channels_[0].session.isOpen() || !channels_[1].session.isOpen()) {
        QMessageBox::information(
            this, QStringLiteral("尚未连接"), QStringLiteral("需要先连接端点 A 和端点 B。"));
        return;
    }

    const std::string token = std::to_string(QDateTime::currentMSecsSinceEpoch());
    const std::string frameA = "SERIAL_POC_A_TO_B_" + token;
    const std::string frameB = "SERIAL_POC_B_TO_A_" + token;
    channels_[1].expectedFrame = frameA;
    channels_[0].expectedFrame = frameB;
    channels_[0].detectionBuffer.clear();
    channels_[1].detectionBuffer.clear();
    aToBPassed_ = false;
    bToAPassed_ = false;
    bidirectionalTestRunning_ = true;
    const std::uint64_t generation = ++testGeneration_;
    updateBidirectionalResult();
    updateGlobalControls();

    ByteBuffer bytesA(frameA.begin(), frameA.end());
    ByteBuffer bytesB(frameB.begin(), frameB.end());
    appendLineEnding(bytesA, lineEndingFromIndex(channels_[0].lineEndingCombo->currentIndex()));
    appendLineEnding(bytesB, lineEndingFromIndex(channels_[1].lineEndingCombo->currentIndex()));
    if (!sendBytes(0, bytesA) || !sendBytes(1, bytesB)) {
        bidirectionalTestRunning_ = false;
        testResultLabel_->setText(QStringLiteral("双向收发验证 · 发送失败"));
        setVisualRole(testResultLabel_, QStringLiteral("testStatusError"));
        updateGlobalControls();
        return;
    }

    QPointer<SerialWorkspace> guard(this);
    QTimer::singleShot(3000, this, [guard, generation] {
        if (guard && guard->bidirectionalTestRunning_ && guard->testGeneration_ == generation) {
            guard->updateBidirectionalResult(true);
        }
    });
}

void SerialWorkspace::updateBidirectionalResult(bool timedOut)
{
    if (aToBPassed_ && bToAPassed_) {
        bidirectionalTestRunning_ = false;
        testResultLabel_->setText(QStringLiteral("双向收发验证 · A→B PASS  /  B→A PASS"));
        setVisualRole(testResultLabel_, QStringLiteral("testStatusGood"));
    } else if (timedOut) {
        bidirectionalTestRunning_ = false;
        testResultLabel_->setText(
            QStringLiteral("双向收发验证超时 · A→B %1  /  B→A %2")
                .arg(aToBPassed_ ? QStringLiteral("PASS") : QStringLiteral("FAIL"))
                .arg(bToAPassed_ ? QStringLiteral("PASS") : QStringLiteral("FAIL")));
        setVisualRole(testResultLabel_, QStringLiteral("testStatusError"));
    } else {
        testResultLabel_->setText(
            QStringLiteral("正在验证 · A→B %1  /  B→A %2")
                .arg(aToBPassed_ ? QStringLiteral("PASS") : QStringLiteral("等待"))
                .arg(bToAPassed_ ? QStringLiteral("PASS") : QStringLiteral("等待")));
        setVisualRole(testResultLabel_, QStringLiteral("testStatusWorking"));
    }
    updateGlobalControls();
}

void SerialWorkspace::showStressDialog()
{
    if (!channels_[0].session.isOpen() || !channels_[1].session.isOpen()) {
        QMessageBox::information(this,
                                 QStringLiteral("尚未连接"),
                                 QStringLiteral("可靠性压测需要先连接端点 A 和端点 B。"));
        return;
    }
    if (channelConfig(0).dataBits != 8 || channelConfig(1).dataBits != 8) {
        QMessageBox::information(
            this,
            QStringLiteral("串口格式不适合压测"),
            QStringLiteral(
                "可靠性压测帧包含完整 8 bit 二进制数据，请先将两个端点的数据位都设为 8。"));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("串口可靠性压测"));
    dialog.setMinimumWidth(420);
    auto* layout = new QVBoxLayout(&dialog);
    auto* description = new QLabel(
        QStringLiteral(
            "发送带序号、确定性载荷和 CRC32 的测试帧。BER 只统计已解析载荷，丢帧单独报告。"),
        &dialog);
    description->setWordWrap(true);
    description->setObjectName(QStringLiteral("cardBody"));
    layout->addWidget(description);

    auto *form = new QFormLayout;
    auto *directionCombo = new QComboBox(&dialog);
    directionCombo->addItems({QStringLiteral("A → B"), QStringLiteral("B → A"),
                              QStringLiteral("双向同时")});
    auto *frameCountSpin = new QSpinBox(&dialog);
    frameCountSpin->setRange(1, 1000000);
    frameCountSpin->setValue(1000);
    frameCountSpin->setSuffix(QStringLiteral(" 帧"));
    auto *payloadSizeSpin = new QSpinBox(&dialog);
    payloadSizeSpin->setRange(1, static_cast<int>(IntegrityStreamVerifier::MaximumPayloadSize));
    payloadSizeSpin->setValue(256);
    payloadSizeSpin->setSuffix(QStringLiteral(" B"));
    auto *intervalSpin = new QSpinBox(&dialog);
    intervalSpin->setRange(10, 60000);
    intervalSpin->setValue(10);
    intervalSpin->setSuffix(QStringLiteral(" ms"));
    form->addRow(QStringLiteral("方向"), directionCombo);
    form->addRow(QStringLiteral("每方向帧数"), frameCountSpin);
    form->addRow(QStringLiteral("有效载荷"), payloadSizeSpin);
    form->addRow(QStringLiteral("发送间隔"), intervalSpin);
    layout->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("开始压测"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() == QDialog::Accepted) {
        startStressTest(directionCombo->currentIndex(),
                        static_cast<std::uint64_t>(frameCountSpin->value()),
                        static_cast<std::size_t>(payloadSizeSpin->value()),
                        intervalSpin->value());
    }
}

void SerialWorkspace::startStressTest(int direction, std::uint64_t frameCount,
                                      std::size_t payloadSize, int intervalMs)
{
    stopAutoSend(0);
    stopAutoSend(1);
    stressDirections_ = {};

    int effectiveIntervalMs = intervalMs;
    const auto accountWireTime = [this, payloadSize, &effectiveIntervalMs](int transmitter) {
        const auto config = channelConfig(transmitter);
        const int parityBits = config.parity == Parity::None ? 0 : 1;
        const int stopBits = config.stopBits == StopBits::Two ? 2 : 1;
        const int bitsPerByte = 1 + config.dataBits + parityBits + stopBits;
        const std::uint64_t frameBits = static_cast<std::uint64_t>(payloadSize + 16)
                                        * static_cast<std::uint64_t>(bitsPerByte);
        const int wireTimeMs = static_cast<int>((frameBits * 1000
                                                 + static_cast<std::uint64_t>(config.baudRate) - 1)
                                                / static_cast<std::uint64_t>(config.baudRate));
        effectiveIntervalMs = std::max(effectiveIntervalMs, wireTimeMs);
    };
    if (direction == 0 || direction == 2) {
        accountWireTime(0);
    }
    if (direction == 1 || direction == 2) {
        accountWireTime(1);
    }
    if (effectiveIntervalMs > intervalMs) {
        QMessageBox::information(
            this, QStringLiteral("已按串口线速调整"),
            QStringLiteral("当前帧在所选波特率下至少需要约 %1 ms，实际发送间隔已由 %2 ms 调整为 %1 ms，避免后台队列堆积。")
                .arg(effectiveIntervalMs).arg(intervalMs));
    }

    const auto activate = [this, frameCount, payloadSize, effectiveIntervalMs](int slot, int tx, int rx) {
        auto &testDirection = stressDirections_[slot];
        testDirection.active = true;
        testDirection.transmitter = tx;
        testDirection.receiver = rx;
        testDirection.payloadSize = payloadSize;
        testDirection.nextSequence = 0;
        testDirection.verifier.reset();
        testDirection.sendPlan.start({std::chrono::milliseconds(effectiveIntervalMs), frameCount});
    };
    if (direction == 0 || direction == 2) {
        activate(0, 0, 1);
    }
    if (direction == 1) {
        activate(0, 1, 0);
    } else if (direction == 2) {
        activate(1, 1, 0);
    }

    stressTestRunning_ = true;
    stressAwaitingReceive_ = false;
    ++stressGeneration_;
    stressResultLabel_->setText(QStringLiteral("可靠性压测 · 正在发送"));
    setVisualRole(stressResultLabel_, QStringLiteral("testStatusWorking"));
    for (int index = 0; index < 2; ++index) {
        appendNotice(index,
                     QStringLiteral("— 可靠性压测数据流已折叠，仅更新字节计数与统计结果 —"));
    }
    emit activityChanged(QStringLiteral("串口可靠性压测已开始"));
    updateGlobalControls();
}

void SerialWorkspace::stopStressTest(const QString &reason, bool completed)
{
    if (!stressTestRunning_) {
        return;
    }

    bool passed = completed;
    QStringList summaries;
    for (auto &direction : stressDirections_) {
        if (!direction.active) {
            continue;
        }
        const auto &stats = direction.verifier.stats();
        const std::uint64_t sent = direction.sendPlan.successes();
        const std::uint64_t lost = sent > stats.uniqueFrames ? sent - stats.uniqueFrames : 0;
        passed = passed && lost == 0 && stats.corruptedFrames == 0
                 && stats.framingErrors == 0 && stats.bitErrors == 0;
        summaries.push_back(stressDirectionSummary(direction));
        direction.sendPlan.stop();
    }

    stressTestRunning_ = false;
    stressAwaitingReceive_ = false;
    if (completed) {
        stressResultLabel_->setText(QStringLiteral("可靠性压测 · %1 · %2")
                                        .arg(passed ? QStringLiteral("PASS") : QStringLiteral("FAIL"),
                                             summaries.join(QStringLiteral(" / "))));
        setVisualRole(stressResultLabel_, passed ? QStringLiteral("testStatusGood")
                                                 : QStringLiteral("testStatusError"));
    } else {
        stressResultLabel_->setText(QStringLiteral("可靠性压测 · 已停止 · %1")
                                        .arg(summaries.join(QStringLiteral(" / "))));
        setVisualRole(stressResultLabel_, QStringLiteral("testStatusNeutral"));
    }
    emit activityChanged(reason);
    updateGlobalControls();
}

void SerialWorkspace::finishStressTest(std::uint64_t generation)
{
    if (!stressTestRunning_ || generation != stressGeneration_) {
        return;
    }
    stopStressTest(QStringLiteral("串口可靠性压测已完成"), true);
}

QString SerialWorkspace::stressDirectionSummary(const StressDirection &direction) const
{
    const auto &stats = direction.verifier.stats();
    const std::uint64_t sent = direction.sendPlan.successes();
    const std::uint64_t lost = sent > stats.uniqueFrames ? sent - stats.uniqueFrames : 0;
    return QStringLiteral("%1→%2 收到 %3/%4，丢 %5，坏帧 %6，格式错 %7，BER %8")
        .arg(direction.transmitter == 0 ? QStringLiteral("A") : QStringLiteral("B"))
        .arg(direction.receiver == 0 ? QStringLiteral("A") : QStringLiteral("B"))
        .arg(stats.uniqueFrames)
        .arg(sent)
        .arg(lost)
        .arg(stats.corruptedFrames)
        .arg(stats.framingErrors)
        .arg(QString::number(stats.bitErrorRate(), 'g', 4));
}
