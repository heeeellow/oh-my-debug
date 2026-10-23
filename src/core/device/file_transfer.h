#pragma once

#include "core/serial/serial_types.h"
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace testtools {

enum class FileKind { Regular, Directory, Symlink, Other };
struct FileEntry {
    std::string name;
    FileKind kind = FileKind::Other;
    std::uintmax_t size = 0;
    std::string modified;
    FileKind linkTargetKind = FileKind::Other;
    std::string linkTarget;
    std::string metadataError;
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
};
enum class TransferDirection { Download, Upload };
enum class TransferOutcome { Completed, Skipped, Failed, Cancelled };
struct FileTransferResult {
    TransferOutcome outcome = TransferOutcome::Failed;
    std::string message;
};
using FileProgress = std::function<void(const std::string&)>;

class IFileTransferBackend {
  public:
    virtual ~IFileTransferBackend() = default;
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
