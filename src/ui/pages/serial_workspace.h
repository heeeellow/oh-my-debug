#pragma once

#include "application/agent/agent_service_status.h"
#include "application/serial/at_command_library.h"
#include "application/serial/serial_session.h"
#include "application/serial/serial_receive_queue.h"
#include "application/serial/serial_port_claims.h"
#include "application/serial/serial_terminal.h"
#include "application/serial/serial_workspace_state.h"
#include "application/support/background_task_group.h"
#include "core/serial/serial_test_engine.h"

#include <QtCore/QString>
#include <QtWidgets/QWidget>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

class QCheckBox;
class QAction;
class QComboBox;
class QEvent;
class QFrame;
class QGroupBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QKeyEvent;
class QPlainTextEdit;
class QPushButton;
class QSplitter;
class QSpinBox;
class QTimer;
class QWidget;
class QResizeEvent;
class SerialShortcutSidebar;
class SerialKeywordHighlighter;

// 串口工作区（支持单口/双口调试与独立交互终端模式，复用底层会话、占用锁与核验引擎）。
class SerialWorkspace final : public QWidget {
    Q_OBJECT

  public:
    enum class Purpose { Debug, Terminal };
    explicit SerialWorkspace(QWidget* parent = nullptr,
                             Purpose purpose = Purpose::Debug,
                             std::shared_ptr<testtools::SerialPortClaims> claims = {});
    ~SerialWorkspace() override;

    void setAdbDevice(const serialdemo::AdbDevice& device);
    void setAgentConnection(const std::string& adbSerial, std::uint16_t localControlPort);
    void refreshTargets();
    void setServiceStatus(testtools::AgentServiceState state,
                          const QString& detail,
                          bool cleanupPending = false);
    void prepareForShutdown();
    bool hasActiveConnections() const;
    bool hasActiveAgentConnection() const;
    bool hasActiveAgentConnection(const std::string& serial) const;
    void setDeviceMaintenance(bool blocked);
    bool selectWorkspaceMode(const QString& modeName);
    QString currentWorkspaceMode() const;
    bool setWorkspaceModeForDiagnostics(const QString& modeName);
    struct BackgroundTask {
        enum class Kind { Recording, ScheduledSend, Stress };
        Kind kind;
        int channel = 0;
        std::uint64_t token = 0;
        QString title;
        bool stoppable = true;
    };
    std::vector<BackgroundTask> activeTasks() const;
    void stopTask(const BackgroundTask& task);
    void stopRecordings();
    bool hasPendingRecordings() const;

  signals:
    void prepareServiceRequested();
    void workspaceModeChanged(const QString& modeName);
    void busyChanged(bool busy);
    void activityChanged(const QString& message);

  private:
    using WorkspaceMode = testtools::SerialWorkspaceMode;

