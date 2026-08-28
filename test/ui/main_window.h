#pragma once

#include "application/device_discovery.h"

#include <QtWidgets/QMainWindow>

#include <thread>
#include <vector>

class QComboBox;
class QLabel;
class QPushButton;
class QStackedWidget;
class QWidget;
class SerialWorkspace;

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;
    bool navigateTo(const QString &pageName);

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
    QWidget *buildEmptyPage(const QString &title, const QString &subtitle,
                            const QString &heading, const QString &body);
    QWidget *wrapPage(const QString &title, const QString &subtitle, QWidget *content,
                      bool scrollable = true);
    QPushButton *addNavigationButton(const QString &text, int pageIndex);

    void refreshDevices();
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
    QPushButton *refreshButton_ = nullptr;
    QStackedWidget *pages_ = nullptr;
    SerialWorkspace *serialWorkspace_ = nullptr;
    QLabel *activityLabel_ = nullptr;
    QLabel *overviewDeviceValue_ = nullptr;
    QLabel *overviewAdbValue_ = nullptr;
    QLabel *overviewAgentValue_ = nullptr;
    std::vector<QPushButton *> navigationButtons_;
    std::vector<std::thread> scanThreads_;
    bool scanning_ = false;
    bool serialBusy_ = false;
    bool shuttingDown_ = false;
};
