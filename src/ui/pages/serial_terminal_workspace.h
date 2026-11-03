#pragma once
#include "application/serial/serial_terminal.h"
#include "serial_endpoint_workspace.h"

// 独立单端点终端宿主；保留现有解析、键盘与偏好契约，后续引擎替换局限于此页。
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
    void buildReceiveOptions(QHBoxLayout* header) override;
    void setupReceiveInput(int index) override;
    bool handleReceiveEvent(QObject* watched, QEvent* event) override;
    void streamReset(int index) override;
    void clearReceive(int index) override;
    bool historyTruncated(int index) const override;
    void renderReceiveLog(int index) override;
    void applyLogAppendResult(int index, const testtools::SerialLogAppendResult& result) override;
    void appendReceived(int index, const serialdemo::ByteBuffer& data) override;
    void stopPageTasks() override;
    void focusTerminal() override;
    void updateTerminalCursor(bool follow);
    void resumeTerminalInput();
    bool handleTerminalKey(QKeyEvent* event);
    serialdemo::LineEnding terminalLineEnding() const;
    void scheduleTerminalRender();
    void renderTerminal(bool fullRedraw = false);
    QPushButton* terminalOptionsButton_ = nullptr;
    std::array<QAction*, 3> terminalEndingActions_{};
    std::array<QAction*, 2> terminalBackspaceActions_{};
    QTimer* terminalRenderTimer_ = nullptr;
    testtools::SerialTerminalBuffer terminalBuffer_;
};
