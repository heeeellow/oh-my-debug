#pragma once
#include <QtWidgets/QFrame>
class QComboBox;
class QCheckBox;
class QHBoxLayout;

// Owns connection parameter controls; sessions and sends stay in the workspace/service.
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
