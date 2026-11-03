#include "serial_endpoint_workspace.h"
#include "ui/icons.h"
#include "ui/theme.h"
#include "ui/widgets/serial_connection_bar.h"
#include "ui/widgets/serial_keyword_highlighter.h"
#include "ui/widgets/serial_receive_view.h"
#include "ui/widgets/serial_shortcut_sidebar.h"
#include "ui/widgets/serial_workspace_helpers.h"

#include "application/serial/serial_transport_service.h"
#include "core/serial/codec.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QEvent>
#include <QtCore/QFile>
#include <QtCore/QMetaObject>
#include <QtCore/QPointer>
#include <QtCore/QSignalBlocker>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtGui/QActionGroup>
#include <QtGui/QClipboard>
#include <QtGui/QInputMethodEvent>
#include <QtGui/QKeyEvent>
#include <QtGui/QPainter>
#include <QtGui/QResizeEvent>
#include <QtGui/QShortcut>
#include <QtGui/QTextBlock>
#include <QtGui/QTextCharFormat>
#include <QtGui/QTextCursor>
#include <QtWidgets/QApplication>
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
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QStyle>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <functional>
#include <utility>

using namespace serialdemo;

using namespace testtools::ui::serial;

SerialEndpointWorkspace::SerialEndpointWorkspace(
    QWidget* parent, int channelCount, std::shared_ptr<testtools::SerialPortClaims> claims)
    : QWidget(parent), channels_(std::size_t(channelCount)),
      portClaims_(claims ? std::move(claims) : std::make_shared<testtools::SerialPortClaims>())
{
}

SerialEndpointWorkspace::~SerialEndpointWorkspace()
{
    prepareForShutdown();
}

bool SerialEndpointWorkspace::initializePreferences(const wchar_t* filename,
                                                    bool migrateTerminal,
                                                    int defaultLineEnding)
{
    const auto root = storageDirectory();
    preferencesStorePath_ = root / filename;
    std::error_code existsError;
    const bool exists = std::filesystem::exists(preferencesStorePath_, existsError);
    std::string error;
    preferencesLoaded_ =
        !existsError && exists &&
        testtools::SerialPreferencesStore::load(preferencesStorePath_, preferences_, error);
    if (migrateTerminal && !exists)
        preferencesLoaded_ = testtools::SerialPreferencesStore::load(
            root / L"serial-preferences.json", preferences_, error);
    if (!preferencesLoaded_)
        preferences_.channels[1].lineEnding = defaultLineEnding;
    return !exists;
}

void SerialEndpointWorkspace::finishSetup()
{
    for (int index = 0; index < int(channels_.size()); ++index)
        attachSessionCallbacks(index);
    recordingTimer_ = new QTimer(this);
    recordingTimer_->setInterval(200);
    connect(recordingTimer_, &QTimer::timeout, this, &SerialEndpointWorkspace::pollRecordings);
    recordingTimer_->start();
    pollRecordings();
    refreshTargets();
    updateGlobalControls();
}

void SerialEndpointWorkspace::prepareForShutdown()
{
    if (shuttingDown_)
        return;
    shuttingDown_ = true;
    if (recordingTimer_)
        recordingTimer_->stop();
    stopRecordings();
    stopPageTasks();
    controlTasks_.wait();
    disconnectAll();
    scanTasks_.wait();
    QCoreApplication::removePostedEvents(this);
}

void SerialEndpointWorkspace::buildPageHeader(const QString& title, const QString& description)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(8);
    auto* bar = new QFrame(this);
    bar->setObjectName(QStringLiteral("taskModeBar"));
    auto* row = new QHBoxLayout(bar);
    row->setContentsMargins(2, 0, 2, 2);
    row->setSpacing(12);
    pageTitle_ = new QLabel(title, bar);
    pageTitle_->setObjectName(QStringLiteral("pageTitle"));
    modeDescriptionLabel_ = new QLabel(description, bar);
    modeDescriptionLabel_->setObjectName(QStringLiteral("modeDescription"));
    modeDescriptionLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row->addWidget(pageTitle_);
    row->addStretch();
    row->addWidget(modeDescriptionLabel_);
    root->addWidget(bar);
}

void SerialEndpointWorkspace::addChannelBody(QVBoxLayout* layout, int index)
{
    layout->addWidget(channels_[index].logSendBody, 1);
}

void SerialEndpointWorkspace::installSearchShortcuts()
{
    const auto selected = [this] {
        auto* focus = QApplication::focusWidget();
        for (int index = 0; index < int(channels_.size()); ++index)
            if (focus && channels_[index].group->isVisible() &&
                channels_[index].group->isAncestorOf(focus))
                return index;
        return primaryChannelIndex();
    };
    auto* find = new QShortcut(
        isTerminal() ? QKeySequence(QStringLiteral("Ctrl+Shift+F")) : QKeySequence::Find, this);
    find->setContext(Qt::WidgetWithChildrenShortcut);
    connect(find, &QShortcut::activated, this, [this, selected] {
        auto& channel = channels_[selected()];
        channel.searchButton->setChecked(true);
        channel.searchEdit->setFocus();
        channel.searchEdit->selectAll();
    });
    for (int step : {-1, 1}) {
        auto* next = new QShortcut(QKeySequence(step > 0 ? "F3" : "Shift+F3"), this);
        next->setContext(Qt::WidgetWithChildrenShortcut);
        connect(next, &QShortcut::activated, this, [this, selected, step] {
            findLogMatch(selected(), step);
        });
    }
}

bool SerialEndpointWorkspace::eventFilter(QObject* watched, QEvent* event)
{
    if (handleReceiveEvent(watched, event))
        return true;
    if (event->type() == QEvent::Resize) {
        for (auto& channel : channels_)
            if (watched == channel.logSendBody)
                QTimer::singleShot(0, this, [this] { updateLayout(); });
    }
    if (event->type() == QEvent::KeyPress &&
        static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        for (auto& channel : channels_)
            if ((watched == channel.searchEdit || watched == channel.receiveEdit) &&
                channel.searchButton->isChecked()) {
                channel.searchButton->setChecked(false);
                return true;
            }
    }
    return QWidget::eventFilter(watched, event);
}

void SerialEndpointWorkspace::refreshTargets()
{
    if (shuttingDown_ || hasActiveConnections() || scanning_)
        return;
    refreshLocalPorts();
    refreshAgentPorts();
}
bool SerialEndpointWorkspace::hasActiveConnections() const
{
    for (int index = 0; index < int(channels_.size()); ++index)
        if (channelActive(index))
            return true;
    return false;
}
bool SerialEndpointWorkspace::hasActiveAgentConnection() const
{
    for (int index = 0; index < int(channels_.size()); ++index)
        if (channelUsesAgent(index) && channelActive(index))
            return true;
    return false;
}
void SerialEndpointWorkspace::disconnectAll()
{
    for (int index = 0; index < int(channels_.size()); ++index)
        disconnectChannel(index, channelActive(index));
}
void SerialEndpointWorkspace::clearReceive(int index)
{
    auto& channel = channels_[index];
    channel.logBuffer.clear();
    channel.receiveEdit->clear();
    channel.truncatedLabel->hide();
    updateSearchMatches(index);
}
bool SerialEndpointWorkspace::historyTruncated(int index) const
{
    return channels_[index].logBuffer.truncated();
}
void SerialEndpointWorkspace::appendReceived(int index, const ByteBuffer& data)
{
    auto& channel = channels_[index];
    const auto now = QDateTime::currentDateTime();
    const auto result = channel.logBuffer.appendRx(
        data, now.toMSecsSinceEpoch(), toUtf8(now.toString(QStringLiteral("HH:mm:ss.zzz"))));
    applyLogAppendResult(index, result);
    updateCounter(index);
}
bool SerialEndpointWorkspace::resourceConflict() const
{
    if (channels_.size() != 2)
        return false;
    auto first = endpointSelection(0);
    auto second = endpointSelection(1);
    first.active = second.active = true;
    return !first.endpoint.empty() && !second.endpoint.empty() &&
           testtools::SerialWorkspaceState::endpointConflict(first, second) !=
               testtools::SerialEndpointConflict::None;
}
bool SerialEndpointWorkspace::channelReady(int index) const
{
    const auto& channel = channels_[index];
    const bool basic = !channel.portCombo->currentText().trimmed().isEmpty() &&
                       channel.baudCombo->currentText().toInt() > 0 &&
                       (!channelUsesAgent(index) || selectedAgentIsOnline());
    return basic && !(channels_.size() == 2 && channelActive(1 - index) && resourceConflict());
}

