#pragma once

#include "core/serial_transport.h"

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
class QWidget;

class SerialWorkspace final : public QWidget
{
    Q_OBJECT

public:
    explicit SerialWorkspace(QWidget *parent = nullptr);
    ~SerialWorkspace() override;

    void setAdbDevice(const serialdemo::AdbDevice &device);
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
        QCheckBox *adbRootCheck = nullptr;
        QPushButton *connectButton = nullptr;
        QPlainTextEdit *receiveEdit = nullptr;
        QPlainTextEdit *sendEdit = nullptr;
        QCheckBox *receiveHexCheck = nullptr;
        QCheckBox *sendHexCheck = nullptr;
        QCheckBox *timestampCheck = nullptr;
        QComboBox *lineEndingCombo = nullptr;
        QPushButton *sendButton = nullptr;
        QLabel *statusLabel = nullptr;
        QLabel *counterLabel = nullptr;

        std::unique_ptr<serialdemo::ISerialTransport> transport;
        std::uint64_t receivedBytes = 0;
        std::uint64_t sentBytes = 0;
        bool connecting = false;
        std::string expectedFrame;
        std::string detectionBuffer;
    };

    void buildUi();
    QWidget *buildChannelPanel(int index);
    void refreshAdbPorts();
    void applyPorts(int index);
    void connectChannel(int index);
    void disconnectChannel(int index, bool showClosedStatus = true);
    void disconnectAll();
    void attachTransportCallbacks(int index);
    void setChannelControlsEnabled(int index, bool enabled);
    void updateGlobalControls();
    void setChannelStatus(int index, const QString &message, bool error = false);
    void updateRouteSummary();
    bool channelUsesAdb(int index) const;
    bool selectedAdbIsOnline() const;
    bool validateChannel(int index, QString &error) const;
    serialdemo::SerialConfig channelConfig(int index) const;
    serialdemo::ByteBuffer manualPayload(int index, bool &ok);
    bool sendBytes(int index, const serialdemo::ByteBuffer &data);
    void sendManual(int index);
    void appendTransmitted(int index, const serialdemo::ByteBuffer &data);
    void appendReceived(int index, const serialdemo::ByteBuffer &data);
    void updateCounter(int index);
    void runBidirectionalTest();
    void updateBidirectionalResult(bool timedOut = false);

    QPushButton *connectBothButton_ = nullptr;
    QPushButton *disconnectBothButton_ = nullptr;
    QPushButton *bidirectionalTestButton_ = nullptr;
    QLabel *testResultLabel_ = nullptr;
    QLabel *routeLabel_ = nullptr;

    std::array<ChannelUi, 2> channels_;
    serialdemo::AdbDevice currentAdbDevice_;
    std::vector<serialdemo::PortInfo> localPorts_;
    std::vector<std::string> adbPorts_;
    std::vector<std::thread> scanThreads_;
    bool scanning_ = false;
    bool aToBPassed_ = false;
    bool bToAPassed_ = false;
    bool bidirectionalTestRunning_ = false;
    bool shuttingDown_ = false;
    std::uint64_t testGeneration_ = 0;
};
