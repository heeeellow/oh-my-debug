#pragma once
#include "core/device/device_maintenance.h"
#include <mutex>

namespace testtools {
struct MaintenanceSnapshot {
    std::uint64_t id = 0;
    bool active = false, cancelling = false;
    MaintenanceRequest request;
    MaintenanceProgress progress;
    MaintenanceResult result;
};

// prepare/cancel/snapshot 不执行 I/O；executePrepared 由表现层放入所拥有的后台任务。
class DeviceMaintenanceService {
  public:
    explicit DeviceMaintenanceService(std::shared_ptr<IDeviceMaintenanceBackend> backend);
    bool loaderSupported(const testtools::AdbDevice& device) const;
    bool prepare(MaintenanceRequest request, const MaintenanceConflicts&, std::string& error);
    void executePrepared();
    void cancel(std::uint64_t expectedId = 0) noexcept;
    MaintenanceSnapshot snapshot() const;

  private:
    std::shared_ptr<IDeviceMaintenanceBackend> backend_;
    mutable std::mutex mutex_;
    std::atomic<bool> cancel_{false};
    bool executing_ = false;
    MaintenanceSnapshot state_;
};
std::shared_ptr<DeviceMaintenanceService>
createDeviceMaintenanceService(const std::string& adbProgram = {});
} // namespace testtools
