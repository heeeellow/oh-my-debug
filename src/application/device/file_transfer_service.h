#pragma once

#include "core/device/file_transfer.h"
#include <mutex>

namespace testtools {

struct FileTransferRequest {
    serialdemo::AdbDevice device;
    TransferDirection direction = TransferDirection::Download;
    std::filesystem::path localDirectory;
    std::string remoteDirectory;
    std::vector<FileEntry> entries;
    bool overwrite = false;
};
struct FileTransferRecord {
    std::string name;
    std::string local;
    std::string remote;
    FileTransferResult result;
};
struct FileTransferSnapshot {
    std::uint64_t id = 0;
    bool active = false;
    bool cancelling = false;
    std::string serial;
    TransferDirection direction = TransferDirection::Download;
    std::size_t total = 0;
    std::string current;
    std::string progress;
    std::string summary;
    std::vector<FileTransferRecord> records;
};

// 同步传输与目录列表操作均在界面层持有的后台线程中运行；
// 界面线程仅调用 prepare / cancel / snapshot，三者均不执行任何阻塞 I/O。
class FileTransferService {
  public:
    explicit FileTransferService(std::shared_ptr<IFileTransferBackend> backend);
    FileListing listRemote(const serialdemo::AdbDevice& device,
                           const std::string& directory,
                           const std::atomic<bool>& cancel) const;
    static FileListing listLocal(const std::filesystem::path& directory,
                                 const std::atomic<bool>& cancel);
    static bool validName(const std::string& name);
    static std::string normalizeRemoteDirectory(const std::string& directory);
    bool prepare(FileTransferRequest request, std::string& error);
    void executePrepared();
    void cancel(std::uint64_t expectedId = 0) noexcept;
    FileTransferSnapshot snapshot() const;

  private:
    std::shared_ptr<IFileTransferBackend> backend_;
    std::atomic<bool> cancel_{false};
    mutable std::mutex mutex_;
    FileTransferRequest request_;
    FileTransferSnapshot state_;
};

std::shared_ptr<FileTransferService> createFileTransferService();

} // namespace testtools
