#pragma once

#include <filesystem>
#include <string>

namespace testtools {

// 屏幕镜像控制按键与外部镜像进程后端抽象接口。
enum class MirrorControl { Back, Home, Recents, Power, VolumeUp, VolumeDown };

struct MirrorProcessStatus {
    bool running = false;
    int exitCode = 0;
    std::string detail;
};

class IScreenMirrorBackend {
public:
    virtual ~IScreenMirrorBackend() = default;
    virtual bool start(const std::string& serial, std::string& error) = 0;
    virtual void stop() = 0;
    virtual void requestStop() noexcept = 0;
    virtual MirrorProcessStatus poll() = 0;
    virtual bool control(MirrorControl control, std::string& error) = 0;
    virtual bool screenshot(const std::filesystem::path& path, std::string& error) = 0;
};

} // namespace testtools
