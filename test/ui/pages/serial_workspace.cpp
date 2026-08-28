#include "serial_workspace.h"

#include "core/adb_transport.h"
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
    routeArea->addWidget(routeLabel_);
    routeArea->addWidget(testResultLabel_);
    linkLayout->addLayout(routeArea, 1);
    connectBothButton_ = new QPushButton(QStringLiteral("连接链路"), linkBar);
    connectBothButton_->setObjectName(QStringLiteral("primaryButton"));
    disconnectBothButton_ = new QPushButton(QStringLiteral("断开"), linkBar);
    bidirectionalTestButton_ = new QPushButton(QStringLiteral("双向验证"), linkBar);
    bidirectionalTestButton_->setObjectName(QStringLiteral("verifyButton"));
    linkLayout->addWidget(connectBothButton_);
    linkLayout->addWidget(disconnectBothButton_);
    linkLayout->addWidget(bidirectionalTestButton_);
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
            if (channelUsesAdb(0) == channelUsesAdb(1)
                && configA.endpoint == configB.endpoint
                && (!channelUsesAdb(0) || configA.adbSerial == configB.adbSerial)) {
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
    channel.sourceCombo->addItem(QStringLiteral("ADB 板端串口"), true);
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
    channel.adbRootCheck = new QCheckBox(QStringLiteral("权限不足时尝试 ADB root"), channel.group);
    channel.adbRootCheck->setChecked(true);
    channel.adbRootCheck->setToolTip(
        QStringLiteral("仅在板端节点不可读写时重启 adbd；量产设备可能不支持"));
    settings->addWidget(channel.adbRootCheck, 2, 1, 1, 6);
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
    channel.sendEdit->setMaximumHeight(95);
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
    sendTools->addWidget(channel.sendHexCheck);
    sendTools->addWidget(new QLabel(QStringLiteral("行尾"), channel.group));
    sendTools->addWidget(channel.lineEndingCombo);
    sendTools->addStretch();
    sendTools->addWidget(clearSend);
    sendTools->addWidget(channel.sendButton);
    layout->addLayout(sendTools);

    connect(channel.sourceCombo, &QComboBox::currentIndexChanged, this, [this, index] {
        channels_[index].adbRootCheck->setVisible(channelUsesAdb(index));
        channels_[index].adbRootCheck->setEnabled(channelUsesAdb(index));
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
    connect(clearReceive, &QPushButton::clicked, channel.receiveEdit, &QPlainTextEdit::clear);
    connect(clearSend, &QPushButton::clicked, channel.sendEdit, &QPlainTextEdit::clear);

    channel.adbRootCheck->setVisible(channelUsesAdb(index));
    channel.adbRootCheck->setEnabled(channelUsesAdb(index));
    setChannelStatus(index, QStringLiteral("未连接"));
    return channel.group;
}

bool SerialWorkspace::channelUsesAdb(int index) const
{
    return channels_[index].sourceCombo->currentData().toBool();
}

bool SerialWorkspace::selectedAdbIsOnline() const
{
    return currentAdbDevice_.state == "device" && !currentAdbDevice_.serial.empty();
}

void SerialWorkspace::refreshTargets()
{
    if (shuttingDown_ || channels_[0].transport || channels_[1].transport || scanning_) {
        return;
    }

    localPorts_ = CSerialPortTransport::availablePorts();
    applyPorts(0);
    applyPorts(1);
    refreshAdbPorts();
}

void SerialWorkspace::refreshAdbPorts()
{
    adbPorts_.clear();
    applyPorts(0);
    applyPorts(1);

    if (!selectedAdbIsOnline()) {
        scanning_ = false;
        emit activityChanged(currentAdbDevice_.serial.empty()
                                 ? QStringLiteral("未选择在线 ADB 设备，板端串口列表为空")
                                 : QStringLiteral("ADB 设备状态为 %1，板端串口不可用")
                                       .arg(fromUtf8(currentAdbDevice_.state)));
        updateGlobalControls();
        return;
    }

    scanning_ = true;
    emit activityChanged(QStringLiteral("正在读取 %1 的板端串口")
                             .arg(fromUtf8(currentAdbDevice_.serial)));
    updateGlobalControls();
    const std::string serial = currentAdbDevice_.serial;
    QPointer<SerialWorkspace> guard(this);
    scanThreads_.emplace_back([guard, serial] {
        AdbTransport scanner;
        std::string error;
        auto ports = scanner.availableRemotePorts(serial, error);
        QMetaObject::invokeMethod(guard, [guard, ports = std::move(ports), error = std::move(error)] {
            if (!guard || guard->shuttingDown_) {
                return;
            }
            guard->scanning_ = false;
            guard->adbPorts_ = std::move(ports);
            guard->applyPorts(0);
            guard->applyPorts(1);
            if (!error.empty()) {
                emit guard->activityChanged(fromUtf8(error));
            } else {
                emit guard->activityChanged(QStringLiteral("已发现 %1 个板端串口")
                                                .arg(guard->adbPorts_.size()));
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

    if (channelUsesAdb(index)) {
        for (const auto &port : adbPorts_) {
            channel.portCombo->addItem(fromUtf8(port), fromUtf8(port));
        }
        channel.portCombo->setToolTip(QStringLiteral("板端设备节点，例如 /dev/ttyS2"));
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
    config.allowAdbRoot = channelUsesAdb(index) && channel.adbRootCheck->isChecked();
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
    if (channelUsesAdb(index) && !selectedAdbIsOnline()) {
        error = QStringLiteral("ADB 设备不是 device 状态，无法连接 ") + channelName(index);
        return false;
    }

    const int otherIndex = 1 - index;
    const auto &other = channels_[otherIndex];
    if (other.transport || other.connecting) {
        const auto otherConfig = channelConfig(otherIndex);
        if (channelUsesAdb(index) == channelUsesAdb(otherIndex)
            && config.endpoint == otherConfig.endpoint
            && (!channelUsesAdb(index) || config.adbSerial == otherConfig.adbSerial)) {
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

    const bool adb = channelUsesAdb(index);
    if (adb) {
        channel.transport = std::make_unique<AdbTransport>();
    } else {
        channel.transport = std::make_unique<CSerialPortTransport>();
    }
    attachTransportCallbacks(index);
    channel.connecting = true;
    channel.connectButton->setText(QStringLiteral("取消 / 断开"));
    setChannelControlsEnabled(index, false);
    setChannelStatus(index, QStringLiteral("正在连接..."));
    updateGlobalControls();
    channel.transport->open(channelConfig(index));

    if (!adb && !channel.transport->isOpen()) {
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
    channels_[index].transport->setStatusCallback([guard, index](bool connected, const std::string &message) {
        QMetaObject::invokeMethod(guard, [guard, index, connected, message] {
            if (!guard || guard->shuttingDown_) {
                return;
            }
            auto &channel = guard->channels_[index];
            const QString status = fromUtf8(message);
            emit guard->activityChanged(status);
            const bool progress = status.startsWith(QStringLiteral("正在"));
            guard->setChannelStatus(index, status, !connected && !progress
                                                   && !status.endsWith(QStringLiteral("已关闭")));
            if (connected) {
                channel.connecting = false;
                channel.connectButton->setText(QStringLiteral("断开"));
                channel.sendButton->setEnabled(true);
            } else if (!progress) {
                channel.connecting = false;
                if (channel.transport && !channel.transport->isOpen()) {
                    guard->disconnectChannel(index, false);
                    guard->setChannelStatus(index, status, !status.endsWith(QStringLiteral("已关闭")));
                }
            }
            guard->updateGlobalControls();
        }, Qt::QueuedConnection);
    });
}

void SerialWorkspace::disconnectChannel(int index, bool showClosedStatus)
{
    auto &channel = channels_[index];
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
    channel.adbRootCheck->setEnabled(enabled && channelUsesAdb(index));
}

void SerialWorkspace::updateGlobalControls()
{
    const bool anyActive = channels_[0].transport || channels_[1].transport
                           || channels_[0].connecting || channels_[1].connecting;
    const bool bothOpen = channels_[0].transport && channels_[0].transport->isOpen()
                          && channels_[1].transport && channels_[1].transport->isOpen();
    const auto sameEndpoint = [this] {
        const auto configA = channelConfig(0);
        const auto configB = channelConfig(1);
        return channelUsesAdb(0) == channelUsesAdb(1)
               && !configA.endpoint.empty() && configA.endpoint == configB.endpoint
               && (!channelUsesAdb(0) || configA.adbSerial == configB.adbSerial);
    };
    const auto channelReady = [this, &sameEndpoint](int index) {
        const auto &channel = channels_[index];
        const bool basicReady = !channel.portCombo->currentText().trimmed().isEmpty()
                                && channel.baudCombo->currentText().toInt() > 0
                                && (!channelUsesAdb(index) || selectedAdbIsOnline());
        const int otherIndex = 1 - index;
        const bool conflictsWithOpenChannel = channels_[otherIndex].transport && sameEndpoint();
        return basicReady && !conflictsWithOpenChannel;
    };
    for (int index = 0; index < 2; ++index) {
        if (channels_[index].transport) {
            channels_[index].connectButton->setEnabled(true);
        } else {
            channels_[index].connectButton->setEnabled(!scanning_ && channelReady(index));
        }
    }
    const bool missingChannelsReady = (channels_[0].transport || channelReady(0))
                                      && (channels_[1].transport || channelReady(1));
    const bool pairCompatible = channels_[0].transport || channels_[1].transport || !sameEndpoint();
    connectBothButton_->setEnabled(!scanning_ && !bothOpen && !channels_[0].connecting
                                   && !channels_[1].connecting && missingChannelsReady && pairCompatible);
    connectBothButton_->setText(bothOpen ? QStringLiteral("链路已连接")
                                : anyActive ? QStringLiteral("连接另一端") : QStringLiteral("连接链路"));
    disconnectBothButton_->setEnabled(anyActive);
    bidirectionalTestButton_->setEnabled(bothOpen && !bidirectionalTestRunning_);
    bidirectionalTestButton_->setText(bidirectionalTestRunning_
                                          ? QStringLiteral("验证中...") : QStringLiteral("双向验证"));
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
        const QString type = channelUsesAdb(index) ? QStringLiteral("ADB 直连") : QStringLiteral("Windows");
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

bool SerialWorkspace::sendBytes(int index, const ByteBuffer &data)
{
    auto &channel = channels_[index];
    if (!channel.transport || !channel.transport->isOpen()) {
        setChannelStatus(index, QStringLiteral("尚未连接，无法发送"), true);
        return false;
    }
    if (!channel.transport->write(data)) {
        setChannelStatus(index, QStringLiteral("发送失败，连接可能已经断开"), true);
        return false;
    }
    channel.sentBytes += data.size();
    appendTransmitted(index, data);
    updateCounter(index);
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
