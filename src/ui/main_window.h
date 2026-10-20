#pragma once

#include "application/device/device_discovery.h"
#include "application/support/background_task_group.h"

#include <QtWidgets/QMainWindow>

#include <cstdint>
#include <memory>
#include <vector>

class QComboBox;
class QLabel;
class QPushButton;
class QStackedWidget;
class QTimer;
class QWidget;
class SerialWorkspace;
namespace testtools {
class IAgentConnection;
}

class MainWindow final : public QMainWindow {
    Q_OBJECT

  public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;
    bool navigateTo(const QString& pageName);
    bool setSerialModeForDiagnostics(const QString& modeName);

  protected:
    void closeEvent(QCloseEvent* event) override;

  private:
    enum class StatusTone { Neutral, Good, Warning, Error };

    enum PageIndex { SerialPage = 0, DeviceInfoPage };

    void buildUi();
    QWidget* buildTopBar();
    QWidget* buildNavigation();
    QWidget* buildSerialPage();
    QWidget* buildDeviceInfoPage();
    QWidget* wrapPage(const QString& title,
                      const QString& subtitle,
                      QWidget* content,
                      bool scrollable = true);
    QPushButton* addNavigationButton(const QString& text, int pageIndex);

    void refreshDevices();
    void openAdbTerminal();
    void updateAdbTerminalControls();
    void startDeviceScan(bool automatic);
    void deploySelectedAgent();
    void connectSelectedAgent(bool allowDeploy);
    void stopSelectedAgent();
    void disconnectAgent(const QString& reason = {});
    void setAgentStatus(const QString& text, StatusTone tone, const QString& detail = {});
    void updateAgentControls();
    void prepareForShutdown();
    void applyDeviceScan(testtools::DeviceScanResult result, bool automatic);
    void applySelectedDevice();
    serialdemo::AdbDevice selectedDevice() const;
    void showPage(int pageIndex);
    void setAdbStatus(const QString& text, StatusTone tone = StatusTone::Good);
    void setActivity(const QString& message);
    void updateDeviceControls();

    QComboBox* deviceCombo_ = nullptr;
    QLabel* adbStatusLabel_ = nullptr;
    QLabel* agentStatusLabel_ = nullptr;
    QPushButton* agentActionButton_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QPushButton* adbTerminalButton_ = nullptr;
    QTimer* deviceRefreshTimer_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    SerialWorkspace* serialWorkspace_ = nullptr;
    QLabel* activityLabel_ = nullptr;
    QLabel* agentDeviceValue_ = nullptr;
    QPushButton* deployAgentButton_ = nullptr;
    QPushButton* disconnectAgentButton_ = nullptr;
    QPushButton* stopAgentButton_ = nullptr;
    QLabel* agentConnectionDetail_ = nullptr;
    QLabel* agentVersionValue_ = nullptr;
    QLabel* agentCapabilitiesValue_ = nullptr;
    QLabel* agentModelValue_ = nullptr;
    QLabel* agentManufacturerValue_ = nullptr;
    QLabel* agentHardwareValue_ = nullptr;
    QLabel* agentOsValue_ = nullptr;
    QLabel* agentBuildValue_ = nullptr;
    QLabel* agentSecurityValue_ = nullptr;
    std::shared_ptr<testtools::IAgentConnection> agentClient_;
    std::vector<QPushButton*> navigationButtons_;
    testtools::BackgroundTaskGroup scanTasks_;
    testtools::BackgroundTaskGroup agentTasks_;
    testtools::BackgroundTaskGroup terminalTasks_;
    QString agentSummary_ = QStringLiteral("未连接");
    QString lastDeviceScanSignature_;
    std::string agentAdbSerial_;
    std::uint16_t agentForwardPort_ = 0;
    std::uint64_t agentGeneration_ = 0;
    bool scanning_ = false;
    bool terminalLaunching_ = false;
    bool agentConnecting_ = false;
    bool serialBusy_ = false;
    bool shuttingDown_ = false;
};
