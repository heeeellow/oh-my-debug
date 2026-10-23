#include "adapters/adb/process_runner.h"
#include "application/device/file_transfer_service.h"
#include <fstream>
#include <iostream>
#include <thread>
#include <windows.h>

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
        fs::path root = fs::absolute(fs::u8path("build/iteration-0.8.0/native data & 中文'空间")) /
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
        auto managed = serialdemo::ProcessRunner::runManaged(
            fs::path(argv[1]).u8string(), {"sleep"}, std::chrono::milliseconds(80), cancel);
        check(managed.command.timedOut, "owned client timeout");
        const auto pidFile = root / "leaf.pid";
        managed = serialdemo::ProcessRunner::runManaged(fs::path(argv[1]).u8string(),
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
        managed = serialdemo::ProcessRunner::runManaged(
            fs::path(argv[1]).u8string(), {"flood"}, std::chrono::seconds(5), cancel);
        check(managed.outputTruncated, "bounded command output");
        std::cout << "Native file backend checks passed: NUL names, Unicode/quotes/& paths, binary "
                     "files, recursive/empty folders, overwrite metadata, conflicts, "
                     "failed/cancelled staging, timeout, output bound.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
