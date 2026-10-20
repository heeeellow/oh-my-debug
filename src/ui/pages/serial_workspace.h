#pragma once

#include "application/serial/at_command_library.h"
#include "application/serial/serial_session.h"
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
class QComboBox;
class QEvent;
class QFrame;
class QGroupBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSplitter;
class QSpinBox;
class QTimer;
class QWidget;

class SerialWorkspace final : public QWidget {
    Q_OBJECT

  public:
    explicit SerialWorkspace(QWidget* parent = nullptr);
    ~SerialWorkspace() override;

    void setAdbDevice(const serialdemo::AdbDevice& device);
    void setAgentConnection(const std::string& adbSerial, std::uint16_t localControlPort);
    void refreshTargets();
    void prepareForShutdown();
    bool hasActiveConnections() const;
    bool setWorkspaceModeForDiagnostics(const QString& modeName);

  signals:
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
        QLabel* sendShortcutHint = nullptr;
        QPushButton* toggleAtBarButton = nullptr;
        QWidget* manualSendArea = nullptr;
        QWidget* atCommandArea = nullptr;
        QComboBox* atCommandCombo = nullptr;
        QPushButton* fillAtCommandButton = nullptr;
        QPushButton* sendAtCommandButton = nullptr;

        testtools::SerialSession session;
        testtools::SerialLogBuffer logBuffer;
        std::string expectedFrame;
        std::string detectionBuffer;
        int autoIntervalMs = 1000;
        std::uint64_t autoTargetCount = 100;
        bool autoEcho = true;
        bool disconnectPending = false;
    };

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
    void refreshAgentPorts();
    void applyPorts(int index);
    void applyPreferencesToUi();
    void savePreferences();
    void connectChannel(int index);
    void disconnectChannel(int index, bool showClosedStatus = true);
    void completeChannelDisconnect(int index, std::uint64_t generation, bool showClosedStatus);
    void disconnectAll();
    void attachSessionCallbacks(int index);
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
    void sendAtCommand(int index);
    void fillAtCommandToInput(int index);
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
    QTimer* scheduleTimer_ = nullptr;

    std::array<QPushButton*, 2> modeButtons_{};
    std::array<ChannelUi, 2> channels_;
    serialdemo::AdbDevice currentAdbDevice_;
    std::vector<serialdemo::PortInfo> localPorts_;
    std::vector<serialdemo::AgentSerialPort> agentPorts_;
    std::string agentAdbSerial_;
    std::uint16_t agentControlPort_ = 0;
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
    std::array<StressDirection, 2> stressDirections_;
    std::uint64_t testGeneration_ = 0;
    std::uint64_t stressGeneration_ = 0;
};
