#pragma once

#include "application/device/file_transfer_service.h"
#include "application/support/background_task_group.h"
#include <QtWidgets/QWidget>
#include <unordered_map>

class FilePane;
class QMimeData;
class QToolButton;
class QLineEdit;
class QTreeWidget;
class QPushButton;
class QLabel;
class QTimer;

// 本地与 ADB 设备双栏文件传输工作区（目录列举与批量传输均在后台线程执行）。
class FileTransferWorkspace final : public QWidget {
    Q_OBJECT
  public:
    explicit FileTransferWorkspace(QWidget* parent = nullptr,
                                   std::shared_ptr<testtools::FileTransferService> service = {});
    ~FileTransferWorkspace() override;
    void setDevice(const serialdemo::AdbDevice& device);
    void shutdown();

  protected:
    void showEvent(QShowEvent* event) override;
  signals:
    void activityChanged(const QString& message);
    void shutdownFinished();

  private:
    QWidget* buildPane(bool remote);
    void loadLocal(const QString& directory = {});
    void loadRemote(const std::string& directory = {});
    void refreshOnEntry();
    void transfer(testtools::TransferDirection direction);
    void updateControls();
    void poll();
    void startRequest(testtools::FileTransferRequest request);
    QMimeData* dragData(bool remote);
    bool dropData(bool remote, const QMimeData* mime);
    void retryFailed();
    std::shared_ptr<testtools::FileTransferService> service_;
    testtools::BackgroundTaskGroup tasks_;
    serialdemo::AdbDevice device_;
    testtools::FileListing local_, remote_;
    std::shared_ptr<std::atomic<bool>> localCancel_, remoteCancel_;
    std::uint64_t localGeneration_ = 0, remoteGeneration_ = 0;
    bool localLoading_ = false, remoteLoading_ = false, closing_ = false, wasActive_ = false;
    bool entryRefreshPending_ = false;
    std::unordered_map<std::string, std::string> remoteDirectories_;
    std::size_t shownRecords_ = 0;
    std::size_t batchRowOffset_ = 0;
    FilePane *localPane_ = nullptr, *remotePane_ = nullptr;
    testtools::FileTransferRequest lastRequest_;
    QString dragToken_;
    QStringList pendingDropNames_;
    QString pendingDropLocal_;
    std::string pendingDropSerial_, pendingDropRemote_;
    QToolButton* taskToggle_ = nullptr;
    QPushButton* retry_ = nullptr;
    QLineEdit *localPath_ = nullptr, *remotePath_ = nullptr;
    QTreeWidget* records_ = nullptr;
    QPushButton *upload_ = nullptr, *download_ = nullptr, *cancel_ = nullptr;
    QLabel *status_ = nullptr, *uploadTarget_ = nullptr, *downloadTarget_ = nullptr;
    QTimer* timer_ = nullptr;
};
