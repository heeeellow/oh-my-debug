#pragma once
#include "application/device/device_maintenance_service.h"
#include "application/support/background_task_group.h"
#include <QtWidgets/QWidget>

class QLabel;
class QPushButton;
class QPlainTextEdit;
class QTimer;
class DeviceMaintenancePanel final : public QWidget {
    Q_OBJECT
  public:
    DeviceMaintenancePanel(QWidget* parent = nullptr,
                           std::shared_ptr<testtools::DeviceMaintenanceService> service = {});
    ~DeviceMaintenancePanel() override;
    void setDevice(const serialdemo::AdbDevice& device);
    void shutdown();
    void cancelTask(std::uint64_t id);
    testtools::MaintenanceSnapshot snapshot() const;
    std::function<testtools::MaintenanceConflicts(const serialdemo::AdbDevice&)> conflicts;
  signals:
    void activityChanged(const QString& message);
    void taskChanged();
    void shutdownFinished();

  private:
    void start(testtools::MaintenanceAction action);
    void startRequest(testtools::MaintenanceRequest request);
    void poll();
    void updateControls();
    std::shared_ptr<testtools::DeviceMaintenanceService> service_;
    testtools::BackgroundTaskGroup tasks_;
    serialdemo::AdbDevice device_;
    bool closing_ = false, wasActive_ = false, shutdownEmitted_ = false;
    QLabel *target_ = nullptr, *status_ = nullptr, *artifact_ = nullptr;
    QPlainTextEdit* detail_ = nullptr;
    QPushButton *logcat_ = nullptr, *dmesg_ = nullptr, *root_ = nullptr, *reboot_ = nullptr,
                *loader_ = nullptr, *cancel_ = nullptr;
    QTimer* timer_ = nullptr;
};
