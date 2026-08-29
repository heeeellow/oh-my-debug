#include "serial_workspace.h"

#include "core/agent_serial_transport.h"
#include "core/codec.h"
#include "core/cserialport_transport.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QMetaObject>
#include <QtCore/QPointer>
#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
#include <QtGui/QTextCursor>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QStyle>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <thread>
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
    buildUi();
    refreshTargets();
}

SerialWorkspace::~SerialWorkspace()
{
    prepareForShutdown();
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
    for (auto &thread : scanThreads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    scanThreads_.clear();
    QCoreApplication::removePostedEvents(this);
}

void SerialWorkspace::buildUi()
{
    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(12);

    auto *linkBar = new QFrame(this);
    linkBar->setObjectName(QStringLiteral("linkBar"));
    auto *linkLayout = new QHBoxLayout(linkBar);
    linkLayout->setContentsMargins(16, 11, 16, 11);
    auto *routeArea = new QVBoxLayout;
    routeArea->setSpacing(2);
    routeLabel_ = new QLabel(QStringLiteral("A  未选择   ⇄   B  未选择"), linkBar);
    routeLabel_->setObjectName(QStringLiteral("routeSummary"));
    testResultLabel_ = new QLabel(QStringLiteral("链路验证 · 未运行"), linkBar);
    testResultLabel_->setObjectName(QStringLiteral("testStatusNeutral"));
    stressResultLabel_ = new QLabel(QStringLiteral("可靠性压测 · 未运行"), linkBar);
    stressResultLabel_->setObjectName(QStringLiteral("testStatusNeutral"));
    stressResultLabel_->setToolTip(
        QStringLiteral("BER 统计已成功解析帧的有效载荷；丢帧和格式错误单独显示"));
    routeArea->addWidget(routeLabel_);
    routeArea->addWidget(testResultLabel_);
    routeArea->addWidget(stressResultLabel_);
    linkLayout->addLayout(routeArea, 1);
    connectBothButton_ = new QPushButton(QStringLiteral("连接链路"), linkBar);
    connectBothButton_->setObjectName(QStringLiteral("primaryButton"));
    disconnectBothButton_ = new QPushButton(QStringLiteral("断开"), linkBar);
    bidirectionalTestButton_ = new QPushButton(QStringLiteral("双向验证"), linkBar);
    bidirectionalTestButton_->setObjectName(QStringLiteral("verifyButton"));
    stressTestButton_ = new QPushButton(QStringLiteral("可靠性压测"), linkBar);
    stressTestButton_->setObjectName(QStringLiteral("verifyButton"));
    linkLayout->addWidget(connectBothButton_);
    linkLayout->addWidget(disconnectBothButton_);
    linkLayout->addWidget(bidirectionalTestButton_);
    linkLayout->addWidget(stressTestButton_);
    rootLayout->addWidget(linkBar);

    auto *channelSplitter = new QSplitter(Qt::Horizontal, this);
    channelSplitter->setChildrenCollapsible(false);
    channelSplitter->addWidget(buildChannelPanel(0));
    channelSplitter->addWidget(buildChannelPanel(1));
    channelSplitter->setStretchFactor(0, 1);
    channelSplitter->setStretchFactor(1, 1);
    rootLayout->addWidget(channelSplitter, 1);

    connect(connectBothButton_, &QPushButton::clicked, this, [this] {
        QString error;
        for (int index = 0; index < 2; ++index) {
            if (!channels_[index].transport && !validateChannel(index, error)) {
                QMessageBox::warning(this, QStringLiteral("无法连接两路"), error);
                return;
            }
        }
        if (!channels_[0].transport && !channels_[1].transport) {
            const auto configA = channelConfig(0);
            const auto configB = channelConfig(1);
            if (channelUsesAgent(0) && channelUsesAgent(1)) {
                QMessageBox::information(
                    this, QStringLiteral("当前为单板串口会话"),
                    QStringLiteral("当前 Agent 版本一次管理一个板端串口。可使用一路 Windows 串口和一路板端 Agent 串口；多板端会话会在后续扩展。"));
                return;
            }
            if (channelUsesAgent(0) == channelUsesAgent(1)
                && configA.endpoint == configB.endpoint
                && (!channelUsesAgent(0) || configA.adbSerial == configB.adbSerial)) {
                QMessageBox::warning(this, QStringLiteral("无法连接两路"),
                                     QStringLiteral("端点 A 和 B 不能同时打开同一个串口。"));
                return;
            }
        }
        for (int index = 0; index < 2; ++index) {
            if (!channels_[index].transport) {
                connectChannel(index);
            }
        }
    });
    connect(disconnectBothButton_, &QPushButton::clicked, this, &SerialWorkspace::disconnectAll);
    connect(bidirectionalTestButton_, &QPushButton::clicked, this, &SerialWorkspace::runBidirectionalTest);
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
    updateGlobalControls();
}

QWidget *SerialWorkspace::buildChannelPanel(int index)
{
    auto &channel = channels_[index];
    channel.group = new QGroupBox(this);
    channel.group->setObjectName(QStringLiteral("endpointCard"));
    auto *layout = new QVBoxLayout(channel.group);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(9);

    auto *header = new QHBoxLayout;
    auto *badge = new QLabel(index == 0 ? QStringLiteral("A") : QStringLiteral("B"), channel.group);
    badge->setAlignment(Qt::AlignCenter);
    badge->setFixedSize(24, 24);
    badge->setObjectName(index == 0 ? QStringLiteral("endpointBadgeA")
                                    : QStringLiteral("endpointBadgeB"));
    auto *endpointTitle = new QLabel(channelName(index), channel.group);
    endpointTitle->setObjectName(QStringLiteral("endpointTitle"));
    channel.statusLabel = new QLabel(QStringLiteral("未连接"), channel.group);
    channel.statusLabel->setObjectName(QStringLiteral("connectionStatusIdle"));
    channel.statusLabel->setAlignment(Qt::AlignCenter);
    channel.statusLabel->setMinimumWidth(72);
    channel.connectButton = new QPushButton(QStringLiteral("连接"), channel.group);
    channel.connectButton->setMinimumWidth(82);
    header->addWidget(badge);
    header->addWidget(endpointTitle);
    header->addStretch();
    header->addWidget(channel.statusLabel);
    header->addWidget(channel.connectButton);
    layout->addLayout(header);

    auto *settings = new QGridLayout;
    settings->setHorizontalSpacing(8);
    settings->setVerticalSpacing(7);
    channel.sourceCombo = new QComboBox(channel.group);
    channel.sourceCombo->addItem(QStringLiteral("Windows 本地串口"), false);
    channel.sourceCombo->addItem(QStringLiteral("板端 Agent 串口"), true);
    if (index == 1) {
        channel.sourceCombo->setCurrentIndex(1);
    }
    channel.portCombo = new QComboBox(channel.group);
    channel.portCombo->setEditable(true);
    channel.portCombo->setMinimumContentsLength(18);
    channel.baudCombo = new QComboBox(channel.group);
    channel.baudCombo->setEditable(true);
    channel.baudCombo->addItems({QStringLiteral("9600"), QStringLiteral("19200"),
                                 QStringLiteral("38400"), QStringLiteral("57600"),
                                 QStringLiteral("115200"), QStringLiteral("921600")});
    channel.baudCombo->setMinimumWidth(88);
    channel.baudCombo->setCurrentText(QStringLiteral("115200"));
    channel.baudCombo->lineEdit()->setCursorPosition(0);

    const auto fieldLabel = [&channel](const QString &text) {
        auto *label = new QLabel(text, channel.group);
        label->setObjectName(QStringLiteral("fieldLabel"));
        return label;
    };
    settings->addWidget(fieldLabel(QStringLiteral("接口")), 0, 0);
    settings->addWidget(channel.sourceCombo, 0, 1);
    settings->addWidget(fieldLabel(QStringLiteral("串口")), 0, 2);
    settings->addWidget(channel.portCombo, 0, 3, 1, 2);
    settings->addWidget(fieldLabel(QStringLiteral("波特率")), 0, 5);
    settings->addWidget(channel.baudCombo, 0, 6);

    channel.dataBitsCombo = new QComboBox(channel.group);
    channel.dataBitsCombo->addItems({QStringLiteral("5"), QStringLiteral("6"), QStringLiteral("7"), QStringLiteral("8")});
    channel.dataBitsCombo->setCurrentText(QStringLiteral("8"));
    channel.parityCombo = new QComboBox(channel.group);
    channel.parityCombo->addItems({QStringLiteral("无校验"), QStringLiteral("奇校验"), QStringLiteral("偶校验")});
    channel.stopBitsCombo = new QComboBox(channel.group);
    channel.stopBitsCombo->addItems({QStringLiteral("1 停止位"), QStringLiteral("2 停止位")});
    channel.flowControlCombo = new QComboBox(channel.group);
    channel.flowControlCombo->addItems({QStringLiteral("无流控"), QStringLiteral("硬件流控"), QStringLiteral("软件流控")});
    settings->addWidget(fieldLabel(QStringLiteral("格式")), 1, 0);
    settings->addWidget(channel.dataBitsCombo, 1, 1);
    settings->addWidget(channel.parityCombo, 1, 2, 1, 2);
    settings->addWidget(channel.stopBitsCombo, 1, 4);
    settings->addWidget(channel.flowControlCombo, 1, 5, 1, 2);
    settings->setColumnStretch(1, 1);
    settings->setColumnStretch(3, 2);
    layout->addLayout(settings);

    channel.counterLabel = new QLabel(QStringLiteral("RX: 0 B  TX: 0 B"), channel.group);
    channel.counterLabel->setObjectName(QStringLiteral("mutedLabel"));
    auto *receiveHeader = new QHBoxLayout;
    auto *receiveTitle = new QLabel(QStringLiteral("收发监视"), channel.group);
    receiveTitle->setObjectName(QStringLiteral("sectionLabel"));
    receiveHeader->addWidget(receiveTitle);
    receiveHeader->addStretch();
    receiveHeader->addWidget(channel.counterLabel);
    layout->addLayout(receiveHeader);

    channel.receiveEdit = new QPlainTextEdit(channel.group);
    channel.receiveEdit->setObjectName(QStringLiteral("receiveConsole"));
    channel.receiveEdit->setReadOnly(true);
    channel.receiveEdit->setMaximumBlockCount(5000);
    channel.receiveEdit->setPlaceholderText(QStringLiteral("该端点发送和接收到的数据"));
    layout->addWidget(channel.receiveEdit, 1);

    auto *receiveTools = new QHBoxLayout;
    channel.receiveHexCheck = new QCheckBox(QStringLiteral("HEX 显示"), channel.group);
    channel.timestampCheck = new QCheckBox(QStringLiteral("时间戳"), channel.group);
    auto *clearReceive = new QPushButton(QStringLiteral("清空监视"), channel.group);
    receiveTools->addWidget(channel.receiveHexCheck);
    receiveTools->addWidget(channel.timestampCheck);
    receiveTools->addStretch();
    receiveTools->addWidget(clearReceive);
    layout->addLayout(receiveTools);

    auto *sendTitle = new QLabel(QStringLiteral("发送数据"), channel.group);
    sendTitle->setObjectName(QStringLiteral("sectionLabel"));
    layout->addWidget(sendTitle);
    channel.sendEdit = new QPlainTextEdit(channel.group);
    channel.sendEdit->setObjectName(QStringLiteral("sendEditor"));
    channel.sendEdit->setMaximumHeight(72);
    channel.sendEdit->setPlaceholderText(QStringLiteral("输入该端点要发送的数据"));
    layout->addWidget(channel.sendEdit);

    auto *sendTools = new QHBoxLayout;
    channel.sendHexCheck = new QCheckBox(QStringLiteral("HEX 发送"), channel.group);
    channel.lineEndingCombo = new QComboBox(channel.group);
    channel.lineEndingCombo->addItems({QStringLiteral("不追加"), QStringLiteral("LF"),
                                       QStringLiteral("CR"), QStringLiteral("CRLF")});
    auto *clearSend = new QPushButton(QStringLiteral("清空发送"), channel.group);
    clearReceive->setObjectName(QStringLiteral("ghostButton"));
    clearSend->setObjectName(QStringLiteral("ghostButton"));
    channel.sendButton = new QPushButton(QStringLiteral("发送"), channel.group);
    channel.sendButton->setObjectName(QStringLiteral("primaryButton"));
    channel.sendButton->setEnabled(false);
    channel.autoSendButton = new QPushButton(QStringLiteral("定时 / 次数…"), channel.group);
    channel.autoSendButton->setEnabled(false);
    sendTools->addWidget(channel.sendHexCheck);
    sendTools->addWidget(new QLabel(QStringLiteral("行尾"), channel.group));
    sendTools->addWidget(channel.lineEndingCombo);
    sendTools->addStretch();
    sendTools->addWidget(clearSend);
    sendTools->addWidget(channel.autoSendButton);
    sendTools->addWidget(channel.sendButton);
    layout->addLayout(sendTools);

    connect(channel.sourceCombo, &QComboBox::currentIndexChanged, this, [this, index] {
        applyPorts(index);
        updateRouteSummary();
        updateGlobalControls();
    });
    connect(channel.portCombo, &QComboBox::currentTextChanged, this, [this] {
        updateRouteSummary();
        updateGlobalControls();
    });
    connect(channel.baudCombo, &QComboBox::currentTextChanged, this, [this] { updateGlobalControls(); });
    connect(channel.connectButton, &QPushButton::clicked, this, [this, index] {
        if (channels_[index].transport) {
            disconnectChannel(index);
        } else {
            connectChannel(index);
        }
    });
    connect(channel.sendButton, &QPushButton::clicked, this, [this, index] { sendManual(index); });
    connect(channel.autoSendButton, &QPushButton::clicked, this, [this, index] {
        toggleAutoSend(index);
    });
    connect(clearReceive, &QPushButton::clicked, channel.receiveEdit, &QPlainTextEdit::clear);
    connect(clearSend, &QPushButton::clicked, channel.sendEdit, &QPlainTextEdit::clear);

    setChannelStatus(index, QStringLiteral("未连接"));
    return channel.group;
}

bool SerialWorkspace::channelUsesAgent(int index) const
{
    return channels_[index].sourceCombo->currentData().toBool();
}

bool SerialWorkspace::selectedAgentIsOnline() const
{
    return currentAdbDevice_.state == "device" && !currentAdbDevice_.serial.empty()
           && agentControlPort_ != 0 && agentAdbSerial_ == currentAdbDevice_.serial;
}

void SerialWorkspace::refreshTargets()
{
    if (shuttingDown_ || channels_[0].transport || channels_[1].transport || scanning_) {
        return;
    }

    localPorts_ = CSerialPortTransport::availablePorts();
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
                                 ? QStringLiteral("未选择安卓设备，板端串口列表为空")
                                 : QStringLiteral("请先在设置中部署并连接该设备的 Agent"));
        updateGlobalControls();
        return;
    }

    scanning_ = true;
    emit activityChanged(QStringLiteral("正在通过 Agent 读取 %1 的板端串口")
                             .arg(fromUtf8(currentAdbDevice_.serial)));
    updateGlobalControls();
    const std::uint16_t controlPort = agentControlPort_;
    QPointer<SerialWorkspace> guard(this);
    scanThreads_.emplace_back([guard, controlPort] {
        std::string error;
        auto ports = AgentSerialTransport::availablePorts(controlPort, error);
        QMetaObject::invokeMethod(guard, [guard, ports = std::move(ports), error = std::move(error)] {
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
                emit guard->activityChanged(QStringLiteral("Agent 已发现 %1 个板端串口")
                                                .arg(guard->agentPorts_.size()));
            }
            guard->updateGlobalControls();
        }, Qt::QueuedConnection);
    });
}

