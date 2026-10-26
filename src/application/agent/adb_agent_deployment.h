#pragma once

#include "core/protocol/agent_version.h"

#include <cstdint>
#include <functional>
#include <string>

namespace testtools {

// 跨 Android 与 Linux 平台的统一 ADB Agent 部署与端口转发结果。
struct AdbAgentDeploymentResult {
    bool succeeded = false;
    std::string error;
    std::string platform;
    std::string target;
    std::string abi;
    std::string buildId;
    std::string binaryPath;
    std::string remotePath;
    std::uint16_t localPort = 0;
};

// 统一部署门面：自动探测远端系统类型（Android/Linux），委派对应部署器并管理回环转发。
class AdbAgentDeployment {
  public:
    using ProgressCallback = std::function<void(const std::string&)>;

    AdbAgentDeploymentResult deployAndForward(const std::string& serial,
                                              const std::string& applicationDirectory,
                                              const ProgressCallback& progress = {}) const;
    AdbAgentDeploymentResult reconnect(const std::string& serial,
                                       const ProgressCallback& progress = {}) const;
    void removeForward(const std::string& serial, std::uint16_t localPort) const;
    void removeControlForwards(const std::string& serial) const;
    bool stopRemote(const std::string& serial, std::string& error) const;

    static constexpr const char* RequiredAgentVersion = AgentVersion;
};

} // namespace testtools
