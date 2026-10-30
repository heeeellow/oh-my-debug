#pragma once

#include "core/serial/serial_types.h"
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace testtools {

// 远端/本地文件目录项模型与文件传输后端抽象契约。
enum class FileKind { Regular, Directory, Symlink, Other };
struct FileEntry {
    std::string name;
    FileKind kind = FileKind::Other;
    std::uintmax_t size = 0;
    std::string modified;
    FileKind linkTargetKind = FileKind::Other;
    std::string linkTarget;
    std::string metadataError;
    std::string permissions; // 设备 stat 读到的四位八进制权限；未知时为空。
    bool browsable() const noexcept
    {
        return kind == FileKind::Directory ||
               (kind == FileKind::Symlink && linkTargetKind == FileKind::Directory);
    }
};
struct FileListing {
    std::string path;
    std::vector<FileEntry> entries;
    std::string error;
    std::string platform;
};
enum class TransferDirection { Download, Upload };
enum class TransferOutcome { Completed, Skipped, Failed, Cancelled };
struct FileTransferResult {
    TransferOutcome outcome = TransferOutcome::Failed;
    std::string message;
};
using FileProgress = std::function<void(const std::string&)>;

enum class FileOperation { Transfer, CreateFolder, Rename, Remove, SetPermissions };
struct FileChange {
    FileOperation operation = FileOperation::CreateFolder;
    bool remote = false;
    std::string serial, parent;
    FileEntry entry;
    std::string newName;
    std::string permissions;
};

class IFileTransferBackend {
  public:
    virtual ~IFileTransferBackend() = default;
    virtual FileTransferResult change(const FileChange& request, const std::atomic<bool>& cancel) = 0;
    virtual FileListing list(const std::string& serial,
                             const std::string& directory,
                             const std::atomic<bool>& cancel) = 0;
    virtual FileTransferResult transfer(TransferDirection direction,
                                        const std::string& serial,
                                        const std::filesystem::path& local,
                                        const std::string& remote,
                                        FileKind kind,
                                        bool overwrite,
                                        const std::atomic<bool>& cancel,
                                        const FileProgress& progress) = 0;
};

} // namespace testtools
