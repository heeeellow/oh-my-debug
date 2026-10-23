#pragma once

#include "core/device/screen_mirror.h"
#include "core/serial/serial_types.h"

#include <atomic>
#include <memory>
#include <mutex>

namespace testtools {

struct ScreenMirrorSnapshot {
    bool running = false;
    std::string serial;
    std::string message = "尚未投屏";
    std::string detail;
};

struct MirrorOperationResult {
    bool success = false;
    std::string error;
};

class ScreenMirrorService {
public:
    explicit ScreenMirrorService(std::unique_ptr<IScreenMirrorBackend> backend);
    MirrorOperationResult start(const serialdemo::AdbDevice& device);
    void stop();
    void requestShutdown() noexcept;
    ScreenMirrorSnapshot poll();
    ScreenMirrorSnapshot snapshot() const;
    MirrorOperationResult control(MirrorControl control);
    MirrorOperationResult screenshot(const std::filesystem::path& path);

private:
    void refreshLocked();
    mutable std::mutex mutex_;
    std::unique_ptr<IScreenMirrorBackend> backend_;
    std::atomic<bool> shuttingDown_{false};
    ScreenMirrorSnapshot state_;
};

// Composition entry point; the desktop adapter supplies the concrete backend.
std::shared_ptr<ScreenMirrorService> createScreenMirrorService();

} // namespace testtools