void SerialEndpointWorkspace::applyChannelLayout(bool single)
{
    for (int index = 0; index < int(channels_.size()); ++index) {
        auto& channel = channels_[index];
        channel.group->setProperty("singleChannel", single);
        channel.group->layout()->setContentsMargins(
            single ? 0 : 14, single ? 0 : 10, single ? 0 : 14, single ? 0 : 12);
        channel.group->style()->unpolish(channel.group);
        channel.group->style()->polish(channel.group);
        if (channel.sendEdit)
            channel.sendEdit->setFixedHeight(single ? 56 : 44);
        channel.sourceCombo->setItemText(0, QStringLiteral("PC端串口"));
        channel.sourceCombo->setMinimumWidth(single ? 148 : 100);
        channel.sourceCombo->setMaximumWidth(single ? 176 : 116);
        channel.portCombo->setMinimumContentsLength(single ? 12 : 8);
        channel.portCombo->setMinimumWidth(single ? 120 : 80);
        channel.portCombo->setSizePolicy(single ? QSizePolicy::Expanding : QSizePolicy::Ignored,
                                         QSizePolicy::Fixed);
        channel.baudCombo->setMinimumWidth(118);
        channel.baudCombo->setMaximumWidth(132);
        // 双口面板宽度较窄时，将字节计数与截断提示移至独立紧凑工具行。
        auto* counterLayout = single ? channel.receiveHeaderLayout : channel.counterRowLayout;
        auto* previousLayout = single ? channel.counterRowLayout : channel.receiveHeaderLayout;
        previousLayout->removeWidget(channel.counterLabel);
        if (counterLayout->indexOf(channel.counterLabel) < 0) {
            counterLayout->insertWidget(single ? counterLayout->indexOf(channel.searchButton) : 0,
                                        channel.counterLabel);
        }
        previousLayout->removeWidget(channel.truncatedLabel);
        if (counterLayout->indexOf(channel.truncatedLabel) < 0) {
            counterLayout->insertWidget(1, channel.truncatedLabel);
        }
        channel.counterRow->setVisible(!single);
        channel.counterLabel->show();
        updateTruncation(index);
        channel.receiveHexCheck->setText(single ? QStringLiteral("HEX 显示")
                                                : QStringLiteral("HEX"));
        channel.autoScrollCheck->setText(single ? QStringLiteral("自动滚动")
                                                : QStringLiteral("滚动"));
        channel.resetCounterButton->setText(single ? QStringLiteral("计数清零")
                                                   : QStringLiteral("清零"));
        channel.saveLogButton->setText(single ? QStringLiteral("保存日志…")
                                              : QStringLiteral("保存…"));
    }
}

void SerialEndpointWorkspace::updateGlobalControls()
{
    for (int index = 0; index < int(channels_.size()); ++index) {
        auto& channel = channels_[index];
        const bool needsService = channelUsesAgent(index) && !selectedAgentIsOnline();
        const bool connecting = serviceState_ == testtools::AgentServiceState::Connecting;
        channel.serviceHint->setVisible(needsService);
        channel.statusLabel->setVisible(!needsService);
        channel.connectButton->setVisible(!needsService);
        channel.serviceHintLabel->setText(maintenanceBlocked_ ? QStringLiteral("设备维护中…")
                                          : connecting        ? QStringLiteral("正在启动服务…")
                                          : serviceState_ == testtools::AgentServiceState::Failed
                                              ? QStringLiteral("服务连接失败")
                                              : QStringLiteral("需连接板端服务"));
        channel.serviceHintLabel->setToolTip(currentAdbDevice_.state != "device"
                                                 ? QStringLiteral("请先选择在线且已授权的 ADB 设备")
                                                 : serviceDetail_);
        channel.serviceConnectButton->setText(serviceState_ == testtools::AgentServiceState::Failed
                                                  ? QStringLiteral("重试连接")
                                                  : QStringLiteral("连接服务"));
        channel.serviceConnectButton->setEnabled(
            currentAdbDevice_.state == "device" && !connecting && !maintenanceBlocked_ &&
            !serviceCleanupPending_ && !hasActiveAgentConnection());
    }

    for (int index = 0; index < int(channels_.size()); ++index) {
        const auto session = channels_[index].session.snapshot();
        const bool open = session.open;
        if (channels_[index].refreshPortsButton) {
            const bool agent = channelUsesAgent(index);
            channels_[index].refreshPortsButton->setEnabled(
                !shuttingDown_ && !channelActive(index) &&
                (!agent || (!scanning_ && !hasActiveAgentConnection() && selectedAgentIsOnline())));
            channels_[index].refreshPortsButton->setToolTip(
                agent ? selectedAgentIsOnline() ? QStringLiteral("重新扫描设备串口列表")
                                                : QStringLiteral("请先连接当前设备的板端服务")
                      : QStringLiteral("重新扫描 PC端串口，无需连接板端服务"));
        }
        if (channels_[index].disconnectPending ||
            session.state == testtools::SerialSessionState::Disconnecting) {
            channels_[index].connectButton->setEnabled(false);
        } else if (channelActive(index)) {
            channels_[index].connectButton->setEnabled(true);
        } else {
            channels_[index].connectButton->setEnabled((!scanning_ || !channelUsesAgent(index)) &&
                                                       channelReady(index));
        }
        if (channels_[index].sendButton)
            channels_[index].sendButton->setEnabled(open && !stressRunning() &&
                                                    !session.scheduledSend.running);
        const auto role =
            channelActive(index) ? QStringLiteral("secondary") : QStringLiteral("primary");
        auto* connectButton = channels_[index].connectButton;
        if (connectButton->property("buttonRole").toString() != role) {
            connectButton->setProperty("buttonRole", role);
            connectButton->style()->unpolish(connectButton);
            connectButton->style()->polish(connectButton);
        }
        auto& current = channels_[index];
        const bool controlsReady = open && !current.controlPending && !stressRunning() &&
                                   !controlTasksRunning() && !session.scheduledSend.running;
        current.dtrCheck->setEnabled(controlsReady &&
                                     current.session.supportsControlLine(SerialControlLine::Dtr));
        current.rtsCheck->setEnabled(controlsReady &&
                                     current.session.supportsControlLine(SerialControlLine::Rts));
        current.rtsCheck->setToolTip(
            channelUsesAgent(index) ? QStringLiteral("设备串口暂不支持手动控制 RTS")
            : current.flowControlCombo->currentIndex() == 1
                ? QStringLiteral("RTS 正由硬件流控管理")
                : QStringLiteral("连接 PC端串口后可设置；半选表示本次连接尚未手动设置"));
        current.dtrCheck->setToolTip(
            channelUsesAgent(index)
                ? QStringLiteral("设备串口暂不支持手动控制 DTR")
                : QStringLiteral("连接 PC端串口后可设置；半选表示本次连接尚未手动设置"));
        if (channels_[index].autoSendButton)
            channels_[index].autoSendButton->setEnabled(open && !stressRunning() &&
                                                        !current.controlPending);
    }
    updatePageControls();
    updateLayout();
    pollRecordings();
    emit busyChanged(hasActiveConnections());
}

