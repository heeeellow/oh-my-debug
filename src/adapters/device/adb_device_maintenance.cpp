#include "adapters/device/adb_device_maintenance.h"
#include "adapters/adb/adb_transport.h"
#include "application/device/device_maintenance_service.h"
#include <algorithm>
#include <cctype>
#include <random>
#include <thread>
#include <windows.h>

namespace testtools {
namespace {
using Clock = std::chrono::steady_clock;
using Command = serialdemo::ManagedCommandResult;
std::string trim(std::string text)
{
    const auto start = text.find_first_not_of(" \r\n\t");
    return start == std::string::npos
               ? std::string{}
               : text.substr(start, text.find_last_not_of(" \r\n\t") - start + 1);
}
bool ok(const Command& command)
{
    return command.started && command.command.exitCode == 0 && !command.cancelled &&
           !command.command.timedOut && !command.outputTruncated && !command.outputFailed;
}
std::string commandDetail(const Command& command)
{
    auto text = trim(command.command.standardOutput + "\n" + command.command.standardError);
    if (command.outputTruncated)
        text += "\n诊断输出达到限额";
    if (command.outputFailed)
        text += "\n本地日志文件写入失败";
    return text;
}
MaintenanceOutcome outcome(const Command& command)
{
    if (command.cancelled)
        return MaintenanceOutcome::Cancelled;
    if (command.command.timedOut)
        return MaintenanceOutcome::TimedOut;
    return MaintenanceOutcome::Failed;
}
void append(std::string& detail, const std::string& title, const std::string& text)
{
    constexpr std::size_t limit = 48 * 1024;
    if (detail.size() >= limit)
        return;
    const auto value = title + "\n" + text + "\n";
    detail.append(value, 0, std::min(value.size(), limit - detail.size()));
}

// 独占创建同目录临时文件，完成后再原子替换；失败不覆盖原文件，保留部分日志。
class LogArtifact {
    HANDLE file_ = INVALID_HANDLE_VALUE;

  public:
    std::filesystem::path partial;
    std::uint64_t bytes = 0;
    ~LogArtifact()
    {
        close();
    }
    void close()
    {
        if (file_ != INVALID_HANDLE_VALUE) {
            CloseHandle(file_);
            file_ = INVALID_HANDLE_VALUE;
        }
    }
    bool open(const MaintenanceRequest& request, std::string& error)
    {
        const auto attrs = GetFileAttributesW(request.output.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES &&
            (!request.overwrite ||
             (attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))) {
            error = "目标已经存在且未授权覆盖，或目标不是普通文件";
            return false;
        }
        for (int attempt = 0; attempt < 8; ++attempt) {
            partial = request.output;
            partial += L".partial-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                       std::to_wstring(std::random_device{}());
            file_ = CreateFileW(partial.c_str(),
                                GENERIC_WRITE,
                                FILE_SHARE_READ,
                                nullptr,
                                CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL,
                                nullptr);
            if (file_ != INVALID_HANDLE_VALUE)
                return true;
            if (GetLastError() != ERROR_FILE_EXISTS)
                break;
        }
        partial.clear();
        error = "无法创建本地日志文件，Windows 错误 " + std::to_string(GetLastError());
        return false;
    }
    bool write(const char* data, std::size_t size)
    {
        while (size) {
            DWORD written = 0;
            const auto chunk = DWORD(std::min<std::size_t>(size, 1024 * 1024));
            if (!WriteFile(file_, data, chunk, &written, nullptr) || !written)
                return false;
            bytes += written;
            data += written;
            size -= written;
        }
        return true;
    }
    bool commit(const MaintenanceRequest& request, std::string& error)
    {
        if (!FlushFileBuffers(file_)) {
            error = "日志文件刷盘失败";
            close();
            return false;
        }
        close();
        const auto attrs = GetFileAttributesW(request.output.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES &&
            (attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
            error = "目标已变为目录或链接，完整日志保留在临时文件";
            return false;
        }
        if (!MoveFileExW(partial.c_str(),
                         request.output.c_str(),
                         MOVEFILE_WRITE_THROUGH |
                             (request.overwrite ? MOVEFILE_REPLACE_EXISTING : 0))) {
            error = "无法提交日志文件，Windows 错误 " + std::to_string(GetLastError());
            return false;
        }
        return true;
    }
};

class AdbDeviceMaintenance final : public IDeviceMaintenanceBackend {
    std::string program_;
    std::set<std::string> loaders_;

