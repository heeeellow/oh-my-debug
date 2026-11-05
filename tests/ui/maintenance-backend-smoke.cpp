#include "adapters/adb/process_runner.h"
#include "adapters/device/adb_device_maintenance.h"
#include "application/device/adb_terminal_service.h"
#include "application/device/device_maintenance_service.h"
#include "test_paths.h"
#include <fstream>
#include <iostream>
#include <thread>
#include <windows.h>

using namespace testtools;
namespace {
void check(bool value, const char* message)
{
    if (!value)
        throw std::runtime_error(message);
}
std::string read(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
void write(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary);
    out << text;
}
} // namespace
int main(int argc, char** argv)
{
    try {
        if (argc == 5 && std::string(argv[1]) == "--inspect-loader") {
            const auto backend = createAdbDeviceMaintenanceBackend(argv[2]); std::atomic<bool> cancel{false};
            const auto support = backend->inspectLoaderSupport({argv[3], "device", {}}, cancel);
            write(std::filesystem::u8path(argv[4]), support.evidence + "\n" + support.reason);
            check(support.state == LoaderSupportState::Supported, support.reason.c_str());
            std::cout << "PASS read-only runtime Loader qualification; no reboot requested\n"; return 0;
        }
        check(argc == 2, "mock program argument");
        const auto program = std::filesystem::absolute(std::filesystem::u8path(argv[1])).u8string();
        SetEnvironmentVariableW(L"ADB_PATH", std::filesystem::u8path(program).c_str());
        const auto root = testOutputDirectory() / "maintenance backend";
        std::filesystem::create_directories(root);
        const auto fixture = root / "commands";
        std::filesystem::create_directories(fixture);
        SetEnvironmentVariableW(L"TESTTOOLS_MAINTENANCE_FIXTURE", fixture.c_str());
        auto scene = [&](const std::string& name) {
            write(fixture / "scene.txt", name);
            write(fixture / "calls.txt", "");
        };
        auto service =
            std::make_shared<DeviceMaintenanceService>(createAdbDeviceMaintenanceBackend(program));
        MaintenanceRequest request;
        request.device = {"device-A", "device", "fixture"};
        request.action = MaintenanceAction::Dmesg;
        request.output = root / L"日志 & ' binary.log";
        request.overwrite = true;
        request.commandTimeout = std::chrono::milliseconds(1500);
        request.recoveryTimeout = std::chrono::milliseconds(350);
        auto run = [&] {
            std::string error;
            check(service->prepare(request, {}, error), error.c_str());
            service->executePrepared();
            return service->snapshot().result;
        };
        scene("big-log");
        auto result = run();
        auto content = read(request.output);
        check(result.outcome == MaintenanceOutcome::Completed &&
                  result.bytes == 6 * 1024 * 1024 + 7 && content.size() == result.bytes,
              "more than 4 MiB streams to file without output truncation");
        check(content[6 * 1024 * 1024] == '\0' && content.back() == '!',
              "binary and UTF-8 bytes preserved");
        scene("success");
        request.overwrite = false;
        write(request.output, "original");
        result = run();
        check(result.outcome == MaintenanceOutcome::Failed && read(request.output) == "original" &&
                  read(fixture / "calls.txt").empty(),
              "unapproved overwrite performs no ADB operation");
        request.overwrite = true;
        scene("log-denied");
        result = run();
        check(result.outcome == MaintenanceOutcome::Partial && read(request.output) == "original" &&
                  read(result.artifact) == "prefix\n" &&
                  result.detail.find("Permission denied") != std::string::npos,
              "permission failure keeps partial data and original target");
        scene("slow-log");
        request.commandTimeout = std::chrono::milliseconds(150);
        result = run();
        check(result.outcome == MaintenanceOutcome::TimedOut && read(result.artifact) == "prefix\n",
              "timeout retains partial file");
        request.commandTimeout = std::chrono::milliseconds(1500);
        scene("slow-log");
        std::string error;
        check(service->prepare(request, {}, error), "cancel export prepared");
        std::thread exportWorker([&] { service->executePrepared(); });
        while (service->snapshot().active && service->snapshot().progress.bytes == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        service->cancel(service->snapshot().id);
        exportWorker.join();
        result = service->snapshot().result;
        check(result.outcome == MaintenanceOutcome::Cancelled &&
                  read(result.artifact) == "prefix\n",
              "cancel retains partial file");
        scene("success");
        auto invalid = request;
        invalid.output = root / "missing" / "cannot.log";
        check(service->prepare(invalid, {}, error), "invalid host target prepared without GUI I/O");
        service->executePrepared();
        check(service->snapshot().result.outcome == MaintenanceOutcome::Failed &&
                  read(fixture / "calls.txt").empty(),
              "host open failure prevents ADB operation");
        std::atomic<bool> cancel{false};
        scene("big-log");
        auto streamed =
            testtools::ProcessRunner::runManaged(program,
                                                  {"-s", "device-A", "logcat", "-d"},
                                                  std::chrono::seconds(2),
                                                  cancel,
                                                  {},
                                                  [](const char*, std::size_t) { return false; });
        check(streamed.outputFailed && streamed.command.standardOutput.empty(),
              "sink failure stops client without retaining stdout in memory");

        scene("success");
        check(AdbTerminalService().shellUid(request.device) == 0 &&
                  read(fixture / "calls.txt").find("\troot\t") == std::string::npos,
              "terminal identity reads current root without requesting root");
        scene("uid-denied");
        check(AdbTerminalService().shellUid(request.device) == 2000,
              "terminal identity retains ordinary shell uid");
        scene("uid-invalid");
        check(AdbTerminalService().shellUid(request.device) == -1,
              "unrecognized identity remains unknown");
        request.action = MaintenanceAction::RootRemount;
        scene("linux");
        result = run();
        check(result.outcome == MaintenanceOutcome::Failed &&
                  read(fixture / "calls.txt").find("\troot\t") == std::string::npos,
              "non Android target does not receive root");
        scene("root-denied");
        result = run();
        check(!result.rootVerified && result.outcome == MaintenanceOutcome::Failed &&
                  read(fixture / "calls.txt").find("remount") == std::string::npos,
              "production root denial never runs remount");
        scene("uid-denied");
        result = run();
        check(!result.rootVerified && result.outcome == MaintenanceOutcome::TimedOut &&
                  read(fixture / "calls.txt").find("remount") == std::string::npos,
              "root requires numeric uid verification");
        scene("remount-fail");
        result = run();
        check(result.rootVerified && !result.remountAccepted &&
                  result.outcome == MaintenanceOutcome::Partial,
              "root and failed remount are reported separately even with exit 0");
        scene("remount-unknown");
        result = run();
        check(result.outcome == MaintenanceOutcome::Partial && !result.remountAccepted,
              "unrecognized remount response is not success");
        scene("remount-mixed");
        result = run();
        check(result.outcome == MaintenanceOutcome::Partial && !result.remountAccepted,
              "mixed remount response is partial");
        scene("success");
        result = run();
        check(result.rootVerified && result.remountAccepted &&
                  result.outcome == MaintenanceOutcome::Completed,
              "root and remount stages complete");
        scene("remount-success-reboot");
        result = run();
        check(result.rootVerified && result.remountAccepted && result.remountRebootRequired &&
                  result.outcome == MaintenanceOutcome::Partial &&
                  read(fixture / "calls.txt").find("\treboot\t") == std::string::npos,
              "first remount success and required manual reboot reported separately");
        scene("remount-prepare-reboot");
        result = run();
        check(!result.remountAccepted && result.remountRebootRequired &&
                  result.outcome == MaintenanceOutcome::Partial,
              "overlayfs preparation requiring reboot is not yet remount success");
        request.action = MaintenanceAction::Reboot;
        scene("success");
        result = run();
        check(result.commandIssued && result.offlineObserved && result.onlineObserved &&
                  result.outcome == MaintenanceOutcome::Completed,
              "reboot observes offline then same target online");
        scene("reboot-never-offline");
        result = run();
        check(!result.offlineObserved && result.outcome == MaintenanceOutcome::TimedOut,
              "command return alone does not prove a reboot");
        scene("reboot-never-online");
        scene("reboot-state-error");
        result = run();
        check(!result.offlineObserved && result.outcome == MaintenanceOutcome::TimedOut &&
                  result.detail.find("ADB server could not start") != std::string::npos,
              "client error is not device offline evidence");
        scene("reboot-never-online");
        check(service->prepare(request, {}, error), "reboot wait prepared");
        std::thread rebootWorker([&] { service->executePrepared(); });
        while (service->snapshot().active && !service->snapshot().progress.commandIssued)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        service->cancel();
        rebootWorker.join();
        result = service->snapshot().result;
        check(result.commandIssued && result.outcome == MaintenanceOutcome::Cancelled,
              "stopping reboot wait preserves issued action");
        request.action = MaintenanceAction::Loader;
        check(!service->prepare(request, {}, error),
              "default product exposes no unverified Loader target");
        auto loader =
            DeviceMaintenanceService(createAdbDeviceMaintenanceBackend(program));
        scene("loader-unknown");
        loader.setLoaderTarget(request.device); auto probe = loader.prepareLoaderProbe(); loader.executeLoaderProbe(probe);
        check(!loader.loaderSupported(request.device), "platform name alone does not enable Loader");
        loader.cancelLoaderProbe(); scene("success");
        probe = loader.prepareLoaderProbe(); loader.executeLoaderProbe(probe);
        check(loader.prepare(request, {}, error), "explicit fixture proof enables Loader");
        loader.executePrepared();
        result = loader.snapshot().result;
        check(result.commandIssued && result.offlineObserved &&
                  result.outcome == MaintenanceOutcome::Partial && !result.onlineObserved,
              "ADB offline is not Loader proof");
        check(read(fixture / "calls.txt").find("reboot\tloader") != std::string::npos,
              "fixed Loader command");
        loader.cancelLoaderProbe(); scene("success");
        probe = loader.prepareLoaderProbe(); loader.executeLoaderProbe(probe);
        check(loader.prepare(request, {}, error), "fresh Loader fixture prepared");
        scene("loader-denied"); loader.executePrepared();
        check(!loader.snapshot().result.commandIssued && loader.snapshot().result.outcome == MaintenanceOutcome::Failed,
              "runtime capability loss prevents Loader request");
        std::cout
            << "PASS native streaming, binary data, overwrite, partial failures, cancellation, "
               "timeout, root identity, remount, reboot states and Loader qualification\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
