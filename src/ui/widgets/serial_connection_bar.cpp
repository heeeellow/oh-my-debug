#include "ui/widgets/serial_connection_bar.h"
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QVBoxLayout>

SerialConnectionBar::SerialConnectionBar(QWidget* parent) : QFrame(parent)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(4);
    auto* connectionRow = new QFrame(this);
    connectionRow->setObjectName(QStringLiteral("connectionRow"));
    settingsRowLayout = new QHBoxLayout(connectionRow);
    settingsRowLayout->setContentsMargins(0, 0, 0, 8);
    settingsRowLayout->setSpacing(6);
    sourceCombo = new QComboBox(connectionRow);
    sourceCombo->addItem(QStringLiteral("Windows 本地串口"), false);
    sourceCombo->addItem(QStringLiteral("设备串口"), true);
    sourceCombo->setMinimumWidth(148);
    sourceCombo->setMaximumWidth(176);

    portCombo = new QComboBox(connectionRow);
    portCombo->setEditable(true);
    portCombo->setMinimumContentsLength(12);
    portCombo->setMaximumWidth(320);

    baudCombo = new QComboBox(connectionRow);
    baudCombo->setEditable(true);
    baudCombo->addItems({QStringLiteral("9600"),
                         QStringLiteral("19200"),
                         QStringLiteral("38400"),
                         QStringLiteral("57600"),
                         QStringLiteral("115200"),
                         QStringLiteral("230400"),
                         QStringLiteral("460800"),
                         QStringLiteral("921600"),
                         QStringLiteral("1500000")});
    baudCombo->setMinimumWidth(106);
    baudCombo->setMaximumWidth(128);
    baudCombo->setCurrentText(QStringLiteral("115200"));
    baudCombo->setToolTip(
        QStringLiteral("物理 UART 使用该波特率；ttyUSB/ttyACM 是 USB 虚拟串口，驱动可能忽略该数值，"
                       "它不代表 USB 链路速率。"));
    baudCombo->lineEdit()->setCursorPosition(0);

    const auto fieldLabel = [this](const QString& text) {
        auto* label = new QLabel(text, this);
        label->setObjectName(QStringLiteral("fieldLabel"));
        return label;
    };
    settingsRowLayout->addWidget(fieldLabel(QStringLiteral("位置")));
    settingsRowLayout->addWidget(sourceCombo);
    settingsRowLayout->addWidget(fieldLabel(QStringLiteral("串口")));
    settingsRowLayout->addWidget(portCombo, 1);
    settingsRowLayout->addWidget(fieldLabel(QStringLiteral("波特率")));
    settingsRowLayout->addWidget(baudCombo);
    settingsRowLayout->addStretch();
    root->addWidget(connectionRow);

    dataBitsCombo = new QComboBox(this);
    dataBitsCombo->addItems(
        {QStringLiteral("5"), QStringLiteral("6"), QStringLiteral("7"), QStringLiteral("8")});
    dataBitsCombo->setCurrentText(QStringLiteral("8"));
    parityCombo = new QComboBox(this);
    parityCombo->addItems(
        {QStringLiteral("无校验"), QStringLiteral("奇校验"), QStringLiteral("偶校验")});
    stopBitsCombo = new QComboBox(this);
    stopBitsCombo->addItems({QStringLiteral("1 停止位"), QStringLiteral("2 停止位")});
    flowControlCombo = new QComboBox(this);
    flowControlCombo->addItems(
        {QStringLiteral("无流控"), QStringLiteral("硬件流控"), QStringLiteral("软件流控")});

    advancedSettingsArea = new QWidget(this);
    auto* advancedSettings = new QGridLayout(advancedSettingsArea);
    advancedSettings->setContentsMargins(0, 0, 0, 2);
    advancedSettings->setSpacing(6);
    dataBitsCombo->setMinimumWidth(68);
    parityCombo->setMinimumWidth(92);
    stopBitsCombo->setMinimumWidth(92);
    flowControlCombo->setMinimumWidth(92);
    advancedSettings->addWidget(fieldLabel(QStringLiteral("数据位")), 0, 0);
    advancedSettings->addWidget(dataBitsCombo, 0, 1);
    advancedSettings->addWidget(fieldLabel(QStringLiteral("校验")), 0, 2);
    advancedSettings->addWidget(parityCombo, 0, 3);
    advancedSettings->addWidget(fieldLabel(QStringLiteral("停止位")), 1, 0);
    advancedSettings->addWidget(stopBitsCombo, 1, 1);
    advancedSettings->addWidget(fieldLabel(QStringLiteral("流控")), 1, 2);
    advancedSettings->addWidget(flowControlCombo, 1, 3);
    dtrCheck = new QCheckBox(QStringLiteral("DTR"), advancedSettingsArea);
    rtsCheck = new QCheckBox(QStringLiteral("RTS"), advancedSettingsArea);
    for (auto* control : {dtrCheck, rtsCheck}) {
        control->setCheckState(Qt::PartiallyChecked);
        control->setEnabled(false);
        control->setToolTip(
            QStringLiteral("连接本地串口后可手动设置；半选表示本次连接尚未设置，不自动记忆电平"));
    }
    advancedSettings->addWidget(dtrCheck, 0, 4);
    advancedSettings->addWidget(rtsCheck, 1, 4);
    advancedSettings->setColumnStretch(5, 1);
    advancedSettingsArea->hide();
    root->addWidget(advancedSettingsArea);
}
