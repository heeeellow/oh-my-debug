#pragma once
#include "application/serial/serial_link_verifier.h"
#include "serial_endpoint_workspace.h"

// 单/双串口调试页，终端是独立页面；这里只调度普通发送及双端验证、压测的表现。
class SerialWorkspace final : public SerialEndpointWorkspace {
    Q_OBJECT
  public:
    explicit SerialWorkspace(QWidget* parent = nullptr,
                             std::shared_ptr<testtools::SerialPortClaims> claims = {});
    ~SerialWorkspace() override;
    bool selectWorkspaceMode(const QString& modeName) override;
    QString currentWorkspaceMode() const override;
    std::vector<BackgroundTask> activeTasks() const override;
    void stopTask(const BackgroundTask& task) override;

  private:
    using WorkspaceMode = testtools::SerialWorkspaceMode;
    bool isTerminal() const override
    {
        return false;
    }
    bool singleChannelView() const override
    {
        return workspaceState_.mode() != WorkspaceMode::Dual;
    }
    bool controlTasksRunning() const override
    {
        return linkVerifier_.probe().running || linkVerifier_.stressRunning();
    }
    bool stressRunning() const override
    {
        return linkVerifier_.stressRunning();
    }
    void buildUi();
    void buildSendArea(int index,
                       QWidget* header,
                       QHBoxLayout* receiveHeader,
                       QVBoxLayout* body) override;
    void addChannelBody(QVBoxLayout* layout, int index) override;
    void setWorkspaceMode(WorkspaceMode mode);
    bool canSwitchWorkspaceMode(WorkspaceMode mode);
    void applyWorkspaceMode();
    void resizeEvent(QResizeEvent* event) override;
    void updateLayout() override;
    void updatePageControls() override;
    void updateRouteSummary() override;
    bool handleReceiveEvent(QObject* watched, QEvent* event) override;
    void channelDisconnected(int index) override;
    void sendFailed(int index) override;
    void displayGap(int index) override;
    void stopPageTasks() override;
    void appendReceived(int index, const serialdemo::ByteBuffer& data) override;
    serialdemo::ByteBuffer manualPayload(int index, bool& ok);
    void sendManual(int index);
    void sendAtCommand(int index, int commandIndex);
    void fillAtCommandToInput(int index, int commandIndex);
    void showAtCommandManager();
    void refreshAtCommandChoices();
    bool saveAtCommands();
    void importAtCommands();
    void exportAtCommands();
    void toggleAutoSend(int index);
    void stopAutoSend(int index, bool announce = false);
    void processScheduledSends();
    void updateScheduleTimer();
    void runBidirectionalTest();
    void updateBidirectionalResult(bool timedOut = false);
    void showStressDialog();
    void startStressTest(int direction,
                         std::uint64_t frameCount,
                         std::size_t payloadSize,
                         int intervalMs);
    void stopStressTest(const QString& reason, bool completed = false);
    void finishStressTest(std::uint64_t generation);
    QString
    stressDirectionSummary(const testtools::SerialLinkVerifier::StressDirection& direction) const;
    QPushButton* connectBothButton_ = nullptr;
    QPushButton* disconnectBothButton_ = nullptr;
    QPushButton* bidirectionalTestButton_ = nullptr;
    QPushButton* stressTestButton_ = nullptr;
    QLabel* testResultLabel_ = nullptr;
    QLabel* stressResultLabel_ = nullptr;
    QLabel* routeLabel_ = nullptr;
    QFrame* linkBar_ = nullptr;
    QFrame* resultsPanel_ = nullptr;
    QSplitter* channelSplitter_ = nullptr;
    QSplitter* shortcutSplitter_ = nullptr;
    SerialShortcutSidebar* shortcutSidebar_ = nullptr;
    bool updatingShortcutLayout_ = false;
    bool forceShortcutOpen_ = false;
    QTimer* scheduleTimer_ = nullptr;
    testtools::SerialWorkspaceState workspaceState_;
    testtools::AtCommandLibrary atCommandLibrary_;
    std::filesystem::path atCommandStorePath_;
    testtools::SerialLinkVerifier linkVerifier_;
};
