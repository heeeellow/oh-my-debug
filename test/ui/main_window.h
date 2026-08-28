#pragma once

#include "application/device_discovery.h"

#include <QtWidgets/QMainWindow>

#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QProcess;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QWidget;
class SerialWorkspace;
namespace testtools { class AgentClient; }

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;
    bool navigateTo(const QString &pageName);
    void startAgentDemo();

private:
    enum class StatusTone {
        Neutral,
        Good,
        Warning,
        Error
    };

    enum PageIndex {
        OverviewPage = 0,
        SerialPage,
        TestCenterPage,
        HistoryPage,
        SettingsPage
    };

    void buildUi();
    QWidget *buildTopBar();
    QWidget *buildNavigation();
    QWidget *buildOverviewPage();
    QWidget *buildSerialPage();
    QWidget *buildTestCenterPage();
    QWidget *buildSettingsPage();
    QWidget *buildEmptyPage(const QString &title, const QString &subtitle,
                            const QString &heading, const QString &body);
    QWidget *wrapPage(const QString &title, const QString &subtitle, QWidget *content,
                      bool scrollable = true);
    QPushButton *addNavigationButton(const QString &text, int pageIndex);

    void refreshDevices();
    void startLocalFakeAgent();
    void connectAgentEndpoint();
    void disconnectAgent(const QString &reason = {});
    void setAgentStatus(const QString &text, StatusTone tone, const QString &detail = {});
    void updateAgentControls();
    void prepareForShutdown();
    void applyDeviceScan(testtools::DeviceScanResult result);
    void applySelectedDevice();
    serialdemo::AdbDevice selectedDevice() const;
    void showPage(int pageIndex);
    void setAdbStatus(const QString &text, StatusTone tone = StatusTone::Good);
    void setActivity(const QString &message);
    void updateOverview();
    void updateDeviceControls();

    QComboBox *deviceCombo_ = nullptr;
    QLabel *adbStatusLabel_ = nullptr;
    QLabel *agentStatusLabel_ = nullptr;
    QPushButton *agentActionButton_ = nullptr;
    QPushButton *refreshButton_ = nullptr;
    QStackedWidget *pages_ = nullptr;
    SerialWorkspace *serialWorkspace_ = nullptr;
    QLabel *activityLabel_ = nullptr;
    QLabel *overviewDeviceValue_ = nullptr;
    QLabel *overviewAdbValue_ = nullptr;
    QLabel *overviewAgentValue_ = nullptr;
    QLineEdit *agentHostEdit_ = nullptr;
    QSpinBox *agentPortSpin_ = nullptr;
    QPushButton *startFakeAgentButton_ = nullptr;
    QPushButton *connectAgentButton_ = nullptr;
    QPushButton *disconnectAgentButton_ = nullptr;
    QLabel *agentConnectionDetail_ = nullptr;
    QLabel *agentCapabilitiesValue_ = nullptr;
    QProcess *fakeAgentProcess_ = nullptr;
    std::shared_ptr<testtools::AgentClient> agentClient_;
    std::vector<QPushButton *> navigationButtons_;
    std::vector<std::thread> scanThreads_;
    std::vector<std::thread> agentThreads_;
    QString fakeAgentOutput_;
    QString agentSummary_ = QStringLiteral("未连接");
    std::uint64_t agentGeneration_ = 0;
    bool scanning_ = false;
    bool agentConnecting_ = false;
    bool serialBusy_ = false;
    bool shuttingDown_ = false;
};
