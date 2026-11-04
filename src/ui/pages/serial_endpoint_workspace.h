#pragma once

#include "application/serial/serial_preferences.h"

#include "application/agent/agent_service_status.h"
#include "application/serial/at_command_library.h"
#include "application/serial/serial_port_claims.h"
#include "application/serial/serial_preferences_writer.h"
#include "application/serial/serial_receive_queue.h"
#include "application/serial/serial_session.h"
#include "application/serial/serial_workspace_state.h"
#include "application/support/background_task_group.h"
#include "core/serial/serial_test_engine.h"

#include <QtCore/QString>
#include <QtWidgets/QWidget>

#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
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

class QVBoxLayout;

// 两种实际页面共用端点连接、日志、录制与关闭规则；不拥有终端解析或双端测试状态。
class SerialEndpointWorkspace : public QWidget {
    Q_OBJECT
    friend class MainWindowUiChecks;
  public:
    ~SerialEndpointWorkspace() override;
    void setAdbDevice(const testtools::AdbDevice& device);
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
    virtual bool selectWorkspaceMode(const QString& modeName) = 0;
    virtual QString currentWorkspaceMode() const = 0;
    bool setWorkspaceModeForDiagnostics(const QString& modeName);
    struct BackgroundTask {
        enum class Kind { Recording, ScheduledSend, Stress };
        Kind kind;
        int channel = 0;
        std::uint64_t token = 0;
        QString title;
        bool stoppable = true;
    };
    virtual std::vector<BackgroundTask> activeTasks() const;
    virtual void stopTask(const BackgroundTask& task);
    void stopRecordings();
    bool hasPendingRecordings() const;
    void flushPreferences();
    bool hasPendingPreferences() const;
    bool hasPendingBackgroundWork() const
    {
        return localScanning_ || hasPendingPreferences();
    }

  signals:
    void prepareServiceRequested();
    void workspaceModeChanged(const QString& modeName);
    void busyChanged(bool busy);
    void activityChanged(const QString& message);

