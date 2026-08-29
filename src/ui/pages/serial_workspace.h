#pragma once

#include "core/serial_transport.h"
#include "core/serial_test_engine.h"
#include "core/async_serial_sender.h"
#include "core/agent_serial_transport.h"

#include <QtCore/QString>
#include <QtWidgets/QWidget>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTimer;
class QWidget;

class SerialWorkspace final : public QWidget
{
    Q_OBJECT

public:
    explicit SerialWorkspace(QWidget *parent = nullptr);
    ~SerialWorkspace() override;

    void setAdbDevice(const serialdemo::AdbDevice &device);
    void setAgentConnection(const std::string &adbSerial, std::uint16_t localControlPort);
    void refreshTargets();
    void prepareForShutdown();
    bool hasActiveConnections() const;

signals:
    void busyChanged(bool busy);
    void activityChanged(const QString &message);

private:
    struct ChannelUi {
        QGroupBox *group = nullptr;
        QComboBox *sourceCombo = nullptr;
        QComboBox *portCombo = nullptr;
        QComboBox *baudCombo = nullptr;
        QComboBox *dataBitsCombo = nullptr;
        QComboBox *parityCombo = nullptr;
        QComboBox *stopBitsCombo = nullptr;
        QComboBox *flowControlCombo = nullptr;
        QPushButton *connectButton = nullptr;
        QPlainTextEdit *receiveEdit = nullptr;
        QPlainTextEdit *sendEdit = nullptr;
        QCheckBox *receiveHexCheck = nullptr;
        QCheckBox *sendHexCheck = nullptr;
        QCheckBox *timestampCheck = nullptr;
        QComboBox *lineEndingCombo = nullptr;
        QPushButton *sendButton = nullptr;
        QPushButton *autoSendButton = nullptr;
        QLabel *statusLabel = nullptr;
        QLabel *counterLabel = nullptr;

        std::unique_ptr<serialdemo::ISerialTransport> transport;
        serialdemo::AsyncSerialSender sender;
        std::uint64_t receivedBytes = 0;
        std::uint64_t sentBytes = 0;
        bool connecting = false;
        std::string expectedFrame;
        std::string detectionBuffer;
        serialdemo::ByteBuffer autoSendPayload;
        serialdemo::PeriodicSendPlan autoSendPlan;
        int autoIntervalMs = 1000;
        std::uint64_t autoTargetCount = 100;
        bool autoEcho = true;
        bool lineEndingCustomized = false;
        std::uint64_t senderGeneration = 0;
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
    QWidget *buildChannelPanel(int index);
    void refreshAgentPorts();
    void applyPorts(int index);
    void connectChannel(int index);
    void disconnectChannel(int index, bool showClosedStatus = true);
    void disconnectAll();
    void attachTransportCallbacks(int index);
    void startSender(int index);
    void setChannelControlsEnabled(int index, bool enabled);
    void updateGlobalControls();
    void setChannelStatus(int index, const QString &message, bool error = false);
    void updateRouteSummary();
    bool channelUsesAgent(int index) const;
    bool selectedAgentIsOnline() const;
    bool validateChannel(int index, QString &error) const;
    serialdemo::SerialConfig channelConfig(int index) const;
    serialdemo::ByteBuffer manualPayload(int index, bool &ok);
    bool sendBytes(int index, const serialdemo::ByteBuffer &data, bool echo = true);
    void sendManual(int index);
    void toggleAutoSend(int index);
    void stopAutoSend(int index, bool announce = false);
    void processScheduledSends();
    void appendTransmitted(int index, const serialdemo::ByteBuffer &data);
    void appendReceived(int index, const serialdemo::ByteBuffer &data);
    void updateCounter(int index);
    void runBidirectionalTest();
    void updateBidirectionalResult(bool timedOut = false);
    void showStressDialog();
    void startStressTest(int direction, std::uint64_t frameCount,
                         std::size_t payloadSize, int intervalMs);
    void stopStressTest(const QString &reason, bool completed = false);
    void finishStressTest(std::uint64_t generation);
    QString stressDirectionSummary(const StressDirection &direction) const;

    QPushButton *connectBothButton_ = nullptr;
    QPushButton *disconnectBothButton_ = nullptr;
    QPushButton *bidirectionalTestButton_ = nullptr;
    QPushButton *stressTestButton_ = nullptr;
    QLabel *testResultLabel_ = nullptr;
    QLabel *stressResultLabel_ = nullptr;
    QLabel *routeLabel_ = nullptr;
    QTimer *scheduleTimer_ = nullptr;

    std::array<ChannelUi, 2> channels_;
    serialdemo::AdbDevice currentAdbDevice_;
    std::vector<serialdemo::PortInfo> localPorts_;
    std::vector<serialdemo::AgentSerialPort> agentPorts_;
    std::string agentAdbSerial_;
    std::uint16_t agentControlPort_ = 0;
    std::vector<std::thread> scanThreads_;
    bool scanning_ = false;
    bool aToBPassed_ = false;
    bool bToAPassed_ = false;
    bool bidirectionalTestRunning_ = false;
    bool stressTestRunning_ = false;
    bool stressAwaitingReceive_ = false;
    bool shuttingDown_ = false;
    std::array<StressDirection, 2> stressDirections_;
    std::uint64_t testGeneration_ = 0;
    std::uint64_t stressGeneration_ = 0;
};
