#include "adapters/adb/process_runner.h"
#include "application/device/file_transfer_service.h"
#include <fstream>
#include <iostream>
#include <windows.h>
using namespace testtools;
namespace fs = std::filesystem;
void check(bool b, const std::string& text)
{
    if (!b)
        throw std::runtime_error(text);
}
void put(const fs::path& p, const std::string& bytes)
{
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write(bytes.data(), bytes.size());
    check(f.good(), "local fixture write");
}
std::string get(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
std::string quote(const std::string& p)
{
    std::string out = "'";
    for (char c : p)
        out += c == '\'' ? "'\\''" : std::string(1, c);
    return out + "'";
}
int wmain(int argc, wchar_t** argv)
{
    std::string remote;
    std::string adb;
    std::string serial;
    bool owns = false;
    const auto shell = [&](const std::string& script) {
        return serialdemo::ProcessRunner::run(
            adb, {"-s", serial, "shell", "-T", script}, std::chrono::seconds(10));
    };
    const auto cleanup = [&] {
        if (owns && remote.rfind("/data/local/tmp/testtools-file-qa-", 0) == 0 &&
            remote.find_first_not_of("0123456789",
                                     std::string("/data/local/tmp/testtools-file-qa-").size()) ==
                std::string::npos) {
            auto result = shell("chmod 700 " + quote(remote + "/denied") +
                                " 2>/dev/null; rm -rf -- " + quote(remote));
            check(result.exitCode == 0, "owned remote cleanup failed");
            owns = false;
        }
    };
    try {
        check(argc == 3, "bundled ADB path and explicit device serial required");
        serial = fs::path(argv[2]).u8string();
        adb = fs::path(argv[1]).u8string();
        _wputenv_s(L"ADB_PATH", argv[1]);
        const auto stamp = std::to_string(GetTickCount64());
        remote = "/data/local/tmp/testtools-file-qa-" + stamp;
        auto made = shell("mkdir " + quote(remote));
        check(made.exitCode == 0, "create owned device fixture: " + made.standardError);
        owns = true;
        auto resolved = shell("cd " + quote(remote) + " && pwd -P");
        while (!resolved.standardOutput.empty() &&
               (resolved.standardOutput.back() == '\r' || resolved.standardOutput.back() == '\n'))
            resolved.standardOutput.pop_back();
        check(resolved.exitCode == 0 && resolved.standardOutput == remote,
              "verify physical fixture path: " + resolved.standardOutput + resolved.standardError);
        const auto root = fs::absolute("build/iteration-0.8.0/device-smoke") / stamp;
        const auto send = root / "send", receive = root / "receive";
        fs::create_directories(send / "folder" / "empty");
        fs::create_directories(send / "empty");
        fs::create_directories(receive);
        const std::string name = u8"配置 & '文件.bin";
        const std::string bytes("\x00\xff\x01\r\nA\x7f", 7);
        put(send / fs::u8path(name), bytes);
        put(send / "folder" / "nested.log", bytes);
        auto service = createFileTransferService();
        FileTransferRequest request;
        request.device = {serial, "device", ""};
        request.remoteDirectory = remote;
        request.localDirectory = send;
        std::vector<FileEntry> entries = {{name, FileKind::Regular},
                                          {"folder", FileKind::Directory},
                                          {"empty", FileKind::Directory}};
        std::string error;
        const auto execute = [&](std::vector<FileEntry> selected,
                                 TransferDirection direction,
                                 bool overwrite = false) {
            request.entries = std::move(selected);
            request.direction = direction;
            request.overwrite = overwrite;
            check(service->prepare(request, error), error);
            service->executePrepared();
            return service->snapshot();
        };
        const auto complete = [&](const FileTransferSnapshot& state) {
            for (const auto& r : state.records)
                check(r.result.outcome == TransferOutcome::Completed,
                      "transfer " + r.name + ": " + r.result.message);
        };
        complete(execute(entries, TransferDirection::Upload));
        std::atomic<bool> cancel{false};
        auto listed = service->listRemote(request.device, remote, cancel);
        check(listed.error.empty() && listed.entries.size() == 3,
              "real NUL listing: " + listed.error);
        auto rootListing = service->listRemote(request.device, "/", cancel);
        check(rootListing.error.empty(), "list root");
        bool sdcard = false;
        for (const auto& entry : rootListing.entries)
            if (entry.name == "sdcard")
                sdcard = entry.kind == FileKind::Symlink && entry.browsable();
        check(sdcard, "sdcard directory symlink metadata");
        auto sd = service->listRemote(request.device, "/sdcard", cancel);
        check(sd.error.empty(), "browse real sdcard: " + sd.error);
        auto links = shell("ln -s folder " + quote(remote + "/folder-link") + " && ln -s missing " +
                           quote(remote + "/broken-link"));
        check(links.exitCode == 0, "create owned symlinks");
        auto linkListing = service->listRemote(request.device, remote, cancel);
        check(linkListing.error.empty(), "list owned symlinks");
        bool folderLink = false, broken = false;
        for (const auto& e : linkListing.entries) {
            if (e.name == "folder-link")
                folderLink = e.browsable();
            if (e.name == "broken-link")
                broken = e.kind == FileKind::Symlink && !e.browsable();
        }
        check(folderLink && broken, "directory and broken links distinguished");
        check(service->listRemote(request.device, remote + "/folder-link", cancel).error.empty(),
              "browse directory link");
        auto forbidden = execute({{"folder-link", FileKind::Symlink}}, TransferDirection::Download);
        check(forbidden.records[0].result.outcome != TransferOutcome::Completed,
              "link transfer not followed");
        request.localDirectory = receive;
        complete(execute(entries, TransferDirection::Download));
        check(get(receive / fs::u8path(name)) == bytes &&
                  get(receive / "folder" / "nested.log") == bytes &&
                  fs::is_directory(receive / "folder" / "empty") &&
                  fs::is_directory(receive / "empty"),
              "real binary/tree/empty round trip");
        auto state = execute({entries[0]}, TransferDirection::Download);
        check(state.records[0].result.outcome == TransferOutcome::Skipped,
              "download conflict default skip");
        put(receive / fs::u8path(name), "old local");
        complete(execute({entries[0]}, TransferDirection::Download, true));
        check(get(receive / fs::u8path(name)) == bytes, "local overwrite bytes");
        const auto file = remote + "/" + name;
        auto mode = shell("chmod 640 " + quote(file) + " && stat -c '%u:%g:%a' -- " + quote(file) +
                          " && ls -Zd -- " + quote(file));
        check(mode.exitCode == 0, "set fixture metadata");
        request.localDirectory = send;
        put(send / fs::u8path(name), "replacement bytes");
        complete(execute({entries[0]}, TransferDirection::Upload, true));
        auto after = shell("stat -c '%u:%g:%a' -- " + quote(file) + " && ls -Zd -- " + quote(file));
        check(after.exitCode == 0 && after.standardOutput == mode.standardOutput,
              "real owner/mode/context preserved");
        request.localDirectory = receive;
        complete(execute({entries[0]}, TransferDirection::Download, true));
        check(get(receive / fs::u8path(name)) == "replacement bytes", "remote overwrite bytes");
        request.localDirectory = send;
        state = execute({entries[1]}, TransferDirection::Upload, true);
        check(state.records[0].result.outcome == TransferOutcome::Skipped,
              "existing remote directory not merged");
        auto denied = shell("mkdir " + quote(remote + "/denied") + " && chmod 000 " +
                            quote(remote + "/denied"));
        check(denied.exitCode == 0, "set fixture directory permissions");
        listed = service->listRemote(request.device, remote + "/denied", cancel);
        check(!listed.error.empty(), "real directory access failure");
        request.remoteDirectory = remote + "/denied";
        state = execute({entries[0]}, TransferDirection::Upload);
        check(state.records[0].result.outcome == TransferOutcome::Failed,
              "real upload permission failure");
        cleanup();
        auto missing = shell("[ ! -e " + quote(remote) + " ]");
        check(missing.exitCode == 0, "fixture removed");
        std::cout << "Real RK3568 Android 11 shell UID2000 checks passed: Unicode/quote paths, "
                     "binary files, recursive/empty folders, two-way round trip, default skip, "
                     "explicit file overwrite with owner/mode/SELinux context, directory conflict, "
                     "read/write permission errors, owned fixture cleanup.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        try {
            cleanup();
        } catch (const std::exception& c) {
            std::cerr << c.what() << '\n';
        }
        return 1;
    }
}