QWidget* SerialEndpointWorkspace::buildChannelPanel(int index)
{
    auto& channel = channels_[index];
    channel.group = new QGroupBox(this);
    channel.group->setObjectName(QStringLiteral("endpointCard"));
    auto* layout = new QVBoxLayout(channel.group);
    layout->setContentsMargins(14, 10, 14, 12);
    layout->setSpacing(8);

    channel.headerWidget = new QWidget(channel.group);
    channel.headerLayout = new QHBoxLayout(channel.headerWidget);
    channel.headerLayout->setContentsMargins(0, 0, 0, 0);
    channel.headerLayout->setSpacing(6);
    channel.badgeLabel =
        new QLabel(index == 0 ? QStringLiteral("A") : QStringLiteral("B"), channel.headerWidget);
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
    auto* actionsLayout = new QHBoxLayout(channel.actionsContainer);
    actionsLayout->setContentsMargins(0, 0, 0, 0);
    actionsLayout->setSpacing(6);
    channel.refreshPortsButton =
        new QPushButton(QStringLiteral("刷新串口"), channel.actionsContainer);
    channel.refreshPortsButton->setObjectName(QStringLiteral("ghostButton"));
    channel.refreshPortsButton->setToolTip(QStringLiteral("重新扫描当前所选位置的串口"));
    channel.advancedSettingsButton =
        new QPushButton(QStringLiteral("更多参数"), channel.actionsContainer);
    channel.advancedSettingsButton->setObjectName(QStringLiteral("ghostButton"));
    channel.advancedSettingsButton->setCheckable(true);
    channel.statusLabel = new QLabel(QStringLiteral("● 未连接"), channel.actionsContainer);
    channel.statusLabel->setObjectName(QStringLiteral("connectionStatusIdle"));
    channel.statusLabel->setAlignment(Qt::AlignCenter);
    channel.connectButton = new QPushButton(QStringLiteral("连接"), channel.actionsContainer);
    channel.connectButton->setMinimumWidth(78);
    channel.connectButton->setCursor(Qt::PointingHandCursor);
    channel.serviceHint = new QFrame(channel.actionsContainer);
    channel.serviceHint->setObjectName(QStringLiteral("serialServiceHint"));
    auto* hintLayout = new QHBoxLayout(channel.serviceHint);
    hintLayout->setContentsMargins(0, 0, 0, 0);
    hintLayout->setSpacing(6);
    channel.serviceHintLabel = new QLabel(QStringLiteral("需连接板端服务"), channel.serviceHint);
    channel.serviceHintLabel->setObjectName(QStringLiteral("serialServiceMessage"));
    channel.serviceConnectButton = new QPushButton(QStringLiteral("连接服务"), channel.serviceHint);
    channel.serviceConnectButton->setObjectName(QStringLiteral("serialServiceConnect"));
    hintLayout->addWidget(channel.serviceHintLabel);
    hintLayout->addWidget(channel.serviceConnectButton);
    channel.serviceHint->hide();
    connect(channel.serviceConnectButton,
            &QPushButton::clicked,
            this,
            &SerialEndpointWorkspace::prepareServiceRequested);
    channel.refreshPortsButton->setIcon(testtools::ui::appIcon(testtools::ui::Icon::Refresh));
    channel.refreshPortsButton->setIconSize({20, 20});
    actionsLayout->addWidget(channel.refreshPortsButton);
    actionsLayout->addWidget(channel.advancedSettingsButton);
    actionsLayout->addWidget(channel.serviceHint);
    actionsLayout->addWidget(channel.statusLabel);
    actionsLayout->addWidget(channel.connectButton);

    channel.headerLayout->addWidget(channel.actionsContainer);
    layout->addWidget(channel.headerWidget);

    auto* connection = new SerialConnectionBar(channel.group);
    channel.sourceCombo = connection->sourceCombo;
    channel.portCombo = connection->portCombo;
    channel.baudCombo = connection->baudCombo;
    channel.dataBitsCombo = connection->dataBitsCombo;
    channel.parityCombo = connection->parityCombo;
    channel.stopBitsCombo = connection->stopBitsCombo;
    channel.flowControlCombo = connection->flowControlCombo;
    channel.dtrCheck = connection->dtrCheck;
    channel.rtsCheck = connection->rtsCheck;
    channel.settingsRowLayout = connection->settingsRowLayout;
    channel.advancedSettingsArea = connection->advancedSettingsArea;
    layout->addWidget(connection);
    channel.dtrCheck->setProperty("diagnosticObjectName", QStringLiteral("dtr%1").arg(index));
    channel.rtsCheck->setProperty("diagnosticObjectName", QStringLiteral("rts%1").arg(index));
    connect(channel.dtrCheck, &QCheckBox::clicked, this, [this, index](bool checked) {
        setControlLine(index, SerialControlLine::Dtr, checked);
    });
    connect(channel.rtsCheck, &QCheckBox::clicked, this, [this, index](bool checked) {
        setControlLine(index, SerialControlLine::Rts, checked);
    });

    channel.logSendBody = new QWidget(channel.group);
    channel.logSendBody->installEventFilter(this);
    auto* logSendLayout = new QVBoxLayout(channel.logSendBody);
    logSendLayout->setContentsMargins(0, 0, 0, 0);
    logSendLayout->setSpacing(8);
    addChannelBody(layout, index);

    auto* terminalFrame = new QFrame(channel.group);
    terminalFrame->setObjectName(QStringLiteral("terminalFrame"));
    auto* terminalLayout = new QVBoxLayout(terminalFrame);
    terminalLayout->setContentsMargins(0, 0, 0, 0);
    terminalLayout->setSpacing(0);

    auto* terminalHeader = new QFrame(terminalFrame);
    terminalHeader->setObjectName(QStringLiteral("terminalHeader"));
    auto* headerRows = new QVBoxLayout(terminalHeader);
    headerRows->setContentsMargins(0, 0, 0, 0);
    headerRows->setSpacing(0);
    auto* receiveHeader = new QHBoxLayout;
    headerRows->addLayout(receiveHeader);
    channel.receiveHeaderLayout = receiveHeader;
    receiveHeader->setContentsMargins(12, 5, 10, 5);
    receiveHeader->setSpacing(8);

    auto* receiveTitle = new QLabel(QStringLiteral("收发记录"), terminalHeader);
    channel.receiveTitle = receiveTitle;
    receiveTitle->setObjectName(QStringLiteral("sectionLabel"));

    channel.receiveHexCheck = new QCheckBox(QStringLiteral("HEX 显示"), terminalHeader);
    channel.timestampCheck = new QCheckBox(QStringLiteral("时间戳"), terminalHeader);
    channel.autoScrollCheck = new QCheckBox(QStringLiteral("自动滚动"), terminalHeader);
    channel.autoScrollCheck->setChecked(true);
    channel.autoScrollCheck->setToolTip(
        QStringLiteral("收到新数据时自动滚动到底部；取消勾选后可固定视图查看或复制历史日志"));

    channel.counterLabel = new QLabel(QStringLiteral("RX: 0 B  |  TX: 0 B"), terminalHeader);
    channel.counterLabel->setObjectName(QStringLiteral("counterLabel"));
    channel.counterLabel->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);

    channel.resetCounterButton = new QPushButton(QStringLiteral("计数清零"), terminalHeader);
    channel.resetCounterButton->setObjectName(QStringLiteral("clearButton"));
    channel.resetCounterButton->setToolTip(
        QStringLiteral("将当前端点的 RX / TX 字节计数归零，保留收发记录"));

    channel.saveLogButton = new QPushButton(QStringLiteral("保存日志…"), terminalHeader);
    channel.saveLogButton->setObjectName(QStringLiteral("clearButton"));
    channel.saveLogButton->setToolTip(
        QStringLiteral("保存当前显示的日志；启用过滤时仅保存匹配记录"));

    channel.clearReceiveButton = new QPushButton(QStringLiteral("清空"), terminalHeader);
    channel.clearReceiveButton->setObjectName(QStringLiteral("clearButton"));
    channel.clearReceiveButton->setToolTip(
        QStringLiteral("清空当前收发记录，保留 RX / TX 字节计数"));

    receiveHeader->addWidget(receiveTitle);
    channel.truncatedLabel = new QLabel(QStringLiteral("已截断"), terminalHeader);
    channel.truncatedLabel->setObjectName(QStringLiteral("mutedLabel"));
    channel.truncatedLabel->setToolTip(
        QStringLiteral("缓冲已淘汰最早记录；保存日志仅包含当前显示内容"));
    channel.truncatedLabel->hide();
    receiveHeader->addWidget(channel.truncatedLabel);
    receiveHeader->addWidget(channel.receiveHexCheck);
    receiveHeader->addWidget(channel.timestampCheck);
    receiveHeader->addWidget(channel.autoScrollCheck);
    receiveHeader->addStretch();
    receiveHeader->addWidget(channel.counterLabel);
    channel.recordButton = new QPushButton(QStringLiteral("录制…"), terminalHeader);
    channel.recordButton->setObjectName(QStringLiteral("serialRecordButton"));
    channel.recordButton->setToolTip(
        QStringLiteral("持续录制到独立文件，不受清屏、过滤或显示截断影响"));
    receiveHeader->addWidget(channel.recordButton);
    channel.highlightButton = new QPushButton(QStringLiteral("高亮"), terminalHeader);
    channel.highlightButton->setObjectName(QStringLiteral("serialHighlightButton"));
    channel.highlightButton->setCheckable(true);
    channel.highlightButton->setToolTip(
        QStringLiteral("只读标记错误与警告词；不改变原始字节、导出内容或 ANSI 样式"));
    receiveHeader->addWidget(channel.highlightButton);
    channel.searchButton = new QPushButton(QStringLiteral("搜索"), terminalHeader);
    channel.searchButton->setObjectName(QStringLiteral("clearButton"));
    channel.saveLogButton->setIcon(testtools::ui::appIcon(testtools::ui::Icon::Save));
    channel.saveLogButton->setIconSize({20, 20});
    channel.clearReceiveButton->setIcon(testtools::ui::appIcon(testtools::ui::Icon::Clear));
    channel.clearReceiveButton->setIconSize({20, 20});
    channel.searchButton->setCheckable(true);
    channel.searchButton->setToolTip(QStringLiteral("搜索与过滤当前日志（Ctrl+F）"));
    receiveHeader->addWidget(channel.searchButton);
    receiveHeader->addWidget(channel.resetCounterButton);
    receiveHeader->addWidget(channel.saveLogButton);
    receiveHeader->addWidget(channel.clearReceiveButton);
    buildReceiveOptions(receiveHeader);
    channel.counterRow = new QWidget(terminalHeader);
    channel.counterRowLayout = new QHBoxLayout(channel.counterRow);
    channel.counterRowLayout->setContentsMargins(12, 0, 10, 4);
    channel.counterRowLayout->addStretch();
    channel.counterRow->hide();
    headerRows->addWidget(channel.counterRow);
    terminalLayout->addWidget(terminalHeader);

    channel.searchArea = new QWidget(terminalFrame);
    auto* searchLayout = new QHBoxLayout(channel.searchArea);
    searchLayout->setContentsMargins(12, 4, 10, 4);
    searchLayout->setSpacing(6);
    channel.searchEdit = new QLineEdit(channel.searchArea);
    channel.searchEdit->setPlaceholderText(QStringLiteral("搜索当前显示内容（区分大小写）"));
    channel.searchEdit->setClearButtonEnabled(true);
    channel.searchEdit->installEventFilter(this);
    channel.matchCountLabel = new QLabel(QStringLiteral("0 / 0"), channel.searchArea);
    channel.matchCountLabel->setObjectName(QStringLiteral("mutedLabel"));
    channel.previousMatchButton = new QPushButton(QStringLiteral("↑"), channel.searchArea);
    channel.nextMatchButton = new QPushButton(QStringLiteral("↓"), channel.searchArea);
    for (auto* button : {channel.previousMatchButton, channel.nextMatchButton}) {
        button->setObjectName(QStringLiteral("clearButton"));
        button->setFixedWidth(34);
        button->setEnabled(false);
    }
    channel.previousMatchButton->setToolTip(QStringLiteral("上一个匹配（Shift+F3）"));
    channel.nextMatchButton->setToolTip(QStringLiteral("下一个匹配（Enter / F3）"));
    channel.filterCheck = new QCheckBox(QStringLiteral("仅看匹配"), channel.searchArea);
    auto* closeSearch = new QPushButton(QStringLiteral("关闭"), channel.searchArea);
    closeSearch->setObjectName(QStringLiteral("clearButton"));
    searchLayout->addWidget(channel.searchEdit, 1);
    searchLayout->addWidget(channel.matchCountLabel);
    searchLayout->addWidget(channel.previousMatchButton);
    searchLayout->addWidget(channel.nextMatchButton);
    searchLayout->addWidget(channel.filterCheck);
    searchLayout->addWidget(closeSearch);
    channel.searchArea->hide();
    terminalLayout->addWidget(channel.searchArea);
    channel.searchTimer = new QTimer(this);
    channel.searchTimer->setSingleShot(true);
    channel.searchTimer->setInterval(75);
    connect(channel.searchTimer, &QTimer::timeout, this, [this, index] {
        auto& current = channels_[index];
        if (current.filteredRenderPending)
            renderReceiveLog(index);
        else
            updateSearchMatches(index);
    });
    connect(channel.searchButton, &QPushButton::toggled, this, [this, index](bool shown) {
        auto& current = channels_[index];
        current.searchArea->setVisible(shown);
        if (shown) {
            current.searchEdit->setFocus();
        } else {
            const QSignalBlocker queryBlock(current.searchEdit);
            const QSignalBlocker filterBlock(current.filterCheck);
            current.searchEdit->clear();
            current.filterCheck->setChecked(false);
            renderReceiveLog(index);
            focusTerminal();
        }
    });
    connect(closeSearch, &QPushButton::clicked, this, [this, index] {
        channels_[index].searchButton->setChecked(false);
    });
    connect(channel.searchEdit, &QLineEdit::textChanged, this, [this, index] {
        channels_[index].currentMatch = -1;
        if (channels_[index].filterCheck->isChecked())
            renderReceiveLog(index);
        else
            updateSearchMatches(index);
        findLogMatch(index, 1);
    });
    connect(channel.filterCheck, &QCheckBox::toggled, this, [this, index] {
        channels_[index].currentMatch = -1;
        renderReceiveLog(index);
        findLogMatch(index, 1);
    });
    connect(channel.searchEdit, &QLineEdit::returnPressed, this, [this, index] {
        findLogMatch(index, 1);
    });
    connect(channel.previousMatchButton, &QPushButton::clicked, this, [this, index] {
        findLogMatch(index, -1);
    });
    connect(channel.nextMatchButton, &QPushButton::clicked, this, [this, index] {
        findLogMatch(index, 1);
    });

    channel.receiveEdit = new ReceiveConsole(terminalFrame);
    channel.receiveEdit->setObjectName(QStringLiteral("receiveConsole"));
    channel.receiveEdit->setReadOnly(true);
    // SerialLogBuffer 已限制日志容量，视图层不再二次隐式裁剪行数。
    channel.receiveEdit->setMaximumBlockCount(0);
    channel.receiveEdit->installEventFilter(this);
    channel.receiveEdit->setPlaceholderText(
        QStringLiteral("等待数据；发送成功显示 TX，接收数据显示 RX"));
    setupReceiveInput(index);
    terminalLayout->addWidget(channel.receiveEdit, 1);
    logSendLayout->addWidget(terminalFrame, 1);

    buildSendArea(index, terminalHeader, receiveHeader, logSendLayout);

    connect(channel.sourceCombo, &QComboBox::currentIndexChanged, this, [this, index] {
        applyPorts(index, false);
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
    connect(
        channel.parityCombo, &QComboBox::currentIndexChanged, this, [this] { savePreferences(); });
    connect(channel.stopBitsCombo, &QComboBox::currentIndexChanged, this, [this] {
        savePreferences();
    });
    connect(channel.flowControlCombo, &QComboBox::currentIndexChanged, this, [this] {
        savePreferences();
    });
    connect(channel.refreshPortsButton, &QPushButton::clicked, this, [this, index] {
        if (shuttingDown_ || channelActive(index))
            return;
        if (channelUsesAgent(index)) {
            if (!scanning_ && !hasActiveAgentConnection() && selectedAgentIsOnline())
                refreshAgentPorts();
        } else {
            refreshLocalPorts(true);
        }
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
    channel.highlighter = new SerialKeywordHighlighter(channel.receiveEdit->document());
    connect(channel.highlightButton, &QPushButton::toggled, this, [this, index](bool enabled) {
        channels_[index].highlighter->setEnabled(enabled);
        savePreferences();
    });
    connect(channel.recordButton, &QPushButton::clicked, this, [this, index] {
        toggleRecording(index);
    });
    connect(channel.clearReceiveButton, &QPushButton::clicked, this, [this, index] {
        channels_[index].displayDroppedBytes = 0;
        clearReceive(index);
    });
    setChannelStatus(index, QStringLiteral("未连接"));
    return channel.group;
}

bool SerialEndpointWorkspace::setWorkspaceModeForDiagnostics(const QString& modeName)
{
    return selectWorkspaceMode(modeName);
}

void SerialEndpointWorkspace::refreshLocalPorts(bool announce)
{
    if (shuttingDown_)
        return;
    localPorts_ = testtools::SerialTransportService::localPorts();
    for (int index = 0; index < int(channels_.size()); ++index)
        if (!channelUsesAgent(index))
            applyPorts(index);
    if (announce)
        emit activityChanged(QStringLiteral("已刷新 PC端串口，共 %1 个").arg(localPorts_.size()));
    updateGlobalControls();
}

void SerialEndpointWorkspace::refreshAgentPorts()
{
    const auto generation = ++agentPortGeneration_;
    agentPorts_.clear();
    for (int index = 0; index < int(channels_.size()); ++index)
        if (channelUsesAgent(index))
            applyPorts(index);

    if (!selectedAgentIsOnline()) {
        scanning_ = false;
        updateGlobalControls();
        return;
    }

    scanning_ = true;
    emit activityChanged(
        QStringLiteral("正在读取 %1 的设备串口").arg(fromUtf8(currentAdbDevice_.serial)));
    updateGlobalControls();
    const std::uint16_t controlPort = agentControlPort_;
    QPointer<SerialEndpointWorkspace> guard(this);
    scanTasks_.start([guard, controlPort, generation] {
        std::string error;
        auto ports = testtools::SerialTransportService::agentPorts(controlPort, error);
        QMetaObject::invokeMethod(
            guard,
            [guard, generation, ports = std::move(ports), error = std::move(error)] {
                if (!guard || guard->shuttingDown_ || generation != guard->agentPortGeneration_) {
                    return;
                }
                guard->scanning_ = false;
                guard->agentPorts_ = std::move(ports);
                for (int index = 0; index < int(guard->channels_.size()); ++index)
                    if (guard->channelUsesAgent(index))
                        guard->applyPorts(index);
                if (!error.empty()) {
                    emit guard->activityChanged(fromUtf8(error));
                } else {
                    emit guard->activityChanged(
                        QStringLiteral("已发现 %1 个设备串口").arg(guard->agentPorts_.size()));
                }
                guard->updateGlobalControls();
            },
            Qt::QueuedConnection);
    });
}

void SerialEndpointWorkspace::setAdbDevice(const AdbDevice& device)
{
    // 本地 COM 连接已开启时仍允许切换当前选中的 ADB 设备上下文；
    // 活跃传输对象已各自持有独立的设备与配置快照。
    currentAdbDevice_ = device;
    refreshTargets();
    updateRouteSummary();
}

void SerialEndpointWorkspace::setServiceStatus(testtools::AgentServiceState state,
                                               const QString& detail,
                                               bool cleanupPending)
{
    if (serviceState_ == state && serviceDetail_ == detail &&
        serviceCleanupPending_ == cleanupPending)
        return;
    serviceCleanupPending_ = cleanupPending;
    serviceState_ = state;
    serviceDetail_ = detail;
    updateGlobalControls();
}

void SerialEndpointWorkspace::setAgentConnection(const std::string& adbSerial,
                                                 std::uint16_t localControlPort)
{
    if (agentAdbSerial_ != adbSerial || agentControlPort_ != localControlPort) {
        for (int i = 0; i < int(channels_.size()); ++i)
            if (channelUsesAgent(i) && channelActive(i))
                disconnectChannel(i);
    }
    agentAdbSerial_ = adbSerial;
    agentControlPort_ = localControlPort;
    if (hasActiveConnections() && !hasActiveAgentConnection())
        refreshAgentPorts();
    else
        refreshTargets();
    updateRouteSummary();
}

void SerialEndpointWorkspace::applyPorts(int index, bool preserveEditedEndpoint)
{
    auto& channel = channels_[index];
    if (!channel.portCombo || channelActive(index)) {
        return;
    }
    const QString previousData = selectedEndpoint(channel.portCombo);
    const QString previousText = channel.portCombo->currentText();
    const bool edited =
        previousText != channel.portCombo->itemText(channel.portCombo->currentIndex());
    const QString preferredEndpoint =
        fromUtf8(preferences_.channels[preferenceIndex(index)].endpoint);
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
            channel.portCombo->addItem(
                detail.isEmpty() ? endpoint : endpoint + QStringLiteral(" — ") + detail, endpoint);
        }
        channel.portCombo->setToolTip(QStringLiteral("当前连接设备提供的串口节点"));
    } else {
        for (const auto& port : localPorts_) {
            const QString name = fromUtf8(port.name);
            const QString description = fromUtf8(port.description);
            channel.portCombo->addItem(
                description.isEmpty() ? name : name + QStringLiteral(" — ") + description, name);
        }
        channel.portCombo->setToolTip(QStringLiteral("PC端 COM 口，例如 COM3"));
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
    } else if (preserveEditedEndpoint && edited && !previousText.trimmed().isEmpty()) {
        channel.portCombo->setEditText(previousText);
    } else if (!preferredEndpoint.isEmpty() && channel.portCombo->count() == 0) {
        channel.portCombo->setEditText(preferredEndpoint);
    }
    if (channel.portCombo->lineEdit()) {
        channel.portCombo->lineEdit()->setCursorPosition(0);
    }
    updateRouteSummary();
}

void SerialEndpointWorkspace::applyPreferencesToUi()
{
    applyingPreferences_ = true;
    for (int index = 0; index < int(channels_.size()); ++index) {
        auto& channel = channels_[static_cast<std::size_t>(index)];
        const auto& pref = preferences_.channels[preferenceIndex(index)];
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
        if (channel.lineEndingCombo)
            channel.lineEndingCombo->setCurrentIndex(std::clamp(pref.lineEnding, 0, 3));
        channel.receiveHexCheck->setChecked(pref.receiveHex);
        channel.timestampCheck->setChecked(pref.timestamp);
        channel.autoScrollCheck->setChecked(pref.autoScroll);
        if (channel.sendHexCheck)
            channel.sendHexCheck->setChecked(pref.sendHex);
        channel.highlightButton->setChecked(pref.highlightKeywords);
        if (!pref.endpoint.empty()) {
            channel.portCombo->setEditText(fromUtf8(pref.endpoint));
            if (channel.portCombo->lineEdit()) {
                channel.portCombo->lineEdit()->setCursorPosition(0);
            }
        }
    }
    applyingPreferences_ = false;
}

void SerialEndpointWorkspace::savePreferences()
{
    if (applyingPreferences_ || shuttingDown_ || preferencesStorePath_.empty()) {
        return;
    }
    for (int index = 0; index < int(channels_.size()); ++index) {
        const auto& channel = channels_[static_cast<std::size_t>(index)];
        auto& pref = preferences_.channels[preferenceIndex(index)];
        pref.useAgent = channelUsesAgent(index);
        const QString endpointText = selectedEndpoint(channel.portCombo);
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
        if (channel.lineEndingCombo)
            pref.lineEnding = channel.lineEndingCombo->currentIndex();
        pref.receiveHex = channel.receiveHexCheck->isChecked();
        pref.timestamp = channel.timestampCheck->isChecked();
        if (!isTerminal())
            pref.autoScroll = channel.autoScrollCheck->isChecked();
        if (channel.sendHexCheck)
            pref.sendHex = channel.sendHexCheck->isChecked();
        pref.highlightKeywords = channel.highlightButton->isChecked();
    }
    std::string ignoredError;
    testtools::SerialPreferencesStore::save(preferencesStorePath_, preferences_, ignoredError);
}

SerialConfig SerialEndpointWorkspace::channelConfig(int index) const
{
    const auto& channel = channels_[index];
    SerialConfig config;
    config.endpoint = toUtf8(selectedEndpoint(channel.portCombo));
    config.adbSerial = channelUsesAgent(index) ? currentAdbDevice_.serial : std::string{};
    config.allowAdbRoot = false;
    config.baudRate = channel.baudCombo->currentText().toInt();
    config.dataBits = channel.dataBitsCombo->currentText().toInt();
    config.parity = channel.parityCombo->currentIndex() == 1   ? Parity::Odd
                    : channel.parityCombo->currentIndex() == 2 ? Parity::Even
                                                               : Parity::None;
    config.stopBits = channel.stopBitsCombo->currentIndex() == 1 ? StopBits::Two : StopBits::One;
    config.flowControl = channel.flowControlCombo->currentIndex() == 1   ? FlowControl::Hardware
                         : channel.flowControlCombo->currentIndex() == 2 ? FlowControl::Software
                                                                         : FlowControl::None;
    return config;
}

bool SerialEndpointWorkspace::validateChannel(int index, QString& error) const
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
    auto conflict = testtools::SerialEndpointConflict::None;
    for (int peer = 0; peer < int(channels_.size()); ++peer) {
        if (peer != index) {
            const auto result = testtools::SerialWorkspaceState::endpointConflict(
                candidate, endpointSelection(peer));
            if (result != testtools::SerialEndpointConflict::None)
                conflict = result;
        }
    }
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

testtools::SerialEndpointSelection SerialEndpointWorkspace::endpointSelection(int index) const
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

void SerialEndpointWorkspace::connectChannel(int index)
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
    std::string claimError;
    const auto key = testtools::SerialPortClaims::resourceKey(
        agent, agentAdbSerial_, channelConfig(index).endpoint);
    channel.portClaim = portClaims_->acquire(key,
                                             isTerminal() ? "串口终端"
                                             : index == 0 ? "双端调试 A"
                                                          : "单/双串口调试 B",
                                             claimError);
    if (!channel.portClaim) {
        setChannelStatus(index, fromUtf8(claimError), true);
        emit activityChanged(fromUtf8(claimError));
        return;
    }
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
    streamReset(index);
    channel.session.open(std::move(transport), channelConfig(index));
    updateGlobalControls();
}