    struct ChannelUi {
        QGroupBox* group = nullptr;
        QWidget* headerWidget = nullptr;
        QHBoxLayout* headerLayout = nullptr;
        QHBoxLayout* receiveHeaderLayout = nullptr;
        QHBoxLayout* counterRowLayout = nullptr;
        QWidget* counterRow = nullptr;
        QHBoxLayout* settingsRowLayout = nullptr;
        QWidget* actionsContainer = nullptr;
        QLabel* badgeLabel = nullptr;
        QLabel* titleLabel = nullptr;
        QComboBox* sourceCombo = nullptr;
        QComboBox* portCombo = nullptr;
        QComboBox* baudCombo = nullptr;
        QComboBox* dataBitsCombo = nullptr;
        QComboBox* parityCombo = nullptr;
        QComboBox* stopBitsCombo = nullptr;
        QComboBox* flowControlCombo = nullptr;
        QCheckBox* dtrCheck = nullptr;
        QCheckBox* rtsCheck = nullptr;
        QPushButton* refreshPortsButton = nullptr;
        QPushButton* advancedSettingsButton = nullptr;
        QWidget* advancedSettingsArea = nullptr;
        QPushButton* connectButton = nullptr;
        QPlainTextEdit* receiveEdit = nullptr;
        QPlainTextEdit* sendEdit = nullptr;
        QCheckBox* receiveHexCheck = nullptr;
        QCheckBox* sendHexCheck = nullptr;
        QCheckBox* timestampCheck = nullptr;
        QCheckBox* autoScrollCheck = nullptr;
        QPushButton* resetCounterButton = nullptr;
        QPushButton* saveLogButton = nullptr;
        QPushButton* receiveMoreButton = nullptr;
        QPushButton* recordButton = nullptr;
        QPushButton* highlightButton = nullptr;
        SerialKeywordHighlighter* highlighter = nullptr;
        std::shared_ptr<testtools::SerialReceiveQueue> receiveQueue =
            std::make_shared<testtools::SerialReceiveQueue>();
        std::uint64_t displayDroppedBytes = 0;
        std::uint64_t displayGeneration = 0;
        QString recordAnnouncement;
        bool receivingBatch = false;
        testtools::SerialLogAppendResult receivedAppend;
        std::array<QAction*, 3> displayActions{};
        QPushButton* clearReceiveButton = nullptr;
        QPushButton* clearSendButton = nullptr;
        QPushButton* searchButton = nullptr;
        QWidget* searchArea = nullptr;
        QLineEdit* searchEdit = nullptr;
        QCheckBox* filterCheck = nullptr;
        QLabel* matchCountLabel = nullptr;
        QLabel* truncatedLabel = nullptr;
        QPushButton* previousMatchButton = nullptr;
        QPushButton* nextMatchButton = nullptr;
        QTimer* searchTimer = nullptr;
        std::vector<int> searchMatches;
        int currentMatch = -1;
        bool filteredRenderPending = false;
        bool controlPending = false;
        std::array<Qt::CheckState, 2> lineStates{Qt::PartiallyChecked, Qt::PartiallyChecked};
        QComboBox* lineEndingCombo = nullptr;
        QPushButton* sendButton = nullptr;
        QPushButton* autoSendButton = nullptr;
        QLabel* statusLabel = nullptr;
        QLabel* counterLabel = nullptr;
        QLabel* receiveTitle = nullptr;
        QLabel* sendShortcutHint = nullptr;
        QPushButton* toggleAtBarButton = nullptr;
        QWidget* manualSendArea = nullptr;
        QWidget* composerFrame = nullptr;
        QWidget* serviceHint = nullptr;
        QLabel* serviceHintLabel = nullptr;
        QPushButton* serviceConnectButton = nullptr;
        QWidget* logSendBody = nullptr;

        std::shared_ptr<testtools::SerialPortClaims::Claim> portClaim;
        testtools::SerialSession session;
        testtools::SerialLogBuffer logBuffer;
        // 双向验证的期望令牌与接收检测滑窗（具体匹配逻辑委托给 core::appendAndMatchFrame）。
        std::string expectedFrame;
        std::string detectionBuffer;
        int autoIntervalMs = 1000;
        std::uint64_t autoTargetCount = 100;
        bool autoEcho = true;
        bool disconnectPending = false;
    };

    // 单向压测运行时状态（线速间隔推算与最终统计判定委托给 core::serial_test_engine）。
    struct StressDirection {
        bool active = false;
        int transmitter = 0;
        int receiver = 1;
        std::size_t payloadSize = 256;
        std::uint32_t nextSequence = 0;
        serialdemo::PeriodicSendPlan sendPlan;
        serialdemo::IntegrityStreamVerifier verifier;
    };

    void buildUi();
    QWidget* buildChannelPanel(int index);
    bool eventFilter(QObject* watched, QEvent* event) override;
    void setWorkspaceMode(WorkspaceMode mode);
    bool canSwitchWorkspaceMode(WorkspaceMode mode);
    void applyWorkspaceMode();
    void resizeEvent(QResizeEvent* event) override;
    void updateShortcutLayout();
    void refreshAgentPorts();
    void applyPorts(int index, bool preserveEditedEndpoint = true);
    void applyPreferencesToUi();
    void savePreferences();
    void connectChannel(int index);
    void disconnectChannel(int index, bool showClosedStatus = true);
    void completeChannelDisconnect(int index, std::uint64_t generation, bool showClosedStatus);
    void disconnectAll();
    void attachSessionCallbacks(int index);
    void drainReceived(int index);
    void toggleRecording(int index);
    void pollRecordings();
    void updateTruncation(int index);
    void setChannelControlsEnabled(int index, bool enabled);
    void updateGlobalControls();
    void setChannelStatus(int index, const QString& message, bool error = false);
    void updateRouteSummary();
    QString channelDisplayName(int index) const;
    bool channelActive(int index) const;
    bool channelUsesAgent(int index) const;
    bool selectedAgentIsOnline() const;
    bool validateChannel(int index, QString& error) const;
    testtools::SerialEndpointSelection endpointSelection(int index) const;
    serialdemo::SerialConfig channelConfig(int index) const;
    serialdemo::ByteBuffer manualPayload(int index, bool& ok);
    bool sendBytes(int index, const serialdemo::ByteBuffer& data, bool echo = true);
    void sendManual(int index);
    void sendAtCommand(int index, int commandIndex);
    void fillAtCommandToInput(int index, int commandIndex);
    void showAtCommandManager();
    void refreshAtCommandChoices();
    bool saveAtCommands();
    void importAtCommands();
    void exportAtCommands();
    void setControlLine(int index, serialdemo::SerialControlLine line, bool enabled);
    void toggleAutoSend(int index);
    void stopAutoSend(int index, bool announce = false);
    void processScheduledSends();
    void appendTransmitted(int index, const serialdemo::ByteBuffer& data);
    void appendReceived(int index, const serialdemo::ByteBuffer& data);
    void appendNotice(int index, const QString& message);
    void renderReceiveLog(int index);
    void updateSearchMatches(int index);
    void findLogMatch(int index, int step);
    void scheduleSearchRefresh(int index);
    void applyLogAppendResult(int index, const testtools::SerialLogAppendResult& result);
    void updateCounter(int index);
    void resetChannelCounters(int index);
    void saveChannelLog(int index);
    void buildTerminalOptions(QHBoxLayout* header);
    bool handleTerminalKey(QKeyEvent* event);
    void focusTerminal();
    void resumeTerminalInput();
    void updateTerminalCursor(bool follow);
    void renderTerminal(bool fullRedraw = false);
    void scheduleTerminalRender();
    serialdemo::LineEnding terminalLineEnding() const;
    void runBidirectionalTest();
    void updateBidirectionalResult(bool timedOut = false);
    void showStressDialog();
    void startStressTest(int direction,
                         std::uint64_t frameCount,
                         std::size_t payloadSize,
                         int intervalMs);
    void stopStressTest(const QString& reason, bool completed = false);
    void finishStressTest(std::uint64_t generation);
    QString stressDirectionSummary(const StressDirection& direction) const;

