#pragma once
#include "application/agent/board_info_service.h"
#include "application/support/background_task_group.h"
#include "core/serial/serial_types.h"
#include <QtWidgets/QWidget>

class QLabel;
class QPushButton;
class QTreeWidget;
class QTimer;
class BoardInfoPanel final : public QWidget {
    Q_OBJECT
  public:
    explicit BoardInfoPanel(QWidget* parent = nullptr,
                            std::shared_ptr<testtools::BoardInfoService> service = {});
    ~BoardInfoPanel() override;
    void setTarget(const serialdemo::AdbDevice& device);
    void setSession(std::string serial,
                    std::uint64_t generation,
                    std::shared_ptr<testtools::IAgentConnection> connection,
                    std::vector<std::string> capabilities,
                    std::function<std::shared_ptr<testtools::IAgentConnection>(std::string&)>
                        openConnection = {});
    void clearSession();
    void setDashboardVisible(bool visible);
    void shutdown();
    void cancelTask(std::uint64_t id);
    bool busyForDevice(const std::string& serial) const;
    testtools::BoardInfoSnapshot snapshot() const;
    std::function<bool(const std::string&)> maintenanceBusy;
  signals:
    void taskChanged();
    void activityChanged(const QString& message);
    void shutdownFinished();

  private:
    bool start(const std::vector<std::string>& capabilities);
    void maybeAutoRefresh();
    void poll();
    void updateControls();
    void render();
    void populateDetails(QTreeWidget* values, const testtools::BoardInfoSnapshot& state) const;
    void showDetails();
    testtools::BoardInfoSession session_;
    serialdemo::AdbDevice target_;
    std::shared_ptr<testtools::BoardInfoService> service_;
    testtools::BackgroundTaskGroup tasks_;
    bool closing_ = false, wasActive_ = false, shutdownEmitted_ = false;
    bool dashboardVisible_ = false, autoReadPending_ = true;
    QLabel* status_ = nullptr;
    QLabel *storageSummary_ = nullptr, *healthSummary_ = nullptr;
    QPushButton *all_ = nullptr, *cancel_ = nullptr, *details_ = nullptr;
    QTimer* timer_ = nullptr;
    std::string rendered_;
};
