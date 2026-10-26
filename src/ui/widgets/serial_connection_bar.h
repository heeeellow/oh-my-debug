#pragma once
#include <QtWidgets/QFrame>
class QComboBox;
class QCheckBox;
class QHBoxLayout;

// 仅持有串口连接参数控件；实际会话生命周期与数据收发仍归属工作区及应用层服务。
class SerialConnectionBar final : public QFrame {
  public:
    explicit SerialConnectionBar(QWidget* parent);
    QHBoxLayout* settingsRowLayout = nullptr;
    QWidget* advancedSettingsArea = nullptr;
    QComboBox* sourceCombo = nullptr;
    QComboBox* portCombo = nullptr;
    QComboBox* baudCombo = nullptr;
    QComboBox* dataBitsCombo = nullptr;
    QComboBox* parityCombo = nullptr;
    QComboBox* stopBitsCombo = nullptr;
    QComboBox* flowControlCombo = nullptr;
    QCheckBox *dtrCheck = nullptr, *rtsCheck = nullptr;
};
