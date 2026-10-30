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
    testtools::FileTransferResult change(const testtools::FileChange& request, const std::atomic<bool>&) override
    {
        serials.push_back(request.serial); paths.push_back(request.parent + '/' + request.entry.name);
        permissions.push_back(request.permissions);
        if (cancelAfterOne) service->cancel();
        return {request.entry.name == "bad.log" ? TransferOutcome::Failed : TransferOutcome::Completed, "change fixture"};
    }

    std::vector<std::string> serials, paths, permissions;
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
        check(!FileTransferService::validChangeName("../escape", true) &&
              !FileTransferService::validChangeName("back\\slash", true) &&
              !FileTransferService::validChangeName("line\nname", true) &&
              !FileTransferService::validChangeName("NUL.txt", false) &&
              !FileTransferService::validChangeName("end.", false) &&
              FileTransferService::validChangeName("中文 & 'file", false) &&
              FileTransferService::validChangeName("key:value", true), "change name boundaries");
        check(FileTransferService::validChangeName("COM10.log", false) && FileTransferService::validChangeName(u8"COM测试.log", false) &&
              !FileTransferService::validChangeName("COM1.log", false) && !FileTransferService::validChangeName(u8"LPT¹.txt", false),
              "device reserved names do not reject ordinary COM-prefixed logs");
#ifdef _WIN32
        std::string chineseName;
        for (int i = 0; i < 100; ++i) chineseName += u8"中";
        check(FileTransferService::validChangeName(chineseName, false) && !FileTransferService::validChangeName(chineseName, true),
              "Windows UTF16 and remote byte name limits differ");
#endif
        backend->cancelAfterOne = false; backend->serials.clear(); backend->paths.clear();
        FileManagementRequest change;
        change.operation = FileOperation::Remove; change.remote = true; change.device = {"fixed-device", "device", ""};
        change.parent = "/tmp/work"; change.entries = {{"a.log", FileKind::Regular}, {"bad.log", FileKind::Regular}};
        check(service.prepareChange(change, error), "prepare file changes");
        check(!service.prepare(request(), error) && !service.prepareChange(change, error), "shared transfer/change gate");
        change.device.serial = "other-device"; change.parent = "/other";
        service.executePrepared(); state = service.snapshot();
        check(!state.active && state.operation == FileOperation::Remove && state.serial == "fixed-device" &&
              state.records.size() == 2 && state.records[0].result.outcome == TransferOutcome::Completed &&
              state.records[1].result.outcome == TransferOutcome::Failed && backend->serials[1] == "fixed-device" &&
              backend->paths[1] == "/tmp/work/bad.log", "fixed change target and per-item outcomes");
        check(service.prepareChange(change, error), "prepare cancellation");
        service.cancel(); service.executePrepared();
        check(service.snapshot().records[0].result.outcome == TransferOutcome::Cancelled, "cancel unstarted mutations");
        check(FileTransferService::normalizePermissions("644") == "0644" &&
            FileTransferService::normalizePermissions("2750") == "2750" &&
            FileTransferService::normalizePermissions("0000") == "0000", "octal permission values preserve special bits and zero");
        for (const auto& bad : {"", "64", "888", "10000", "755;id", "u+rwx", " 644", "0644\n"})
            check(FileTransferService::normalizePermissions(bad).empty(), "reject non-octal permission input");
        change.operation = FileOperation::SetPermissions; change.permissions = "640";
        change.entries = {{"folder", FileKind::Directory}, {"ok.log", FileKind::Regular}};
        check(service.prepareChange(change, error), "prepare explicit non-recursive directory/file permission batch");
        service.executePrepared();
        check(backend->permissions.back() == "0640", "forward canonical permissions rather than fixed 755");
        change.permissions = "777; id";
        check(!service.prepareChange(change, error), "permission commands cannot be injected");
        change.permissions = "644"; change.entries = {{"link", FileKind::Symlink}};
        check(!service.prepareChange(change, error), "permission changes never follow symlinks");
        change.entries = {{"ok.log", FileKind::Regular}};
        change.remote = false; change.parent = std::filesystem::temp_directory_path().u8string();
        change.device = {}; change.operation = FileOperation::SetPermissions;
        check(!service.prepareChange(change, error), "local chmod not available");
        change.operation = FileOperation::Rename; change.newName = "../outside";
        check(!service.prepareChange(change, error), "rename must be one valid entry");
        change.entries.resize(1); change.newName = "renamed.log";
        check(service.prepareChange(change, error), "local changes independent of ADB");
        service.cancel(); service.executePrepared();
        std::cout << "file transfer contracts passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