void SerialWorkspace::setAdbDevice(const AdbDevice &device)
{
    if (hasActiveConnections()) {
        return;
    }
    currentAdbDevice_ = device;
    refreshTargets();
    updateRouteSummary();
}

void SerialWorkspace::setAgentConnection(const std::string &adbSerial,
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
    return channels_[0].transport || channels_[1].transport
           || channels_[0].connecting || channels_[1].connecting;
}

void SerialWorkspace::applyPorts(int index)
{
    auto &channel = channels_[index];
    if (!channel.portCombo || channel.transport) {
        return;
    }
    const QString previousData = channel.portCombo->currentData().toString();
    const QString previousText = channel.portCombo->currentText();
    const QSignalBlocker blocker(channel.portCombo);
    channel.portCombo->clear();

    if (channelUsesAgent(index)) {
        for (const auto &port : agentPorts_) {
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
        channel.portCombo->setToolTip(QStringLiteral("由板端 Agent 枚举和独占管理的设备节点"));
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
    if (restoreIndex >= 0) {
        channel.portCombo->setCurrentIndex(restoreIndex);
    }
    updateRouteSummary();
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
        error = channelName(index) + QStringLiteral(" 尚未选择串口");
        return false;
    }
    if (config.baudRate <= 0) {
        error = channelName(index) + QStringLiteral(" 的波特率无效");
        return false;
    }
    if (channelUsesAgent(index) && !selectedAgentIsOnline()) {
        error = QStringLiteral("请先在设置中部署并连接板端 Agent，再连接 ") + channelName(index);
        return false;
    }

    const int otherIndex = 1 - index;
    const auto &other = channels_[otherIndex];
    if (other.transport || other.connecting) {
        const auto otherConfig = channelConfig(otherIndex);
        if (channelUsesAgent(index) && channelUsesAgent(otherIndex)) {
            error = QStringLiteral("当前 Agent 版本一次管理一个板端串口");
            return false;
        }
        if (channelUsesAgent(index) == channelUsesAgent(otherIndex)
            && config.endpoint == otherConfig.endpoint
            && (!channelUsesAgent(index) || config.adbSerial == otherConfig.adbSerial)) {
            error = QStringLiteral("端点 A 和 B 不能同时打开同一个串口");
            return false;
        }
    }
    return true;
}

void SerialWorkspace::connectChannel(int index)
{
    auto &channel = channels_[index];
    if (channel.transport) {
        return;
    }

    QString error;
    if (!validateChannel(index, error)) {
        QMessageBox::warning(this, QStringLiteral("无法连接"), error);
        return;
    }

    const bool agent = channelUsesAgent(index);
    if (agent) {
        channel.transport = std::make_unique<AgentSerialTransport>(agentAdbSerial_, agentControlPort_);
    } else {
        channel.transport = std::make_unique<CSerialPortTransport>();
    }
    attachTransportCallbacks(index);
    startSender(index);
    channel.connecting = true;
    channel.connectButton->setText(QStringLiteral("取消 / 断开"));
    setChannelControlsEnabled(index, false);
    setChannelStatus(index, QStringLiteral("正在连接..."));
    updateGlobalControls();
    channel.transport->open(channelConfig(index));

    if (!agent && !channel.transport->isOpen()) {
        disconnectChannel(index, false);
    }
}

void SerialWorkspace::attachTransportCallbacks(int index)
{
    QPointer<SerialWorkspace> guard(this);
    channels_[index].transport->setDataCallback([guard, index](const ByteBuffer &data) {
        QMetaObject::invokeMethod(guard, [guard, index, data] {
            if (guard && !guard->shuttingDown_) {
                guard->appendReceived(index, data);
            }
        }, Qt::QueuedConnection);
    });
    channels_[index].transport->setStatusCallback([guard, index](TransportState state,
                                                                 const std::string &message) {
        QMetaObject::invokeMethod(guard, [guard, index, state, message] {
            if (!guard || guard->shuttingDown_) {
                return;
            }
            auto &channel = guard->channels_[index];
            const QString status = fromUtf8(message);
            emit guard->activityChanged(status);
            const bool connected = state == TransportState::Connected;
            const bool progress = state == TransportState::Progress;
            const bool error = state == TransportState::Error;
            guard->setChannelStatus(index, status, error);
            if (connected) {
                channel.connecting = false;
                channel.connectButton->setText(QStringLiteral("断开"));
                channel.sendButton->setEnabled(true);
            } else if (!progress) {
                channel.connecting = false;
                if (channel.transport && !channel.transport->isOpen()) {
                    guard->disconnectChannel(index, false);
                    guard->setChannelStatus(index, status, error);
                }
            }
            guard->updateGlobalControls();
        }, Qt::QueuedConnection);
    });
}

void SerialWorkspace::startSender(int index)
{
    auto &channel = channels_[index];
    const std::uint64_t generation = ++channel.senderGeneration;
    QPointer<SerialWorkspace> guard(this);
    channel.sender.start(
        [this, index](const ByteBuffer &data) {
            auto &current = channels_[index];
            return current.transport && current.transport->isOpen() && current.transport->write(data);
        },
        [guard, index, generation](const ByteBuffer &data, std::uint64_t tag, bool succeeded) {
            QMetaObject::invokeMethod(guard, [guard, index, generation, data, tag, succeeded] {
                if (!guard || guard->shuttingDown_) {
                    return;
                }
                auto &current = guard->channels_[index];
                if (current.senderGeneration != generation) {
                    return;
                }
                if (!succeeded) {
                    guard->setChannelStatus(index, QStringLiteral("发送失败，连接可能已经断开"), true);
                    guard->stopAutoSend(index, true);
                    if (guard->stressTestRunning_) {
                        guard->stopStressTest(QStringLiteral("可靠性压测发送失败"));
                    }
                    return;
                }
                current.sentBytes += data.size();
                if ((tag & 1u) != 0u) {
                    guard->appendTransmitted(index, data);
                }
                guard->updateCounter(index);
            }, Qt::QueuedConnection);
        });
}

void SerialWorkspace::disconnectChannel(int index, bool showClosedStatus)
{
    auto &channel = channels_[index];
    stopAutoSend(index);
    if (stressTestRunning_) {
        stopStressTest(QStringLiteral("串口连接已断开"));
    }
    ++channel.senderGeneration;
    channel.sender.stop(false);
    if (channel.transport) {
        channel.transport->setDataCallback({});
        channel.transport->setStatusCallback({});
        channel.transport->close();
        channel.transport.reset();
    }
    channel.connecting = false;
    channel.expectedFrame.clear();
    channel.detectionBuffer.clear();
    channel.connectButton->setText(QStringLiteral("连接"));
    channel.sendButton->setEnabled(false);
    setChannelControlsEnabled(index, true);
    if (showClosedStatus) {
        setChannelStatus(index, QStringLiteral("已断开"));
    }
    bidirectionalTestRunning_ = false;
    ++testGeneration_;
    updateGlobalControls();
}

void SerialWorkspace::disconnectAll()
{
    disconnectChannel(0, channels_[0].transport != nullptr);
    disconnectChannel(1, channels_[1].transport != nullptr);
}

void SerialWorkspace::setChannelControlsEnabled(int index, bool enabled)
{
    auto &channel = channels_[index];
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
    const bool anyActive = channels_[0].transport || channels_[1].transport
                           || channels_[0].connecting || channels_[1].connecting;
    const bool bothOpen = channels_[0].transport && channels_[0].transport->isOpen()
                          && channels_[1].transport && channels_[1].transport->isOpen();
    const bool anyAutoSend = channels_[0].autoSendPlan.isRunning()
                             || channels_[1].autoSendPlan.isRunning();
    const auto resourceConflict = [this] {
        const auto configA = channelConfig(0);
        const auto configB = channelConfig(1);
        return (channelUsesAgent(0) && channelUsesAgent(1))
               || (channelUsesAgent(0) == channelUsesAgent(1)
               && !configA.endpoint.empty() && configA.endpoint == configB.endpoint
               && (!channelUsesAgent(0) || configA.adbSerial == configB.adbSerial));
    };
    const auto channelReady = [this, &resourceConflict](int index) {
        const auto &channel = channels_[index];
        const bool basicReady = !channel.portCombo->currentText().trimmed().isEmpty()
                                && channel.baudCombo->currentText().toInt() > 0
                                && (!channelUsesAgent(index) || selectedAgentIsOnline());
        const int otherIndex = 1 - index;
        const bool conflictsWithOpenChannel = channels_[otherIndex].transport && resourceConflict();
        return basicReady && !conflictsWithOpenChannel;
    };
    for (int index = 0; index < 2; ++index) {
        const bool open = channels_[index].transport && channels_[index].transport->isOpen();
        if (channels_[index].transport) {
            channels_[index].connectButton->setEnabled(true);
        } else {
            channels_[index].connectButton->setEnabled(!scanning_ && channelReady(index));
        }
        channels_[index].sendButton->setEnabled(open && !stressTestRunning_
                                                && !channels_[index].autoSendPlan.isRunning());
        channels_[index].autoSendButton->setEnabled(open && !stressTestRunning_);
    }
    const bool missingChannelsReady = (channels_[0].transport || channelReady(0))
                                      && (channels_[1].transport || channelReady(1));
    const bool pairCompatible = channels_[0].transport || channels_[1].transport || !resourceConflict();
    connectBothButton_->setEnabled(!scanning_ && !bothOpen && !channels_[0].connecting
                                   && !channels_[1].connecting && missingChannelsReady && pairCompatible);
    connectBothButton_->setText(bothOpen ? QStringLiteral("链路已连接")
                                : anyActive ? QStringLiteral("连接另一端") : QStringLiteral("连接链路"));
    disconnectBothButton_->setEnabled(anyActive);
    bidirectionalTestButton_->setEnabled(bothOpen && !bidirectionalTestRunning_
                                         && !stressTestRunning_ && !anyAutoSend);
    bidirectionalTestButton_->setText(bidirectionalTestRunning_
                                          ? QStringLiteral("验证中...") : QStringLiteral("双向验证"));
    stressTestButton_->setEnabled(stressTestRunning_
                                  || (bothOpen && !bidirectionalTestRunning_ && !anyAutoSend));
    stressTestButton_->setText(stressTestRunning_
                                   ? QStringLiteral("停止压测") : QStringLiteral("可靠性压测"));
    emit busyChanged(anyActive);
}

void SerialWorkspace::setChannelStatus(int index, const QString &message, bool error)
{
    auto *label = channels_[index].statusLabel;
    QString display = message;
    QString role = QStringLiteral("connectionStatusIdle");
    if (message.startsWith(QStringLiteral("正在"))) {
        display = QStringLiteral("连接中");
        role = QStringLiteral("connectionStatusWorking");
    } else if (!error && message.contains(QStringLiteral("已连接"))) {
        display = QStringLiteral("已连接");
        role = QStringLiteral("connectionStatusGood");
    } else if (error) {
        display = QStringLiteral("异常");
        role = QStringLiteral("connectionStatusError");
    }
    label->setText(display);
    label->setToolTip(message);
    setVisualRole(label, role);
}

void SerialWorkspace::updateRouteSummary()
{
    if (!routeLabel_ || !channels_[0].sourceCombo || !channels_[1].sourceCombo) {
        return;
    }
    const auto endpoint = [this](int index) {
        const auto &channel = channels_[index];
        const QString type = channelUsesAgent(index) ? QStringLiteral("板端 Agent") : QStringLiteral("Windows");
        const QString data = channel.portCombo->currentData().toString();
        const QString port = data.isEmpty() ? channel.portCombo->currentText() : data;
        return QStringLiteral("%1 · %2").arg(type, port.isEmpty() ? QStringLiteral("未选择") : port);
    };
    routeLabel_->setText(QStringLiteral("A  %1     ⇄     B  %2").arg(endpoint(0), endpoint(1)));
}

ByteBuffer SerialWorkspace::manualPayload(int index, bool &ok)
{
    auto &channel = channels_[index];
    ByteBuffer data;
    ok = true;
    if (channel.sendHexCheck->isChecked()) {
        std::string error;
        if (!parseHex(toUtf8(channel.sendEdit->toPlainText()), data, error)) {
            QMessageBox::warning(this, QStringLiteral("HEX 输入错误"), fromUtf8(error));
            ok = false;
            return {};
        }
    } else {
        const QByteArray text = channel.sendEdit->toPlainText().toUtf8();
        data.assign(reinterpret_cast<const std::uint8_t *>(text.constData()),
                    reinterpret_cast<const std::uint8_t *>(text.constData()) + text.size());
    }
    appendLineEnding(data, lineEndingFromIndex(channel.lineEndingCombo->currentIndex()));
    return data;
}

bool SerialWorkspace::sendBytes(int index, const ByteBuffer &data, bool echo)
{
    auto &channel = channels_[index];
    if (!channel.transport || !channel.transport->isOpen()) {
        setChannelStatus(index, QStringLiteral("尚未连接，无法发送"), true);
        return false;
    }
    if (!channel.sender.isRunning() || !channel.sender.enqueue(data, echo ? 1u : 0u)) {
        setChannelStatus(index, QStringLiteral("发送队列已满或不可用"), true);
        return false;
    }
    return true;
}

void SerialWorkspace::sendManual(int index)
{
    bool ok = false;
    const auto data = manualPayload(index, ok);
    if (ok) {
        sendBytes(index, data);
    }
}

void SerialWorkspace::toggleAutoSend(int index)
{
    auto &channel = channels_[index];
    if (channel.autoSendPlan.isRunning()) {
        stopAutoSend(index, true);
        return;
    }
    bool ok = false;
    auto payload = manualPayload(index, ok);
    if (!ok) {
        return;
    }
    if (payload.empty()) {
        QMessageBox::information(this, QStringLiteral("没有发送内容"),
                                 QStringLiteral("请输入数据，或选择需要追加的行尾。"));
        return;
    }
    if (!channel.transport || !channel.transport->isOpen()) {
        QMessageBox::information(this, QStringLiteral("尚未连接"),
                                 QStringLiteral("需要先连接该端点。"));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(channelName(index) + QStringLiteral(" 定时 / 次数发送"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *intervalSpin = new QSpinBox(&dialog);
    intervalSpin->setRange(10, 60000);
    intervalSpin->setValue(channel.autoIntervalMs);
    intervalSpin->setSuffix(QStringLiteral(" ms"));
    auto *countSpin = new QSpinBox(&dialog);
    countSpin->setRange(0, 1000000);
    countSpin->setValue(static_cast<int>(channel.autoTargetCount));
    countSpin->setSpecialValueText(QStringLiteral("持续"));
    auto *echoCheck = new QCheckBox(QStringLiteral("在收发监视中记录每次 TX"), &dialog);
    echoCheck->setChecked(channel.autoEcho);
    echoCheck->setToolTip(QStringLiteral("高频或长时间发送时可关闭，减少界面刷新开销"));
    form->addRow(QStringLiteral("发送间隔"), intervalSpin);
    form->addRow(QStringLiteral("发送次数"), countSpin);
    form->addRow(QString(), echoCheck);
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
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
    if (!channel.autoSendPlan.start(config)) {
        QMessageBox::warning(this, QStringLiteral("参数无效"), QStringLiteral("发送间隔必须大于 0。"));
        return;
    }
    channel.autoSendPayload = std::move(payload);
    channel.autoSendButton->setText(QStringLiteral("停止计划发送"));
    channel.sendEdit->setEnabled(false);
    channel.sendHexCheck->setEnabled(false);
    channel.lineEndingCombo->setEnabled(false);
    emit activityChanged(QStringLiteral("%1 已开始定时 / 次数发送").arg(channelName(index)));
    updateGlobalControls();
}

void SerialWorkspace::stopAutoSend(int index, bool announce)
{
    auto &channel = channels_[index];
    const bool wasRunning = channel.autoSendPlan.isRunning();
    const auto attempts = channel.autoSendPlan.attempts();
    const auto successes = channel.autoSendPlan.successes();
    channel.autoSendPlan.stop();
    channel.autoSendPayload.clear();
    if (channel.autoSendButton) {
        channel.autoSendButton->setText(QStringLiteral("定时 / 次数…"));
        channel.sendEdit->setEnabled(true);
        channel.sendHexCheck->setEnabled(true);
        channel.lineEndingCombo->setEnabled(true);
    }
    if (announce && wasRunning) {
        emit activityChanged(QStringLiteral("%1 计划发送结束：已提交 %2 / 尝试 %3")
                                 .arg(channelName(index)).arg(successes).arg(attempts));
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
        auto &channel = channels_[index];
        if (!channel.autoSendPlan.isDue(now)) {
            continue;
        }
        const bool sent = sendBytes(index, channel.autoSendPayload,
                                    channel.autoEcho);
        channel.autoSendPlan.recordAttempt(sent, now);
        if (!sent || !channel.autoSendPlan.isRunning()) {
            stopAutoSend(index, true);
        }
    }

    if (!stressTestRunning_ || stressAwaitingReceive_) {
        return;
    }
    bool allSent = true;
    bool queuesDrained = true;
    for (auto &direction : stressDirections_) {
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
        queuesDrained = queuesDrained
                        && channels_[direction.transmitter].sender.pendingCount() == 0;
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
    QString display;
    if (channel.timestampCheck->isChecked()) {
        display += QDateTime::currentDateTime().toString(QStringLiteral("[HH:mm:ss.zzz] "));
    }
    display += QStringLiteral("TX  ");
    if (channel.receiveHexCheck->isChecked()) {
        display += fromUtf8(toHex(data));
    } else {
        QString payload = QString::fromUtf8(reinterpret_cast<const char *>(data.data()),
                                            static_cast<qsizetype>(data.size()));
        payload.replace(QStringLiteral("\r"), QStringLiteral("\\r"));
        payload.replace(QStringLiteral("\n"), QStringLiteral("\\n"));
        display += payload;
    }
    display += QLatin1Char('\n');
    channel.receiveEdit->moveCursor(QTextCursor::End);
    channel.receiveEdit->insertPlainText(display);
    channel.receiveEdit->moveCursor(QTextCursor::End);
}

void SerialWorkspace::appendReceived(int index, const ByteBuffer &data)
{
    auto &channel = channels_[index];
    channel.receivedBytes += data.size();

    if (stressTestRunning_) {
        for (auto &direction : stressDirections_) {
            if (direction.active && direction.receiver == index) {
                direction.verifier.push(data);
            }
        }
        updateCounter(index);
        return;
    }

    if (bidirectionalTestRunning_ && !channel.expectedFrame.empty()) {
        channel.detectionBuffer.append(reinterpret_cast<const char *>(data.data()), data.size());
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

    QString display;
    if (channel.timestampCheck->isChecked()) {
        display += QDateTime::currentDateTime().toString(QStringLiteral("[HH:mm:ss.zzz] "));
    }
    display += QStringLiteral("RX  ");
    if (channel.receiveHexCheck->isChecked()) {
        display += fromUtf8(toHex(data));
    } else {
        display += QString::fromUtf8(reinterpret_cast<const char *>(data.data()),
                                     static_cast<qsizetype>(data.size()));
    }
    if (!display.endsWith(QLatin1Char('\n'))) {
        display += QLatin1Char('\n');
    }
    channel.receiveEdit->moveCursor(QTextCursor::End);
    channel.receiveEdit->insertPlainText(display);
    channel.receiveEdit->moveCursor(QTextCursor::End);
    updateCounter(index);
}

void SerialWorkspace::updateCounter(int index)
{
    const auto &channel = channels_[index];
    channel.counterLabel->setText(QStringLiteral("RX: %1 B  TX: %2 B")
                                      .arg(channel.receivedBytes).arg(channel.sentBytes));
}

void SerialWorkspace::runBidirectionalTest()
{
    if (!channels_[0].transport || !channels_[0].transport->isOpen()
        || !channels_[1].transport || !channels_[1].transport->isOpen()) {
        QMessageBox::information(this, QStringLiteral("尚未连接"), QStringLiteral("需要先连接端点 A 和端点 B。"));
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
        testResultLabel_->setText(QStringLiteral("链路验证 · 发送失败"));
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
        testResultLabel_->setText(QStringLiteral("链路验证 · A→B PASS  /  B→A PASS"));
        setVisualRole(testResultLabel_, QStringLiteral("testStatusGood"));
    } else if (timedOut) {
        bidirectionalTestRunning_ = false;
        testResultLabel_->setText(QStringLiteral("链路验证超时 · A→B %1  /  B→A %2")
                                      .arg(aToBPassed_ ? QStringLiteral("PASS") : QStringLiteral("FAIL"))
                                      .arg(bToAPassed_ ? QStringLiteral("PASS") : QStringLiteral("FAIL")));
        setVisualRole(testResultLabel_, QStringLiteral("testStatusError"));
    } else {
        testResultLabel_->setText(QStringLiteral("正在验证 · A→B %1  /  B→A %2")
                                      .arg(aToBPassed_ ? QStringLiteral("PASS") : QStringLiteral("等待"))
                                      .arg(bToAPassed_ ? QStringLiteral("PASS") : QStringLiteral("等待")));
        setVisualRole(testResultLabel_, QStringLiteral("testStatusWorking"));
    }
    updateGlobalControls();
}

void SerialWorkspace::showStressDialog()
{
    if (!channels_[0].transport || !channels_[0].transport->isOpen()
        || !channels_[1].transport || !channels_[1].transport->isOpen()) {
        QMessageBox::information(this, QStringLiteral("尚未连接"),
                                 QStringLiteral("可靠性压测需要先连接端点 A 和端点 B。"));
        return;
    }
    if (channelConfig(0).dataBits != 8 || channelConfig(1).dataBits != 8) {
        QMessageBox::information(this, QStringLiteral("串口格式不适合压测"),
                                 QStringLiteral("可靠性测试帧包含完整 8 bit 二进制数据，请先将两个端点的数据位都设为 8。"));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("串口可靠性压测"));
    dialog.setMinimumWidth(420);
    auto *layout = new QVBoxLayout(&dialog);
    auto *description = new QLabel(
        QStringLiteral("发送带序号、确定性载荷和 CRC32 的测试帧。BER 只统计已解析载荷，丢帧单独报告。"),
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
        channels_[index].receiveEdit->appendPlainText(
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
