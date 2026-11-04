#include "adapters/adb/process_runner.h"
#include "application/device/file_transfer_service.h"
#include "test_paths.h"
#include <fstream>
#include <iostream>
#include <thread>
#include <windows.h>
#include <algorithm>

using namespace testtools;
namespace fs = std::filesystem;
void check(bool value, const std::string& message)
{
    if (!value)
        throw std::runtime_error(message);
}
void put(const fs::path& p, const std::string& bytes)
{
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write(bytes.data(), bytes.size());
}
std::string get(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
std::string posix(const fs::path& p)
{
    auto value = fs::absolute(p).generic_u8string();
    return "/" + std::string(1, char(std::tolower(value[0]))) + value.substr(2);
}
int wmain(int argc, wchar_t** argv)
{
    try {
        check(argc == 2, "mock exe path");
        _wputenv_s(L"ADB_PATH", argv[1]);
        fs::path root = testOutputDirectory() / fs::u8path("native data & 中文'空间") /
                        std::to_string(GetTickCount64());
        const auto local = root / "local", remote = root / "remote";
        fs::create_directories(local);
        fs::create_directories(remote / "empty");
        const std::string payload("\0\xff\x01\nA\r\n", 7);
        put(remote / fs::u8path("日志 & '测试.bin"), payload);
        put(remote / "slow.bin", payload);
        put(remote / "denied.bin", payload);
        fs::create_directories(remote / "folder" / "empty");
        put(remote / "folder" / "nested.log", payload);
        fs::create_directories(remote / "invalid-name");
        auto service = createFileTransferService();
        std::atomic<bool> cancel{false};
        auto listed = service->listRemote({"device-A", "device", ""}, posix(remote), cancel);
        check(listed.error.empty(), "remote NUL listing: " + listed.error);
        check(listed.entries.size() == 6, "remote entries");
        _wputenv_s(L"TESTTOOLS_FILE_LEGACY_LISTING", L"1");
        const auto legacy = service->listRemote({"device-A", "device", ""}, posix(remote), cancel);
        _wputenv_s(L"TESTTOOLS_FILE_LEGACY_LISTING", L"");
        check(legacy.error.empty() && legacy.entries.size() == listed.entries.size(), "older find falls back to the legacy listing");
        for (const auto& item : listed.entries) {
            const auto found = std::find_if(legacy.entries.begin(), legacy.entries.end(), [&](const auto& old) { return old.name == item.name; });
            check(found != legacy.entries.end() && found->kind == item.kind && found->size == item.size &&
                found->modified == item.modified && found->permissions == item.permissions,
                "batch and fallback metadata agree: " + item.name);
        }
        const auto batched = service->listRemote({"device-A", "device", ""}, posix(remote / "batched-format"), cancel);
        check(batched.error.empty() && batched.entries.size() == 2 &&
            batched.entries[0].name == "report\npart ' &.log" && batched.entries[0].modified == "1700000000" &&
            batched.entries[0].permissions == "0640" && batched.entries[1].browsable() && batched.entries[1].linkTarget == "target",
            "batched NUL names, fractional times, permissions and Toybox link prefixes");
        FileTransferRequest request;
        request.device = {"device-A", "device", ""};
        request.localDirectory = local;
        request.remoteDirectory = posix(remote);
        std::string error;
        const auto execute =
            [&](std::vector<FileEntry> items, TransferDirection dir, bool overwrite = false) {
                request.entries = std::move(items);
                request.direction = dir;
                request.overwrite = overwrite;
                check(service->prepare(request, error), error);
                service->executePrepared();
                return service->snapshot();
            };
        auto state = execute({{u8"日志 & '测试.bin", FileKind::Regular},
                              {"folder", FileKind::Directory},
                              {"empty", FileKind::Directory}},
                             TransferDirection::Download);
        for (const auto& r : state.records)
            check(r.result.outcome == TransferOutcome::Completed,
                  "download " + r.name + ": " + r.result.message);
        check(get(local / fs::u8path("日志 & '测试.bin")) == payload, "download binary bytes");
        check(get(local / "folder" / "nested.log") == payload &&
                  fs::is_directory(local / "folder" / "empty"),
              "folder hierarchy");
        state = execute({{u8"日志 & '测试.bin", FileKind::Regular}}, TransferDirection::Download);
        check(state.records[0].result.outcome == TransferOutcome::Skipped, "default no overwrite");
        put(local / fs::u8path("日志 & '测试.bin"), "old");
        state =
            execute({{u8"日志 & '测试.bin", FileKind::Regular}}, TransferDirection::Download, true);
        check(state.records[0].result.outcome == TransferOutcome::Completed &&
                  get(local / fs::u8path("日志 & '测试.bin")) == payload,
              "local atomic overwrite");
        put(local / fs::u8path("上传 & '配置.bin"), payload);
        state = execute({{u8"上传 & '配置.bin", FileKind::Regular}}, TransferDirection::Upload);
        check(state.records[0].result.outcome == TransferOutcome::Completed,
              "upload: " + state.records[0].result.message);
        check(get(remote / fs::u8path("上传 & '配置.bin")) == payload, "uploaded bytes");
        put(local / fs::u8path("上传 & '配置.bin"), "replacement");
        state =
            execute({{u8"上传 & '配置.bin", FileKind::Regular}}, TransferDirection::Upload, true);
        check(state.records[0].result.outcome == TransferOutcome::Completed,
              "overwrite attrs: " + state.records[0].result.message);
        check(get(remote / fs::u8path("上传 & '配置.bin")) == "replacement", "remote replaced");
        fs::create_directories(local / "upload-folder" / "empty");
        put(local / "upload-folder" / "a.txt", payload);
        state = execute({{"upload-folder", FileKind::Directory}}, TransferDirection::Upload);
        check(state.records[0].result.outcome == TransferOutcome::Completed,
              "upload folder: " + state.records[0].result.message);
        check(get(remote / "upload-folder" / "a.txt") == payload &&
                  fs::is_directory(remote / "upload-folder" / "empty"),
              "uploaded folder hierarchy");
        state = execute({{"upload-folder", FileKind::Directory}}, TransferDirection::Upload, true);
        check(state.records[0].result.outcome == TransferOutcome::Skipped,
              "do not merge directory");
        state = execute({{"denied.bin", FileKind::Regular}}, TransferDirection::Download);
        check(state.records[0].result.outcome == TransferOutcome::Failed &&
                  !fs::exists(local / "denied.bin"),
              "failure does not publish partial file");
        state = execute({{"invalid-name", FileKind::Directory}}, TransferDirection::Download);
        check(state.records[0].result.outcome == TransferOutcome::Failed,
              "reject incompatible Windows nested name");
        fs::create_directories(remote / "case-clash");
        state = execute({{"case-clash", FileKind::Directory}}, TransferDirection::Download);
        check(state.records[0].result.outcome == TransferOutcome::Failed,
              "reject case-colliding remote directory");
        put(remote / "type-changed", "actual file");
        state = execute({{"type-changed", FileKind::Directory}}, TransferDirection::Download);
        check(state.records[0].result.outcome == TransferOutcome::Failed &&
                  !fs::exists(local / "type-changed"),
              "changed remote type not published");
        fs::create_directories(remote / "dir-changed");
        state = execute({{"dir-changed", FileKind::Regular}}, TransferDirection::Download);
        check(state.records[0].result.outcome == TransferOutcome::Failed &&
                  !fs::exists(local / "dir-changed"),
              "changed file to directory not published");
        put(local / "denied.bin", "new local bytes");
        state = execute({{"denied.bin", FileKind::Regular}}, TransferDirection::Upload, true);
        check(state.records[0].result.outcome == TransferOutcome::Failed &&
                  get(remote / "denied.bin") == payload,
              "failed upload preserves remote original");
        request.entries = {{"slow.bin", FileKind::Regular}};
        request.direction = TransferDirection::Download;
        request.overwrite = false;
        check(service->prepare(request, error), error);
        std::thread worker([&] { service->executePrepared(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        service->cancel();
        worker.join();
        check(service->snapshot().records[0].result.outcome == TransferOutcome::Cancelled &&
                  !fs::exists(local / "slow.bin"),
              "cancel discards temporary download");
        for (const auto& e : fs::directory_iterator(local))
            check(e.path().filename().u8string().find(".testtools-download-") != 0,
                  "download temp cleaned");
        for (const auto& e : fs::directory_iterator(remote))
            check(e.path().filename().u8string().find(".testtools-upload-") != 0,
                  "upload temp cleaned");
        auto managed = testtools::ProcessRunner::runManaged(
            fs::path(argv[1]).u8string(), {"sleep"}, std::chrono::milliseconds(80), cancel);
        check(managed.command.timedOut, "owned client timeout");
        const auto pidFile = root / "leaf.pid";
        managed = testtools::ProcessRunner::runManaged(fs::path(argv[1]).u8string(),
                                                        {"spawn-leaf", pidFile.u8string()},
                                                        std::chrono::milliseconds(200),
                                                        cancel);
        DWORD leafId = 0;
        {
            std::ifstream file(pidFile);
            file >> leafId;
        }
        check(leafId != 0, "descendant PID available");
        const auto leaf =
            OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, leafId);
        DWORD exitCode = 0;
        bool alive = leaf && GetExitCodeProcess(leaf, &exitCode) && exitCode == STILL_ACTIVE;
        if (leaf) {
            TerminateProcess(leaf, 2);
            CloseHandle(leaf);
        }
        check(managed.command.timedOut && alive,
              "client timeout leaves independent server child alive");
        managed = testtools::ProcessRunner::runManaged(
            fs::path(argv[1]).u8string(), {"flood"}, std::chrono::seconds(5), cancel);
        check(managed.outputTruncated, "bounded command output");

        const auto mountInfo = root / "mounts.txt";
        const auto baseMounts = std::string("1 0 0:1 / / rw - tmpfs tmpfs rw\n");
        put(mountInfo, baseMounts);
        _wputenv_s(L"TESTTOOLS_FILE_MOUNTINFO", fs::u8path(posix(mountInfo)).c_str());
        const auto change = [&](bool remoteSide, FileOperation operation, const fs::path& parent,
                                const std::string& name, FileKind kind, const std::string& renamed = {}) {
            FileManagementRequest task; task.operation = operation; task.remote = remoteSide;
            task.device = {"device-A", "device", ""}; task.parent = remoteSide ? posix(parent) : fs::absolute(parent).u8string();
            task.entries = {{name, kind}}; task.newName = renamed;
            if (operation == FileOperation::SetPermissions) task.permissions = renamed.empty() ? "755" : renamed;
            check(service->prepareChange(task, error), error); service->executePrepared();
            return service->snapshot().records.front().result;
        };
        const auto newDirectory = u8"新目录 & '测试";
        auto changed = change(true, FileOperation::CreateFolder, remote, newDirectory, FileKind::Directory);
        check(changed.outcome == TransferOutcome::Completed && fs::is_directory(remote / fs::u8path(newDirectory)), "remote mkdir quoted: " + changed.message);
        changed = change(true, FileOperation::CreateFolder, remote, newDirectory, FileKind::Directory);
        check(changed.outcome == TransferOutcome::Failed, "mkdir never overwrites existing target");
        put(remote / "old.log", "old-data"); put(remote / "exists.log", "existing-data");
        changed = change(true, FileOperation::Rename, remote, "old.log", FileKind::Regular, "exists.log");
        check(changed.outcome == TransferOutcome::Failed && get(remote / "exists.log") == "existing-data" && fs::exists(remote / "old.log"), "remote rename collision preserves both");
        changed = change(true, FileOperation::Rename, remote, "old.log", FileKind::Regular, u8"已更名 ' &.log");
        check(changed.outcome == TransferOutcome::Completed && get(remote / fs::u8path(u8"已更名 ' &.log")) == "old-data", "remote rename exact target: " + changed.message);
        changed = change(true, FileOperation::SetPermissions, remote, u8"已更名 ' &.log", FileKind::Regular);
        const auto observedMode = testtools::ProcessRunner::run("C:/Program Files/Git/bin/bash.exe",
            {"-c", "stat -c '%a' -- \"$1\"", "_", posix(remote / fs::u8path(u8"已更名 ' &.log"))}, std::chrono::seconds(5));
        check(observedMode.exitCode == 0, "independent host permission read");
        const bool actually755 = std::stoul(observedMode.standardOutput, nullptr, 8) == 0755;
        check(changed.outcome == (actually755 ? TransferOutcome::Completed : TransferOutcome::Failed) &&
            (actually755 || changed.message.find("权限与请求不一致") != std::string::npos) &&
            get(remote / fs::u8path(u8"已更名 ' &.log")) == "old-data", "permission result must agree with independently observed host mode: " + changed.message);
        changed = change(true, FileOperation::SetPermissions, remote, u8"已更名 ' &.log", FileKind::Regular, "644");
        check(changed.outcome == TransferOutcome::Completed, "explicit mode passes only after matching readback: " + changed.message);
        FileManagementRequest staleMode;
        staleMode.operation = FileOperation::SetPermissions; staleMode.remote = true; staleMode.device = {"device-A", "device", ""};
        staleMode.parent = posix(remote); staleMode.permissions = "644";
        staleMode.entries = {{u8"已更名 ' &.log", FileKind::Regular}}; staleMode.entries[0].permissions = "0600";
        check(service->prepareChange(staleMode, error), error); service->executePrepared();
        check(service->snapshot().records[0].result.outcome == TransferOutcome::Failed &&
            service->snapshot().records[0].result.message.find("权限在确认后已变化") != std::string::npos, "stale observed permission refuses mutation");
        changed = change(true, FileOperation::SetPermissions, remote, newDirectory, FileKind::Directory, "755");
        check(changed.outcome == TransferOutcome::Completed, "directory permissions are non-recursive and read back: " + changed.message);
        fs::create_directories(remote / "protected" / "mounted");
        put(remote / "protected" / "keep.log", "keep");
        auto encoded = posix(remote / "protected" / "mounted");
        std::string escaped;
        for (char c : encoded) escaped += c == ' ' ? "\\040" : c == '\\' ? "\\134" : std::string(1, c);
        put(mountInfo, baseMounts + "2 1 0:2 / " + escaped + " rw - tmpfs tmpfs rw\n");
        changed = change(true, FileOperation::Remove, remote, "protected", FileKind::Directory);
        check(changed.outcome == TransferOutcome::Failed && get(remote / "protected" / "keep.log") == "keep", "nested mount blocks entire delete");
        put(mountInfo, "malformed mount metadata\n");
        changed = change(true, FileOperation::Remove, remote, "protected", FileKind::Directory);
        check(changed.outcome == TransferOutcome::Failed && fs::exists(remote / "protected"), "unreadable mount evidence refuses mutation");
        put(mountInfo, baseMounts);
        changed = change(true, FileOperation::Remove, remote, "protected", FileKind::Directory);
        check(changed.outcome == TransferOutcome::Completed && !fs::exists(remote / "protected"), "remote directory remove: " + changed.message);
        auto guarded = FileManagementRequest{FileOperation::Remove, true, {"device-A", "device", ""}, "/", {{"system", FileKind::Directory}}, {}};
        check(service->prepareChange(guarded, error), error); service->executePrepared();
        check(service->snapshot().records[0].result.outcome == TransferOutcome::Failed, "protected remote root refuses deletion");
        guarded.operation = FileOperation::CreateFolder; guarded.parent = "/proc"; guarded.entries = {{"testtools-never-created", FileKind::Directory}};
        check(service->prepareChange(guarded, error), error); service->executePrepared();
        check(service->snapshot().records[0].result.outcome == TransferOutcome::Failed, "virtual directories remain read only");

        changed = change(false, FileOperation::CreateFolder, local, newDirectory, FileKind::Directory);
        check(changed.outcome == TransferOutcome::Completed && fs::is_directory(local / fs::u8path(newDirectory)), "local mkdir unicode");
        put(local / "rename-source", "source"); put(local / "rename-target", "target");
        changed = change(false, FileOperation::Rename, local, "rename-source", FileKind::Regular, "rename-target");
        check(changed.outcome == TransferOutcome::Failed && get(local / "rename-target") == "target", "local atomic rename no replace");
        changed = change(false, FileOperation::Rename, local, "rename-source", FileKind::Regular, "renamed");
        check(changed.outcome == TransferOutcome::Completed && get(local / "renamed") == "source", "local same-parent rename");
        const auto outside = root / "outside", tree = local / "delete-tree";
        fs::create_directories(outside); fs::create_directories(tree);
        put(outside / "protected.log", "do-not-follow");
        const auto junction = tree / "junction";
        const auto linked = testtools::ProcessRunner::run("cmd.exe", {"/c", "mklink", "/J", junction.u8string(), fs::absolute(outside).u8string()}, std::chrono::seconds(5));
        check(linked.exitCode == 0 && (GetFileAttributesW(junction.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT), "isolated junction fixture");
        changed = change(false, FileOperation::Remove, local, "delete-tree", FileKind::Directory);
        check(changed.outcome == TransferOutcome::Completed && !fs::exists(tree) && get(outside / "protected.log") == "do-not-follow", "recursive remove unlinks junction without traversing target: " + changed.message);
        changed = change(true, FileOperation::Remove, remote, newDirectory, FileKind::Directory);
        check(changed.outcome == TransferOutcome::Completed, "remote selected directory cleanup");
        std::cout << "File management passed: isolated local/remote mkdir/rename/delete/permissions/readback, collisions, canonical parent, mount table and junction protection.\n";
        std::cout << "Native file backend checks passed: NUL names, Unicode/quotes/& paths, binary "
                     "files, recursive/empty folders, overwrite metadata, conflicts, "
                     "failed/cancelled staging, timeout, output bound.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
