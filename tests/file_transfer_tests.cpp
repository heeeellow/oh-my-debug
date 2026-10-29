#include "application/device/file_transfer_service.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace testtools;
namespace {
void check(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}
class Backend final : public IFileTransferBackend {
  public:
    std::vector<std::string> serials, paths;
    FileTransferService* service = nullptr;
    bool cancelAfterOne = false;
    FileListing
    list(const std::string&, const std::string& directory, const std::atomic<bool>&) override
    {
        return {directory, {{"log.txt", FileKind::Regular, 20, "0"}}, {}};
    }
    FileTransferResult transfer(TransferDirection,
                                const std::string& serial,
                                const std::filesystem::path&,
                                const std::string& remote,
                                FileKind,
                                bool,
                                const std::atomic<bool>&,
                                const FileProgress& progress) override
    {
        serials.push_back(serial);
        paths.push_back(remote);
        progress("50%");
        if (cancelAfterOne)
            service->cancel();
        if (remote.find("bad") != std::string::npos)
            throw std::runtime_error("permission denied");
        return {TransferOutcome::Completed, "done"};
    }
};
FileTransferRequest request()
{
    FileTransferRequest value;
    value.device = {"device-A", "device", ""};
    value.localDirectory = std::filesystem::temp_directory_path();
    value.remoteDirectory = "/data/local/tmp";
    value.entries = {{"a.log", FileKind::Regular},
                     {"bad.log", FileKind::Regular},
                     {"folder", FileKind::Directory}};
    return value;
}
} // namespace
int main()
{
    try {
        auto backend = std::make_shared<Backend>();
        FileTransferService service(backend);
        backend->service = &service;
        check(FileTransferService::normalizeRemoteDirectory("/data//local/../log/") == "/data/log",
              "path normalization");
        check(FileTransferService::normalizeRemoteDirectory("relative").empty(),
              "reject relative remote path");
        check(FileTransferService::normalizeRemoteDirectory(std::string("/tmp\0tail", 9)).empty(),
              "reject embedded NUL");
        std::atomic<bool> cancel{false};
        check(!service.listRemote({"A", "offline", ""}, "/", cancel).error.empty(),
              "reject offline browsing");
        check(service.listRemote({"A", "device", ""}, "/tmp", cancel).entries.size() == 1,
              "independent browsing");
        FileEntry directoryLink{"sdcard", FileKind::Symlink};
        directoryLink.linkTargetKind = FileKind::Directory;
        check(directoryLink.browsable(), "directory symlink can be browsed");
        check(!FileEntry{"broken", FileKind::Symlink}.browsable(), "unknown link not treated as directory");
        check(!FileEntry{"fifo", FileKind::Other}.browsable(), "special node not browsable");
        std::string error;
        auto invalid = request();
        invalid.entries[0].name = "../escape";
        check(!service.prepare(invalid, error), "reject escaping selection");
        invalid.entries[0].name = "..\\escape";
        check(!service.prepare(invalid, error), "reject Windows path traversal");
        invalid.entries[0].name = "C:escape";
        check(!service.prepare(invalid, error), "reject Windows drive-relative selection");
        check(service.prepare(request(), error), "prepare batch");
        check(!service.prepare(request(), error), "reject simultaneous batch");
        auto changedSelection = request();
        changedSelection.device.serial = "device-B";
        service.executePrepared();
        auto state = service.snapshot();
        check(!state.active && state.records.size() == 3, "terminal batch records");
        check(state.records[0].result.outcome == TransferOutcome::Completed &&
                  state.records[1].result.outcome == TransferOutcome::Failed &&
                  state.records[2].result.outcome == TransferOutcome::Completed,
              "partial failure retained per item");
        check(backend->serials == std::vector<std::string>({"device-A", "device-A", "device-A"}),
              "fixed job device");
        check(backend->paths[0] == "/data/local/tmp/a.log", "fixed absolute target");
        backend->serials.clear();
        check(service.prepare(request(), error), "prepare retry");
        const auto retryId = service.snapshot().id;
        service.cancel(state.id);
        check(retryId != state.id && !service.snapshot().cancelling, "stale task action cannot cancel next batch");
        service.cancel();
        service.executePrepared();
        check(backend->serials.empty(), "cancel before worker sends nothing");
        check(service.snapshot().records[2].result.outcome == TransferOutcome::Cancelled,
              "cancel queued items");
        backend->cancelAfterOne = true;
        check(service.prepare(request(), error), "prepare running cancellation");
        service.executePrepared();
        state = service.snapshot();
        check(state.records[0].result.outcome == TransferOutcome::Completed &&
                  state.records[1].result.outcome == TransferOutcome::Cancelled,
              "cancel preserves completed item");
        auto local = FileTransferService::listLocal(std::filesystem::temp_directory_path(), cancel);
        check(local.error.empty() && !local.path.empty(), "local directory listing");
        std::cout << "file transfer contracts passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
