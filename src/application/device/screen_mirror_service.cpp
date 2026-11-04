#include "application/device/screen_mirror_service.h"

#include <utility>

namespace testtools {

ScreenMirrorService::ScreenMirrorService(std::unique_ptr<IScreenMirrorBackend> backend)
    : backend_(std::move(backend)) {}

MirrorOperationResult ScreenMirrorService::start(const testtools::AdbDevice& device)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (shuttingDown_) return {false, "程序正在关闭"};
    if (device.serial.empty() || device.state != "device")
        return {false, "请先选择在线且已授权的 Android ADB 设备"};
    refreshLocked();
    if (state_.running) return {false, "请先停止 " + state_.serial + " 的投屏"};
    state_ = {false, device.serial, "正在启动投屏", {}};
    std::string error;
    if (!backend_->start(device.serial, error)) {
        state_.message = "投屏启动失败";
        state_.detail = error;
        return {false, error};
    }
    state_.running = true;
    state_.message = "投屏中";
    return {true, {}};
}

void ScreenMirrorService::stop()
{
    std::lock_guard<std::mutex> lock(mutex_);
    backend_->stop();
    state_.running = false;
    state_.message = "投屏已停止";
    state_.detail.clear();
}

void ScreenMirrorService::requestShutdown() noexcept
{
    shuttingDown_ = true;
    backend_->requestStop();
}

void ScreenMirrorService::refreshLocked()
{
    if (!state_.running) return;
    const auto process = backend_->poll();
    if (!process.running) {
        state_.running = false;
        state_.message = process.exitCode == 0 ? "投屏窗口已关闭" : "投屏已断开";
        state_.detail = process.exitCode == 0 ? std::string{} : process.detail;
    }
}

ScreenMirrorSnapshot ScreenMirrorService::poll()
{
    std::lock_guard<std::mutex> lock(mutex_);
    refreshLocked();
    return state_;
}

ScreenMirrorSnapshot ScreenMirrorService::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

MirrorOperationResult ScreenMirrorService::control(MirrorControl control)
{
    std::lock_guard<std::mutex> lock(mutex_);
    refreshLocked();
    if (!state_.running || shuttingDown_) return {false, "请先启动投屏"};
    std::string error;
    const bool success = backend_->control(control, error);
    state_.message = success ? "投屏中" : "设备操作失败";
    state_.detail = error;
    return {success, error};
}

MirrorOperationResult ScreenMirrorService::screenshot(const std::filesystem::path& path)
{
    std::lock_guard<std::mutex> lock(mutex_);
    refreshLocked();
    if (!state_.running || shuttingDown_) return {false, "请先启动投屏"};
    std::string error;
    const bool success = backend_->screenshot(path, error);
    state_.message = success ? "截图已保存：" + path.u8string() : "截图保存失败";
    state_.detail = error;
    return {success, error};
}

} // namespace testtools