void SerialEndpointWorkspace::attachSessionCallbacks(int index)
{
    QPointer<SerialEndpointWorkspace> guard(this);
    const auto queue = channels_[index].receiveQueue;
    channels_[index].session.setDataCallback(
        [guard, index, queue](std::uint64_t generation, const ByteBuffer& data) {
            if (queue->push(generation, data))
                QMetaObject::invokeMethod(
                    guard,
                    [guard, index] {
                        if (guard && !guard->shuttingDown_)
                            guard->drainReceived(index);
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
                        if (channel.sendButton)
                            channel.sendButton->setEnabled(true);
                    } else if (!progress) {
                        if (!channel.session.isOpen()) {
                            guard->disconnectChannel(index, false);
                            guard->setChannelStatus(index, status, error);
                        }
                    }
                    guard->updateGlobalControls();
                    if (connected)
                        guard->focusTerminal();
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
                    guard->sendFailed(index);
                    emit guard->activityChanged(
                        scheduledSendStopped
                            ? QStringLiteral("%1 发送失败；计划发送已停止")
                                  .arg(guard->channelDisplayName(index))
                            : QStringLiteral("%1 发送失败").arg(guard->channelDisplayName(index)));

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

void SerialEndpointWorkspace::drainReceived(int index)
{
    auto& channel = channels_[index];
    const auto batch = channel.receiveQueue->take();
    if (batch.generation == channel.session.generation()) {
        if (channel.displayGeneration != batch.generation) {
            channel.displayGeneration = batch.generation;
            channel.displayDroppedBytes = 0;
        }
        if (batch.droppedBytes) {
            channel.displayDroppedBytes += batch.droppedBytes;
            streamReset(index);
            displayGap(index);
            emit activityChanged(
                QStringLiteral("%1 显示积压，已跳过 %2；录制状态请查看录制入口")
                    .arg(channelDisplayName(index), recordingSize(batch.droppedBytes)));
        }
        channel.receivingBatch = true;
        channel.receivedAppend = {};
        for (const auto& bytes : batch.chunks)
            appendReceived(index, bytes);
        channel.receivingBatch = false;
        applyLogAppendResult(index, channel.receivedAppend);
        updateTruncation(index);
        updateCounter(index);
    }
    if (batch.more)
        QTimer::singleShot(0, this, [this, index] {
            if (!shuttingDown_)
                drainReceived(index);
        });
}

void SerialEndpointWorkspace::updateTruncation(int index)
{
    auto& channel = channels_[index];
    channel.truncatedLabel->setVisible(channel.displayDroppedBytes || historyTruncated(index));
    channel.truncatedLabel->setToolTip(
        channel.displayDroppedBytes
            ? QStringLiteral("接收显示队列已跳过 "
                             "%1；显示与导出可能不连续。独立录制不经过此队列，请查看其完成状态。")
                  .arg(recordingSize(channel.displayDroppedBytes))
            : QStringLiteral("显示缓冲已淘汰最早记录；保存当前日志只包含保留的显示内容"));
}

void SerialEndpointWorkspace::toggleRecording(int index)
{
    auto& session = channels_[index].session;
    const auto state = session.recordingSnapshot();
    if (state.busy) {
        session.stopRecording("用户停止", state.id);
        pollRecordings();
        return;
    }
    if (!session.isOpen() || shuttingDown_)
        return;
    const auto generation = session.generation();
    const QString textFilter = QStringLiteral("带标记文本 (*.log)");
    QString selected = textFilter;
    const auto filename = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("持续录制串口数据"),
        QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
            .filePath(
                QStringLiteral("serial-%1.log")
                    .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")))),
        textFilter + QStringLiteral(";;原始 RX 字节 (*.bin)"),
        &selected,
        QFileDialog::DontConfirmOverwrite);
    if (filename.isEmpty())
        return;
    testtools::SerialRecordOptions options;
    options.path = std::filesystem::path(filename.toStdWString());
    options.format = selected == textFilter ? testtools::SerialRecordFormat::TaggedText
                                            : testtools::SerialRecordFormat::RawRx;
    const QString metadata =
        QString::fromStdWString(testtools::SerialRecorder::metadataPath(options.path).wstring());
    if (QFile::exists(filename) || QFile::exists(metadata)) {
        if (QMessageBox::question(
                this,
                QStringLiteral("覆盖录制文件"),
                QStringLiteral("将覆盖以下录制文件和元数据：\n%1\n%2").arg(filename, metadata),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No) != QMessageBox::Yes)
            return;
        options.overwrite = true;
    }
    std::string error;
    if (!session.startRecording(options, generation, error))
        emit activityChanged(
            QStringLiteral("%1：%2").arg(channelDisplayName(index), fromUtf8(error)));
    pollRecordings();
}

void SerialEndpointWorkspace::pollRecordings()
{
    for (int index = 0; index < int(channels_.size()); ++index) {
        auto& channel = channels_[index];
        const auto state = channel.session.recordingSnapshot();
        using State = testtools::SerialRecordState;
        const bool stopping =
            state.busy && state.state != State::Starting && state.state != State::Recording;
        channel.recordButton->setText(
            stopping ? QStringLiteral("正在保存…")
            : state.busy
                ? QStringLiteral("■ 停止录制 · %1").arg(recordingSize(state.writtenFileBytes))
            : state.state == State::Failed ? QStringLiteral("录制失败 · 重试…")
                                           : QStringLiteral("录制…"));
        channel.recordButton->setEnabled(!shuttingDown_ && !stopping &&
                                         (state.busy || channel.session.isOpen()));
        const auto visualState = state.state == State::Failed ? QStringLiteral("failed")
                                 : state.busy                 ? QStringLiteral("recording")
                                                              : QStringLiteral("idle");
        if (channel.recordButton->property("recordState").toString() != visualState) {
            channel.recordButton->setProperty("recordState", visualState);
            channel.recordButton->style()->unpolish(channel.recordButton);
            channel.recordButton->style()->polish(channel.recordButton);
        }
        const auto path = QString::fromStdWString(state.path.wstring());
        channel.recordButton->setToolTip(
            state.id ? QStringLiteral("%1\n%2 · 已写 %3 · 待写 %4%5")
                           .arg(path,
                                state.state == State::Starting
                                    ? QStringLiteral("准备文件，已开始缓存数据")
                                : stopping   ? QStringLiteral("正在保存已接受数据")
                                : state.busy ? QStringLiteral("正在录制")
                                : state.state == State::Completed ? QStringLiteral("已完成")
                                                                  : QStringLiteral("录制失败"),
                                recordingSize(state.writtenFileBytes),
                                recordingSize(state.pendingBytes),
                                state.error.empty() ? QString()
                                                    : QStringLiteral("\n") + fromUtf8(state.error))
                     : QStringLiteral("持续录制到独立文件；文本包含 RX/TX，原始格式只保存 RX"));
        const auto announcement = QStringLiteral("%1/%2/%3/%4")
                                      .arg(state.id)
                                      .arg(int(state.state))
                                      .arg(state.busy)
                                      .arg(fromUtf8(state.error));
        if (state.id && announcement != channel.recordAnnouncement) {
            channel.recordAnnouncement = announcement;
            const QString message = !state.error.empty() ? fromUtf8(state.error)
                                    : stopping           ? QStringLiteral("正在保存录制…")
                                    : state.state == State::Starting
                                        ? QStringLiteral("正在准备录制…")
                                    : state.busy ? QStringLiteral("正在录制")
                                                 : QStringLiteral("录制已完成");
            emit activityChanged(
                QStringLiteral("%1 · %2 · %3").arg(channelDisplayName(index), message, path));
        }
    }
}

std::vector<SerialEndpointWorkspace::BackgroundTask> SerialEndpointWorkspace::activeTasks() const
{
    std::vector<BackgroundTask> result;
    const auto prefix = isTerminal() ? QStringLiteral("串口终端") : QStringLiteral("串口调试");
    for (int index = 0; index < int(channels_.size()); ++index) {
        const auto& session = channels_[index].session;
        const auto record = session.recordingSnapshot();
        const auto state = session.snapshot();
        if (record.busy) {
            const bool saving = record.state != testtools::SerialRecordState::Starting &&
                                record.state != testtools::SerialRecordState::Recording;
            result.push_back(
                {BackgroundTask::Kind::Recording,
                 index,
                 record.id,
                 QStringLiteral("%1 · %2 · %3 · %4")
                     .arg(prefix,
                          channelDisplayName(index),
                          saving ? QStringLiteral("录制保存中") : QStringLiteral("录制"),
                          recordingSize(record.writtenFileBytes)),
                 !saving});
        }
        if (state.scheduledSend.running)
            result.push_back({BackgroundTask::Kind::ScheduledSend,
                              index,
                              state.scheduledSend.id,
                              QStringLiteral("%1 · %2 · 计划发送 · 已提交 %3 次")
                                  .arg(prefix, channelDisplayName(index))
                                  .arg(state.scheduledSend.submitted)});
    }
    return result;
}

void SerialEndpointWorkspace::stopTask(const BackgroundTask& task)
{
    if (task.channel < 0 || task.channel >= int(channels_.size()) || shuttingDown_)
        return;
    auto& session = channels_[task.channel].session;
    if (task.kind == BackgroundTask::Kind::Recording)
        session.stopRecording("用户停止", task.token);
    pollRecordings();
    updateGlobalControls();
}

void SerialEndpointWorkspace::stopRecordings()
{
    for (auto& channel : channels_)
        channel.session.stopRecording("应用退出");
}

bool SerialEndpointWorkspace::hasPendingRecordings() const
{
    for (const auto& channel : channels_)
        if (channel.session.recordingSnapshot().busy)
            return true;
    return false;
}

void SerialEndpointWorkspace::disconnectChannel(int index, bool showClosedStatus)
{
    auto& channel = channels_[index];
    const auto snapshot = channel.session.snapshot();
    if (channel.disconnectPending ||
        snapshot.state == testtools::SerialSessionState::Disconnecting) {
        return;
    }
    channelDisconnected(index);

    if (!snapshot.active) {
        completeChannelDisconnect(index, channel.session.generation(), showClosedStatus);
        return;
    }

    channel.connectButton->setText(QStringLiteral("正在断开…"));
    channel.connectButton->setEnabled(false);
    if (channel.sendButton)
        channel.sendButton->setEnabled(false);
    channel.disconnectPending = true;
    setChannelControlsEnabled(index, false);
    if (showClosedStatus) {
        setChannelStatus(index, QStringLiteral("正在断开..."));
    }
    QPointer<SerialEndpointWorkspace> guard(this);
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
    streamReset(index);
    updateGlobalControls();
}

void SerialEndpointWorkspace::completeChannelDisconnect(int index,
                                                        std::uint64_t generation,
                                                        bool showClosedStatus)
{
    auto& channel = channels_[index];
    if (channel.session.generation() != generation || channel.session.isActive()) {
        return;
    }
    channel.disconnectPending = false;
    channel.portClaim.reset();
    channel.controlPending = false;
    channel.lineStates = {Qt::PartiallyChecked, Qt::PartiallyChecked};
    for (auto* control : {channel.dtrCheck, channel.rtsCheck}) {
        const QSignalBlocker blocker(control);
        control->setCheckState(Qt::PartiallyChecked);
    }
    channel.connectButton->setText(QStringLiteral("连接"));
    if (channel.sendButton)
        channel.sendButton->setEnabled(false);
    setChannelControlsEnabled(index, true);
    if (showClosedStatus) {
        setChannelStatus(index, QStringLiteral("已断开"));
    }
    updateGlobalControls();
}

void SerialEndpointWorkspace::setChannelControlsEnabled(int index, bool enabled)
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

void SerialEndpointWorkspace::setChannelStatus(int index, const QString& message, bool error)
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

QString SerialEndpointWorkspace::channelDisplayName(int index) const
{
    if (singleChannelView() && index == primaryChannelIndex()) {
        return QStringLiteral("当前串口");
    }
    return channelName(index);
}

bool SerialEndpointWorkspace::channelActive(int index) const
{
    return channels_[index].disconnectPending || channels_[index].session.isActive();
}

bool SerialEndpointWorkspace::sendBytes(int index, const ByteBuffer& data, bool echo)
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

void SerialEndpointWorkspace::setControlLine(int index, SerialControlLine line, bool enabled)
{
    auto& channel = channels_[index];
    auto* check = line == SerialControlLine::Dtr ? channel.dtrCheck : channel.rtsCheck;
    check->setTristate(false);
    const auto generation = channel.session.generation();
    channel.controlPending = true;
    updateGlobalControls();
    QPointer<SerialEndpointWorkspace> guard(this);
    auto* session = &channel.session;
    controlTasks_.start([guard, session, index, generation, line, enabled] {
        std::string error;
        const bool succeeded = session->setControlLine(generation, line, enabled, error);
        QMetaObject::invokeMethod(
            guard,
            [guard, index, generation, line, enabled, succeeded, error] {
                if (!guard || guard->shuttingDown_ ||
                    guard->channels_[index].session.generation() != generation)
                    return;
                auto& current = guard->channels_[index];
                const int lineIndex = line == SerialControlLine::Dtr ? 0 : 1;
                auto* control = lineIndex == 0 ? current.dtrCheck : current.rtsCheck;
                if (succeeded)
                    current.lineStates[lineIndex] = enabled ? Qt::Checked : Qt::Unchecked;
                const QSignalBlocker blocker(control);
                control->setCheckState(current.lineStates[lineIndex]);
                current.controlPending = false;
                guard->updateGlobalControls();
                emit guard->activityChanged(
                    succeeded
                        ? QStringLiteral("%1：%2 已%3（驱动已接受）")
                              .arg(guard->channelDisplayName(index),
                                   lineIndex == 0 ? QStringLiteral("DTR") : QStringLiteral("RTS"),
                                   enabled ? QStringLiteral("置位") : QStringLiteral("清除"))
                        : QStringLiteral("控制线未修改：%1").arg(fromUtf8(error)));
            },
            Qt::QueuedConnection);
    });
}

void SerialEndpointWorkspace::appendTransmitted(int index, const ByteBuffer& data)
{
    if (data.empty()) {
        return;
    }
    auto& channel = channels_[index];
    const auto now = QDateTime::currentDateTime();
    const auto result = channel.logBuffer.appendTx(
        data, now.toMSecsSinceEpoch(), toUtf8(now.toString(QStringLiteral("HH:mm:ss.zzz"))));
    applyLogAppendResult(index, result);
}

void SerialEndpointWorkspace::appendNotice(int index, const QString& message)
{
    auto& channel = channels_[index];
    const auto now = QDateTime::currentDateTime();
    const auto result =
        channel.logBuffer.appendNotice(toUtf8(message),
                                       now.toMSecsSinceEpoch(),
                                       toUtf8(now.toString(QStringLiteral("HH:mm:ss.zzz"))));
    applyLogAppendResult(index, result);
}

void SerialEndpointWorkspace::scheduleSearchRefresh(int index)
{
    auto& channel = channels_[index];
    if (channel.searchEdit->text().isEmpty())
        return;
    channel.filteredRenderPending = channel.filterCheck->isChecked();
    if (!channel.searchTimer->isActive())
        channel.searchTimer->start();
}

void SerialEndpointWorkspace::updateSearchMatches(int index)
{
    auto& channel = channels_[index];
    channel.searchMatches.clear();
    const QString query = channel.searchEdit->text();
    if (!query.isEmpty()) {
        const QString text = channel.receiveEdit->toPlainText();
        qsizetype position = 0;
        while ((position = text.indexOf(query, position, Qt::CaseSensitive)) >= 0) {
            channel.searchMatches.push_back(static_cast<int>(position));
            position += query.size();
        }
    }
    const int count = static_cast<int>(channel.searchMatches.size());
    channel.currentMatch = std::min(channel.currentMatch, count - 1);
    channel.matchCountLabel->setText(
        QStringLiteral("%1 / %2").arg(channel.currentMatch + 1).arg(count));
    channel.previousMatchButton->setEnabled(count > 0);
    channel.nextMatchButton->setEnabled(count > 0);
    QList<QTextEdit::ExtraSelection> selections;
    if (channel.currentMatch >= 0) {
        QTextEdit::ExtraSelection match;
        match.cursor = QTextCursor(channel.receiveEdit->document());
        match.cursor.setPosition(
            channel.searchMatches[static_cast<std::size_t>(channel.currentMatch)]);
        match.cursor.setPosition(match.cursor.position() + static_cast<int>(query.size()),
                                 QTextCursor::KeepAnchor);
        match.format.setBackground(QColor(QStringLiteral("#f0d8c8")));
        match.format.setForeground(QColor(QStringLiteral("#302f2d")));
        selections.push_back(match);
    }
    channel.receiveEdit->setExtraSelections(selections);
}

void SerialEndpointWorkspace::findLogMatch(int index, int step)
{
    auto& channel = channels_[index];
    if (channel.filteredRenderPending)
        renderReceiveLog(index);
    else
        updateSearchMatches(index);
    const int count = static_cast<int>(channel.searchMatches.size());
    if (count == 0)
        return;
    channel.currentMatch = channel.currentMatch < 0 ? (step > 0 ? 0 : count - 1)
                                                    : (channel.currentMatch + step + count) % count;
    channel.autoScrollCheck->setChecked(false);
    QTextCursor cursor(channel.receiveEdit->document());
    cursor.setPosition(channel.searchMatches[static_cast<std::size_t>(channel.currentMatch)]);
    cursor.setPosition(cursor.position() + static_cast<int>(channel.searchEdit->text().size()),
                       QTextCursor::KeepAnchor);
    channel.receiveEdit->setTextCursor(cursor);
    channel.receiveEdit->ensureCursorVisible();
    updateSearchMatches(index);
}

void SerialEndpointWorkspace::renderReceiveLog(int index)
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

    channel.filteredRenderPending = false;
    const auto filter =
        channel.filterCheck->isChecked() ? toUtf8(channel.searchEdit->text()) : std::string{};
    edit->clear();
    QTextCursor cursor(edit->document());
    cursor.beginEditBlock();
    for (const auto& entry : channel.logBuffer.entries()) {
        const auto text = testtools::SerialLogBuffer::formatEntry(entry, hex, showTimestamp);
        if (filter.empty() || text.find(filter) != std::string::npos) {
            cursor.insertText(fromUtf8(text), logEntryFormat(entry.direction));
        }
    }
    cursor.endEditBlock();
    updateTruncation(index);
    updateSearchMatches(index);
    if (vbar) {
        if (autoScroll || atBottom) {
            vbar->setValue(vbar->maximum());
        } else {
            vbar->setValue(std::min(previousScroll, vbar->maximum()));
        }
    }
}

void SerialEndpointWorkspace::applyLogAppendResult(int index,
                                                   const testtools::SerialLogAppendResult& result)
{
    auto& batch = channels_[index];
    if (batch.receivingBatch) {
        batch.receivedAppend.addedEntries += result.addedEntries;
        batch.receivedAppend.trimmedOldest =
            batch.receivedAppend.trimmedOldest || result.trimmedOldest;
        return;
    }
    auto& current = channels_[index];
    updateTruncation(index);
    scheduleSearchRefresh(index);
    if (current.filterCheck->isChecked() && !current.searchEdit->text().isEmpty()) {
        return; // 过滤模式下由单次定时器合并刷新突发日志，底层原始缓冲已先行更新。
    }
    if (result.addedEntries == 0) {
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
    const std::size_t startAdded =
        entries.size() >= result.addedEntries ? (entries.size() - result.addedEntries) : 0;
    for (std::size_t i = startAdded; i < entries.size(); ++i) {
        const QString line =
            fromUtf8(testtools::SerialLogBuffer::formatEntry(entries[i], hex, showTimestamp));
        cursor.movePosition(QTextCursor::End);
        cursor.insertText(line, logEntryFormat(entries[i].direction));
    }
    if (vbar) {
        if (autoScroll) {
            vbar->setValue(vbar->maximum());
        } else {
            vbar->setValue(previousScroll);
        }
    }
}

void SerialEndpointWorkspace::updateCounter(int index)
{
    auto& channel = channels_[index];
    if (channel.receivingBatch)
        return;
    if (!channel.counterLabel) {
        return;
    }
    const auto snapshot = channel.session.snapshot();
    if (singleChannelView()) {
        channel.counterLabel->setText(QStringLiteral("RX: %1 B  |  TX: %2 B")
                                          .arg(snapshot.receivedBytes)
                                          .arg(snapshot.sentBytes));
    } else {
        channel.counterLabel->setText(
            QStringLiteral("RX %1 · TX %2 B").arg(snapshot.receivedBytes).arg(snapshot.sentBytes));
    }
}

void SerialEndpointWorkspace::resetChannelCounters(int index)
{
    channels_[index].session.resetCounters();
    updateCounter(index);
    emit activityChanged(QStringLiteral("%1 的 RX / TX 计数已清零").arg(channelDisplayName(index)));
}

void SerialEndpointWorkspace::saveChannelLog(int index)
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
            .arg(!isTerminal() && index == 0 ? QStringLiteral("A") : QStringLiteral("B"),
                 QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
    const QString filePath =
        QFileDialog::getSaveFileName(this,
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
        QMessageBox::warning(this,
                             QStringLiteral("保存失败"),
                             QStringLiteral("写入日志文件不完整：%1").arg(filePath));
        return;
    }
    file.close();
    emit activityChanged(QStringLiteral("已保存收发日志：%1").arg(filePath));
}

bool SerialEndpointWorkspace::hasActiveAgentConnection(const std::string& serial) const
{
    return !serial.empty() && agentAdbSerial_ == serial && hasActiveAgentConnection();
}

void SerialEndpointWorkspace::setDeviceMaintenance(bool blocked)
{
    if (maintenanceBlocked_ == blocked)
        return;
    maintenanceBlocked_ = blocked;
    if (blocked) {
        ++agentPortGeneration_;
        scanning_ = false;
    }
    updateGlobalControls();
}

bool SerialEndpointWorkspace::channelUsesAgent(int index) const
{
    return channels_[index].sourceCombo->currentData().toBool();
}

bool SerialEndpointWorkspace::selectedAgentIsOnline() const
{
    return !maintenanceBlocked_ && currentAdbDevice_.state == "device" &&
           !currentAdbDevice_.serial.empty() && agentControlPort_ != 0 &&
           agentAdbSerial_ == currentAdbDevice_.serial;
}