  protected:
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
        QWidget* receiveWidget = nullptr;
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
        int autoIntervalMs = 1000;
        std::uint64_t autoTargetCount = 100;
        bool autoEcho = true;
        bool disconnectPending = false;
        bool endpointManuallyEdited = false;
        bool connectedOnce = false;
        QWidget* feedbackPanel = nullptr;
        QLabel* feedbackLabel = nullptr;
        QPushButton* feedbackRetry = nullptr;
        QString portFeedback, connectionFeedback, recordingFeedback;
    };

    explicit SerialEndpointWorkspace(QWidget* parent,
                                     int channelCount,
                                     std::shared_ptr<testtools::SerialPortClaims> claims);
    bool
    initializePreferences(const wchar_t* filename, bool migrateTerminal, int defaultLineEnding = 0);
    void finishSetup();
    void installSearchShortcuts();
    void buildPageHeader(const QString& title, const QString& description);
    QWidget* buildChannelPanel(int index);
    void applyChannelLayout(bool single);
    void applyPreferencesToUi();
    void savePreferences();
    std::size_t preferenceIndex(int index) const
    {
        return isTerminal() ? 1 : std::size_t(index);
    }
    int primaryChannelIndex() const
    {
        return isTerminal() ? 0 : 1;
    }
    void refreshLocalPorts(bool announce = false, int requestedChannel = -1);
    void refreshAgentPorts();
    void applyPorts(int index, bool preserveEditedEndpoint = true,
                    bool explicitRefresh = false,
                    const std::vector<testtools::PortInfo>* previousPorts = nullptr);
    void applyLocalPorts(std::vector<testtools::PortInfo> ports);
    void updateChannelFeedback(int index);
    void showPageNotice(const QString& message);
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
    QString channelDisplayName(int index) const;
    bool channelActive(int index) const;
    bool channelUsesAgent(int index) const;
    bool selectedAgentIsOnline() const;
    bool channelReady(int index) const;
    bool resourceConflict() const;
    bool validateChannel(int index, QString& error) const;
    testtools::SerialEndpointSelection endpointSelection(int index) const;
    testtools::SerialConfig channelConfig(int index) const;
    bool sendBytes(int index, const testtools::ByteBuffer& data, bool echo = true);
    void setControlLine(int index, testtools::SerialControlLine line, bool enabled);
    void appendTransmitted(int index, const testtools::ByteBuffer& data);
    virtual void appendReceived(int index, const testtools::ByteBuffer& data);
    void appendNotice(int index, const QString& message);
    virtual void renderReceiveLog(int index);
    virtual void applyLogAppendResult(int index, const testtools::SerialLogAppendResult& result);
    virtual void updateSearchMatches(int index);
    virtual void findLogMatch(int index, int step);
    void scheduleSearchRefresh(int index);
    void updateCounter(int index);
    void resetChannelCounters(int index);
    void saveChannelLog(int index);
    bool eventFilter(QObject* watched, QEvent* event) override;
    virtual bool isTerminal() const = 0;
    virtual bool singleChannelView() const
    {
        return true;
    }
    virtual bool controlTasksRunning() const
    {
        return false;
    }
    virtual bool stressRunning() const
    {
        return false;
    }
    virtual void updatePageControls() {}
    virtual void updateRouteSummary() {}
    virtual void updateLayout() {}
    virtual void buildSendArea(int, QWidget*, QHBoxLayout*, QVBoxLayout*) {}
    virtual void addChannelBody(QVBoxLayout* layout, int index);
    virtual void buildReceiveOptions(QHBoxLayout*) {}
    virtual QWidget* createReceiveView(int index, QWidget* parent);
    virtual QString receiveText(int index) const;
    virtual void followOutput(int index);
    virtual void setHighlightEnabled(int index, bool enabled);
    virtual void setupReceiveInput(int) {}
    virtual bool handleReceiveEvent(QObject*, QEvent*)
    {
        return false;
    }
    virtual void clearReceive(int index);
    virtual bool historyTruncated(int index) const;
    virtual void streamReset(int) {}
    virtual void channelDisconnected(int) {}
    virtual void sendFailed(int) {}
    virtual void displayGap(int) {}
    virtual void stopPageTasks() {}
    virtual void focusTerminal() {}

    std::deque<ChannelUi> channels_;
    QLabel* pageTitle_ = nullptr;
    QLabel* modeDescriptionLabel_ = nullptr;
    QTimer* recordingTimer_ = nullptr;
    QTimer* preferenceSaveTimer_ = nullptr;
    QTimer* pageNoticeTimer_ = nullptr;
    QString defaultDescription_;
    testtools::AgentServiceState serviceState_ = testtools::AgentServiceState::Disconnected;
    bool maintenanceBlocked_ = false;
    QString serviceDetail_;
    bool serviceCleanupPending_ = false;
    testtools::AdbDevice currentAdbDevice_;
    std::vector<testtools::PortInfo> localPorts_;
    std::vector<testtools::AgentSerialPort> agentPorts_;
    std::string agentAdbSerial_;
    std::uint16_t agentControlPort_ = 0;
    std::uint64_t agentPortGeneration_ = 0;
    testtools::BackgroundTaskGroup scanTasks_;
    testtools::BackgroundTaskGroup controlTasks_;
    testtools::BackgroundTaskGroup localScanTasks_;
    std::function<std::vector<testtools::PortInfo>()> enumerateLocalPorts_;
    std::uint64_t localScanGeneration_ = 0;
    bool localScanning_ = false;
    std::array<bool, 2> localRefreshRequested_{};
    bool scanning_ = false;
    bool shuttingDown_ = false;
    testtools::SerialWorkspacePreferences preferences_;
    std::filesystem::path preferencesStorePath_;
    std::unique_ptr<testtools::SerialPreferencesWriter> preferencesWriter_;
    QString preferencesError_;
    bool preferencesDirty_ = false;
    bool preferencesLoaded_ = false;
    bool applyingPreferences_ = false;
    std::shared_ptr<testtools::SerialPortClaims> portClaims_;
};
