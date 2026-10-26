#pragma once

#include "application/agent/agent_service_status.h"
#include "application/device/device_discovery.h"
#include "application/support/background_task_group.h"
#include "application/serial/serial_port_claims.h"

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
class QToolButton;
class SerialWorkspace;
class FileTransferWorkspace;
class ScreenMirrorPanel;
namespace testtools {
class IAgentConnection;
}

// 应用主窗口：统一编排顶部设备/服务状态栏、左侧功能导航树、
// 串口调试工作区、独立串口终端、ADB 文件传输、屏幕镜像及连接与设备信息页。
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

    enum PageIndex { SerialPage = 0, DeviceInfoPage, FileTransferPage, MirrorPage, TerminalPage };

    void buildUi();
    QWidget* buildTopBar();
    QWidget* buildNavigation();
    QWidget* buildSerialPage(bool terminal = false);
    bool agentSerialBusy() const;
    QWidget* buildDeviceInfoPage();
    QWidget* buildFileTransferPage();
    QWidget* buildMirrorPage();
    QWidget* wrapAdbPage(const QString& title, QWidget* content);
    void setAdbExpanded(bool expanded);
    void setSerialExpanded(bool expanded);
    void updateNavigation();
    QWidget* wrapPage(const QString& title,
                      const QString& subtitle,
                      QWidget* content,
                      bool scrollable = true);
    QPushButton* addNavigationButton(const QString& text, int pageIndex, const QString& serialMode = {});

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
    void checkServiceHealth();
    void updateServicePresentation();
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
    QLabel* topServiceStatus_ = nullptr;
    QPushButton* topServiceButton_ = nullptr;
    QPushButton *serialGroupButton_ = nullptr, *adbGroupButton_ = nullptr;
    QTimer* serviceHealthTimer_ = nullptr;
    bool serviceProbeActive_ = false, agentCleanupPending_ = false;
    testtools::AgentServiceState serviceState_ = testtools::AgentServiceState::Disconnected;
    QString serviceDetail_;
    QPushButton* refreshButton_ = nullptr;
    QPushButton* adbTerminalButton_ = nullptr;
    std::vector<QPushButton*> terminalButtons_;
    QWidget* serialChildren_ = nullptr;
    QToolButton* serialExpandButton_ = nullptr;
    QWidget* adbChildren_ = nullptr;
    QToolButton* adbExpandButton_ = nullptr;
    int lastAdbPage_ = FileTransferPage;
    int lastSerialPage_ = SerialPage;
    bool closeRequested_ = false;
    bool fileShutdownComplete_ = false;
    FileTransferWorkspace* fileWorkspace_ = nullptr;
    ScreenMirrorPanel* screenMirrorPanel_ = nullptr;
    bool mirrorShutdownComplete_ = false;
    QTimer* deviceRefreshTimer_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    SerialWorkspace* serialWorkspace_ = nullptr;
    SerialWorkspace* terminalWorkspace_ = nullptr;
    std::shared_ptr<testtools::SerialPortClaims> portClaims_ =
        std::make_shared<testtools::SerialPortClaims>();
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