    QPushButton* connectBothButton_ = nullptr;
    QPushButton* disconnectBothButton_ = nullptr;
    QPushButton* bidirectionalTestButton_ = nullptr;
    QPushButton* stressTestButton_ = nullptr;
    QLabel* testResultLabel_ = nullptr;
    QLabel* stressResultLabel_ = nullptr;
    QLabel* routeLabel_ = nullptr;
    QLabel* modeDescriptionLabel_ = nullptr;
    QFrame* linkBar_ = nullptr;
    QFrame* resultsPanel_ = nullptr;
    QSplitter* channelSplitter_ = nullptr;
    QSplitter* shortcutSplitter_ = nullptr;
    SerialShortcutSidebar* shortcutSidebar_ = nullptr;
    bool updatingShortcutLayout_ = false;
    bool forceShortcutOpen_ = false;
    QTimer* scheduleTimer_ = nullptr;
    QTimer* recordingTimer_ = nullptr;

    testtools::AgentServiceState serviceState_ = testtools::AgentServiceState::Disconnected;
    bool maintenanceBlocked_ = false;
    QString serviceDetail_;
    bool serviceCleanupPending_ = false;
    QLabel* pageTitle_ = nullptr;
    QPushButton* terminalOptionsButton_ = nullptr;
    std::array<QAction*, 3> terminalEndingActions_{};
    std::array<QAction*, 2> terminalBackspaceActions_{};
    QTimer* terminalRenderTimer_ = nullptr;
    testtools::SerialTerminalBuffer terminalBuffer_;
    std::array<ChannelUi, 2> channels_;
    serialdemo::AdbDevice currentAdbDevice_;
    std::vector<serialdemo::PortInfo> localPorts_;
    std::vector<serialdemo::AgentSerialPort> agentPorts_;
    std::string agentAdbSerial_;
    std::uint16_t agentControlPort_ = 0;
    std::uint64_t agentPortGeneration_ = 0;
    testtools::BackgroundTaskGroup scanTasks_;
    testtools::BackgroundTaskGroup controlTasks_;
    bool scanning_ = false;
    bool aToBPassed_ = false;
    bool bToAPassed_ = false;
    bool bidirectionalTestRunning_ = false;
    bool stressTestRunning_ = false;
    bool stressAwaitingReceive_ = false;
    bool shuttingDown_ = false;
    testtools::SerialWorkspaceState workspaceState_;
    testtools::AtCommandLibrary atCommandLibrary_;
    std::filesystem::path atCommandStorePath_;
    testtools::SerialWorkspacePreferences preferences_;
    std::filesystem::path preferencesStorePath_;
    bool preferencesLoaded_ = false;
    bool applyingPreferences_ = false;
    Purpose purpose_ = Purpose::Debug;
    std::shared_ptr<testtools::SerialPortClaims> portClaims_;
    std::array<StressDirection, 2> stressDirections_;
    std::uint64_t testGeneration_ = 0;
    std::uint64_t stressGeneration_ = 0;
};
