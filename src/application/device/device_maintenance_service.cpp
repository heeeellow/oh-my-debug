#include "application/device/device_maintenance_service.h"
#include <algorithm>
#include <exception>

namespace testtools {
DeviceMaintenanceService::DeviceMaintenanceService(
    std::shared_ptr<IDeviceMaintenanceBackend> backend)
    : backend_(std::move(backend))
{
}

bool DeviceMaintenanceService::loaderSupported(const serialdemo::AdbDevice& device) const
{
    return backend_ && backend_->loaderSupported(device);
}
bool DeviceMaintenanceService::prepare(MaintenanceRequest request,
                                       const MaintenanceConflicts& conflicts,
                                       std::string& error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    error.clear();
    if (state_.active)
        error = "已有诊断或维护任务，请等待结束";
    else if (!backend_)
        error = "设备维护后端不可用";
    else if (request.device.serial.empty() || request.device.serial.size() > 512 ||
             request.device.state != "device")
        error = "请先选择在线且已授权的 ADB 设备";
    else if (request.device.serial.find_first_of("\r\n\t") != std::string::npos ||
             request.device.serial.find('\0') != std::string::npos)
        error = "设备编号无效";
    else if (int(request.action) < int(MaintenanceAction::Logcat) ||
             int(request.action) > int(MaintenanceAction::Loader))
        error = "不支持的维护动作";
    else if (request.commandTimeout.count() <= 0 ||
             request.commandTimeout > std::chrono::minutes(2) ||
             request.recoveryTimeout.count() <= 0 ||
             request.recoveryTimeout > std::chrono::minutes(3))
        error = "操作超时设置无效";
    else if (!disruptive(request.action) &&
             (!request.output.is_absolute() || !request.output.has_filename() ||
              request.output.filename() == "." || request.output.filename() == ".." ||
              request.output.u8string().find('\0') != std::string::npos))
        error = "请选择完整的本地日志文件路径";
    else if (disruptive(request.action) && conflicts.any())
        error = "请先结束该设备的文件任务、设备串口、板卡任务、投屏或服务准备，再执行维护";
    else if (request.action == MaintenanceAction::Loader && !loaderSupported(request.device))
        error = "当前设备尚未确认支持 Loader，不能执行此动作";
    if (!error.empty())
        return false;
    const auto id = state_.id + 1;
    state_ = {};
    state_.id = id;
    state_.active = true;
    state_.request = std::move(request);
    state_.progress.message = "准备执行";
    cancel_ = false;
    executing_ = false;
    return true;
}
void DeviceMaintenanceService::executePrepared()
{
    MaintenanceRequest request;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!state_.active || executing_)
            return;
        executing_ = true;
        request = state_.request;
    }
    MaintenanceResult result;
    try {
        if (cancel_)
            result = {MaintenanceOutcome::Cancelled, "尚未执行，已取消"};
        else
            result =
                backend_->execute(request, cancel_, [this](const MaintenanceProgress& progress) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    state_.progress = progress;
                });
    } catch (const std::exception& e) {
        result = {MaintenanceOutcome::Failed, "诊断或维护执行失败", e.what()};
    } catch (...) {
        result = {MaintenanceOutcome::Failed, "诊断或维护发生未知错误"};
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (result.outcome == MaintenanceOutcome::None)
        result = {MaintenanceOutcome::Failed, "维护后端未返回有效结果"};
    if (result.artifact.empty())
        result.artifact = state_.progress.artifact;
    result.bytes = std::max(result.bytes, state_.progress.bytes);
    result.commandIssued = result.commandIssued || state_.progress.commandIssued;
    state_.result = std::move(result);
    state_.active = false;
    state_.cancelling = false;
    state_.progress.stage = MaintenanceStage::Finished;
    executing_ = false;
}
void DeviceMaintenanceService::cancel(std::uint64_t expectedId) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!state_.active || (expectedId && expectedId != state_.id))
        return;
    cancel_ = true;
    state_.cancelling = true;
}
MaintenanceSnapshot DeviceMaintenanceService::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}
} // namespace testtools
