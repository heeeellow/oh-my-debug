#pragma once
#include "application/terminal/terminal_engine.h"
#include "serial_endpoint_workspace.h"
#include <atomic>
#include <QtCore/QElapsedTimer>

class TerminalGridWidget;
class SerialTerminalWorkspace final : public SerialEndpointWorkspace {
    Q_OBJECT
  public:
    explicit SerialTerminalWorkspace(QWidget* parent = nullptr,
                                     std::shared_ptr<testtools::SerialPortClaims> claims = {});
    ~SerialTerminalWorkspace() override;
    bool selectWorkspaceMode(const QString& modeName) override;
    QString currentWorkspaceMode() const override
    {
        return QStringLiteral("terminal");
    }

  private:
    bool isTerminal() const override
    {
        return true;
    }
    QWidget* createReceiveView(int index, QWidget* parent) override;
    void buildReceiveOptions(QHBoxLayout* header) override;
    void setupReceiveInput(int index) override;
    void streamReset(int index) override;
    void clearReceive(int index) override;
    bool historyTruncated(int index) const override;
    void renderReceiveLog(int index) override;
    void applyLogAppendResult(int index, const testtools::SerialLogAppendResult& result) override;
    void appendReceived(int index, const testtools::ByteBuffer& data) override;
    void focusTerminal() override;
    QString receiveText(int index) const override;
    void followOutput(int index) override;
    void setHighlightEnabled(int index, bool enabled) override;
    void updateSearchMatches(int index) override;
    void findLogMatch(int index, int step) override;
    void handleTerminalKey(QKeyEvent* event);
    void pasteClipboard();
    void renderTerminal();
    void noteTerminalInput();
    void applyAppearance();
    void editAppearance();
    void resumeTerminalInput();
    QPushButton* terminalOptionsButton_ = nullptr;
    QLabel* dimensionsLabel_ = nullptr;
    TerminalGridWidget* terminalGrid_ = nullptr;
    std::array<QAction*, 3> terminalEndingActions_{};
    std::array<QAction*, 2> terminalBackspaceActions_{};
    QElapsedTimer inputEchoClock_;
    std::uint64_t inputEchoParsedBytes_ = 0;
    bool awaitingInputEcho_ = false;
    std::shared_ptr<testtools::TerminalEngine> terminalEngine_;
    std::shared_ptr<std::atomic<bool>> frameNotice_ = std::make_shared<std::atomic<bool>>(false);
    std::uint64_t pasteApprovedGeneration_ = UINT64_MAX;
    std::uint64_t announcedDroppedBytes_ = 0;
    std::uint64_t announcedRejectedInputs_ = 0;
};
