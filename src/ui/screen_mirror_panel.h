#pragma once

#include "application/device/screen_mirror_service.h"
#include "application/support/background_task_group.h"
#include <QtWidgets/QDialog>
#include <functional>
#include <vector>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTimer;

// Android 投屏控制面板（仅负责展示状态、发起异步控制动作与接收轮询快照）。
class ScreenMirrorPanel final : public QDialog {
    Q_OBJECT
public:
    explicit ScreenMirrorPanel(QWidget* parent = nullptr,
        std::shared_ptr<testtools::ScreenMirrorService> service = {}, bool embedded = false);
    ~ScreenMirrorPanel() override;
    void setDevice(const serialdemo::AdbDevice& device);
    void startCurrentDevice();
    void shutdown();
    void reject() override;
signals:
    void activityChanged(const QString& message);
    void shutdownFinished();
private:
    void updateControls();
    void poll();
    void saveScreenshot();
    void submit(const QString& activity, std::function<testtools::MirrorOperationResult()> operation);
    std::shared_ptr<testtools::ScreenMirrorService> service_;
    testtools::BackgroundTaskGroup tasks_;
    serialdemo::AdbDevice selected_;
    testtools::ScreenMirrorSnapshot state_;
    QLabel* target_ = nullptr;
    QLabel* status_ = nullptr;
    QPlainTextEdit* detail_ = nullptr;
    QPushButton* start_ = nullptr;
    QPushButton* stop_ = nullptr;
    std::vector<QPushButton*> controlButtons_;
    QTimer* timer_ = nullptr;
    std::uint64_t generation_ = 0;
    bool busy_ = false;
    bool polling_ = false;
    bool shuttingDown_ = false;
};
