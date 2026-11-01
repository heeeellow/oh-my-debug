#pragma once

#include "core/serial/serial_types.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>

namespace testtools {
enum class MaintenanceAction { Logcat, Dmesg, RootRemount, Reboot, Loader };
enum class MaintenanceOutcome { None, Completed, Partial, Failed, Cancelled, TimedOut };
enum class MaintenanceStage {
    Ready,
    Exporting,
    CheckingPlatform,
    Root,
    WaitingRoot,
    Remount,
    Reboot,
    WaitingOffline,
    WaitingOnline,
    Finished
};

inline bool disruptive(MaintenanceAction action)
{
    return action == MaintenanceAction::RootRemount || action == MaintenanceAction::Reboot ||
           action == MaintenanceAction::Loader;
}
struct MaintenanceRequest {
    serialdemo::AdbDevice device;
    MaintenanceAction action = MaintenanceAction::Dmesg;
    std::filesystem::path output;
    bool overwrite = false;
    std::chrono::milliseconds commandTimeout{30000};
    std::chrono::milliseconds recoveryTimeout{60000};
};
struct MaintenanceProgress {
    MaintenanceStage stage = MaintenanceStage::Ready;
    std::string message;
    std::uint64_t bytes = 0;
    std::filesystem::path artifact;
    bool commandIssued = false;
};
struct MaintenanceResult {
    MaintenanceOutcome outcome = MaintenanceOutcome::None;
    std::string message, detail;
    std::filesystem::path artifact;
    std::uint64_t bytes = 0;
    bool commandIssued = false;
    bool rootVerified = false;
    bool remountAccepted = false;
    bool offlineObserved = false;
    bool onlineObserved = false;
};
struct MaintenanceConflicts {
    bool files = false, serial = false, board = false, mirror = false, preparingService = false;
    bool any() const
    {
        return files || serial || board || mirror || preparingService;
    }
};
class IDeviceMaintenanceBackend {
  public:
    virtual ~IDeviceMaintenanceBackend() = default;
    // 本地已核实配置查询，不进行设备 I/O；不得仅根据 Rockchip 名称猜测支持。
    virtual bool loaderSupported(const serialdemo::AdbDevice&) const
    {
        return false;
    }
    virtual MaintenanceResult
    execute(const MaintenanceRequest&,
            const std::atomic<bool>& cancel,
            const std::function<void(const MaintenanceProgress&)>& progress) = 0;
};
} // namespace testtools