  public:
    AdbDeviceMaintenance(const std::string& program, std::set<std::string> loaders)
        : program_(serialdemo::AdbTransport(program).program()), loaders_(std::move(loaders))
    {
    }
    bool loaderSupported(const serialdemo::AdbDevice& device) const override
    {
        return loaders_.count(device.serial) != 0;
    }
    MaintenanceResult
    execute(const MaintenanceRequest& request,
            const std::atomic<bool>& cancel,
            const std::function<void(const MaintenanceProgress&)>& progress) override
    {
        MaintenanceResult result;
        auto publish = [&](MaintenanceStage stage, const std::string& message) {
            progress({stage, message, result.bytes, result.artifact, result.commandIssued});
        };
        auto run = [&](const std::vector<std::string>& arguments,
                       std::chrono::milliseconds timeout) {
            std::vector<std::string> args{"-s", request.device.serial};
            args.insert(args.end(), arguments.begin(), arguments.end());
            return serialdemo::ProcessRunner::runManaged(program_, args, timeout, cancel);
        };
        if (!disruptive(request.action)) {
            LogArtifact artifact;
            if (!artifact.open(request, result.detail)) {
                result.outcome = MaintenanceOutcome::Failed;
                result.message = "无法保存日志";
                return result;
            }
            result.artifact = artifact.partial;
            publish(MaintenanceStage::Exporting, "正在导出日志…");
            const auto args =
                request.action == MaintenanceAction::Logcat
                    ? std::vector<std::string>{"-s", request.device.serial, "logcat", "-d"}
                    : std::vector<std::string>{"-s", request.device.serial, "shell", "-T", "dmesg"};
            const auto command =
                serialdemo::ProcessRunner::runManaged(program_,
                                                      args,
                                                      request.commandTimeout,
                                                      cancel,
                                                      {},
                                                      [&](const char* data, std::size_t size) {
                                                          const auto saved =
                                                              artifact.write(data, size);
                                                          progress({MaintenanceStage::Exporting,
                                                                    "正在导出日志…",
                                                                    artifact.bytes,
                                                                    artifact.partial,
                                                                    false});
                                                          return saved;
                                                      });
            result.bytes = artifact.bytes;
            result.detail = commandDetail(command);
            if (ok(command) && !cancel) {
                std::string error;
                if (artifact.commit(request, error)) {
                    result.artifact = request.output;
                    result.outcome = MaintenanceOutcome::Completed;
                    result.message = "日志已导出";
                    return result;
                }
                append(result.detail, "提交文件失败", error);
                result.outcome = MaintenanceOutcome::Failed;
            } else
                result.outcome = cancel ? MaintenanceOutcome::Cancelled : outcome(command);
            artifact.close();
            if (result.outcome == MaintenanceOutcome::Failed && result.bytes)
                result.outcome = MaintenanceOutcome::Partial;
            result.message =
                result.outcome == MaintenanceOutcome::Cancelled  ? "导出已取消，保留部分日志"
                : result.outcome == MaintenanceOutcome::TimedOut ? "导出超时，保留部分日志"
                                                                 : "导出未完成，保留部分日志";
            return result;
        }
        if (request.action == MaintenanceAction::Loader && !loaderSupported(request.device))
            return {MaintenanceOutcome::Failed, "当前设备尚未确认支持 Loader"};
        if (request.action == MaintenanceAction::RootRemount) {
            publish(MaintenanceStage::CheckingPlatform, "正在确认 Android 系统…");
            const auto platform =
                run({"shell", "-T", "getprop ro.build.version.sdk"}, request.commandTimeout);
            const auto sdk = trim(platform.command.standardOutput);
            if (!ok(platform) || sdk.empty() ||
                !std::all_of(
                    sdk.begin(), sdk.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
                result.outcome = ok(platform) ? MaintenanceOutcome::Failed : outcome(platform);
                result.message = "无法确认 Android，未执行 Root 或 Remount";
                result.detail = commandDetail(platform);
                return result;
            }
            publish(MaintenanceStage::Root, "正在请求 Root…");
            const auto root = run({"root"}, request.commandTimeout);
            result.commandIssued = root.started;
            append(result.detail, "Root", commandDetail(root));
            if (!ok(root) || commandDetail(root).find("cannot run as root") != std::string::npos) {
                result.outcome = ok(root) ? MaintenanceOutcome::Failed : outcome(root);
                result.message = "Root 未完成，设备状态需要复核";
                return result;
            }
            publish(MaintenanceStage::WaitingRoot, "等待原设备恢复并核验 Root 身份…");
            const auto deadline = Clock::now() + request.recoveryTimeout;
            std::string lastIdentity;
            while (!cancel && Clock::now() < deadline) {
                const auto remaining = std::max(
                    std::chrono::milliseconds(1),
                    std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()));
                const auto identity =
                    run({"shell", "-T", "id -u"}, std::min(remaining, request.commandTimeout));
                lastIdentity = commandDetail(identity);
                if (ok(identity) && trim(identity.command.standardOutput) == "0") {
                    result.rootVerified = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (!result.rootVerified || cancel) {
                append(result.detail, "最后一次身份核验", lastIdentity);
                result.outcome =
                    cancel ? MaintenanceOutcome::Cancelled : MaintenanceOutcome::TimedOut;
                result.message = "Root 请求已发送，身份核验未完成；未执行 Remount";
                return result;
            }
            publish(MaintenanceStage::Remount, "Root 已核验，正在尝试 Remount…");
            const auto remount = run({"remount"}, request.commandTimeout);
            append(result.detail, "Remount", commandDetail(remount));
            auto text = commandDetail(remount);
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
                return char(std::tolower(c));
            });
            result.remountAccepted =
                ok(remount) && text.find("remount succeeded") != std::string::npos &&
                text.find("failed") == std::string::npos &&
                text.find("error") == std::string::npos && text.find("denied") == std::string::npos;
            // AOSP 可以同时报告 Remount succeeded 和需重启；命令成功与设置生效分开保留。
            result.remountRebootRequired =
                text.find("now reboot your device") != std::string::npos ||
                text.find("reboot the device for new settings") != std::string::npos ||
                text.find("please reboot") != std::string::npos ||
                text.find("reboot required") != std::string::npos ||
                text.find("reboot is required") != std::string::npos;
            result.outcome = result.remountAccepted && !result.remountRebootRequired
                                 ? MaintenanceOutcome::Completed
                             : cancel                   ? MaintenanceOutcome::Cancelled
                             : remount.command.timedOut ? MaintenanceOutcome::TimedOut
                                                        : MaintenanceOutcome::Partial;
            result.message = result.remountAccepted && result.remountRebootRequired
                                 ? "Root 已核验；Remount 返回成功，部分设置需手动重启生效"
                             : result.remountAccepted
                                 ? "Root 已核验，Remount 已返回成功；分区可写性未逐项验证"
                             : result.remountRebootRequired && ok(remount)
                                 ? "Root 已核验；Remount 提示需手动重启后重试，请查看详情"
                                 : "Root 已核验，Remount 未确认完成；请查看详情";
            return result;
        }
        publish(MaintenanceStage::Reboot,
                request.action == MaintenanceAction::Loader ? "正在请求 Loader…"
                                                            : "正在发送重启命令…");
        const auto reboot = run(request.action == MaintenanceAction::Loader
                                    ? std::vector<std::string>{"reboot", "loader"}
                                    : std::vector<std::string>{"reboot"},
                                request.commandTimeout);
        result.commandIssued = reboot.started;
        result.detail = commandDetail(reboot);
        if (!ok(reboot)) {
            result.outcome = outcome(reboot);
            result.message = "重启请求未确认完成；若命令已发出，停止等待不会撤销设备动作";
            return result;
        }
        const auto deadline = Clock::now() + request.recoveryTimeout;
        publish(MaintenanceStage::WaitingOffline, "命令已发出，等待原设备 ADB 离线…");
        std::string lastState;
        while (!cancel && Clock::now() < deadline) {
            const auto remaining = std::max(
                std::chrono::milliseconds(1),
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()));
            const auto state = run({"get-state"}, std::min(remaining, request.commandTimeout));
            const bool online = ok(state) && trim(state.command.standardOutput) == "device";
            lastState = commandDetail(state);
            const auto error = trim(state.command.standardError);
            const bool offline = trim(state.command.standardOutput) == "offline" ||
                                 error.find("error: device offline") != std::string::npos ||
                                 error.find("error: device not found") != std::string::npos ||
                                 error.find("error: device '" + request.device.serial +
                                            "' not found") != std::string::npos ||
                                 error.find("no devices/emulators found") != std::string::npos;
            if (offline && state.started && !state.cancelled && !state.command.timedOut)
                result.offlineObserved = true;
            if (result.offlineObserved) {
                if (request.action == MaintenanceAction::Loader) {
                    result.outcome = MaintenanceOutcome::Partial;
                    append(result.detail, "ADB 状态", lastState);
                    result.message = "ADB 已离线；是否进入 Loader 需核对 USB 枚举或实机";
                    return result;
                }
                publish(MaintenanceStage::WaitingOnline, "已观察到 ADB 离线，等待原设备重新上线…");
                if (online) {
                    result.onlineObserved = true;
                    result.outcome = MaintenanceOutcome::Completed;
                    result.message = "原设备 ADB 已重新上线；系统启动完成尚未验证";
                    return result;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        result.outcome = cancel ? MaintenanceOutcome::Cancelled : MaintenanceOutcome::TimedOut;
        append(result.detail, "最后一次 ADB 状态查询", lastState);
        result.message = cancel                   ? "已停止等待；重启命令已发出，无法撤销"
                         : result.offlineObserved ? "重启命令已发出，等待原设备上线超时"
                                                  : "重启命令已发出，未观察到完整离线/上线过程";
        return result;
    }
};
} // namespace
std::shared_ptr<IDeviceMaintenanceBackend>
createAdbDeviceMaintenanceBackend(const std::string& program, const std::set<std::string>& loaders)
{
    return std::make_shared<AdbDeviceMaintenance>(program, loaders);
}
std::shared_ptr<DeviceMaintenanceService> createDeviceMaintenanceService(const std::string& program)
{
    return std::make_shared<DeviceMaintenanceService>(createAdbDeviceMaintenanceBackend(program));
}
} // namespace testtools
