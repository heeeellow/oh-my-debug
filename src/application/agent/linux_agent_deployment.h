#pragma once

#include "core/protocol/agent_version.h"

#include <cstdint>
#include <functional>
#include <string>

namespace testtools {

// Linux 平台板端 Agent 部署结果。
struct LinuxAgentDeploymentResult {
    bool succeeded = false;
    std::string error;
    std::string architecture;
    std::string libc;
    std::string buildId;
    std::string binaryPath;
    std::string remotePath;
    std::uint16_t localPort = 0;
};

// Linux 平台板端 Agent 部署器：管理 /opt/testtools-agent 下的原子安装、回滚与后台守护进程。
class LinuxAgentDeployment {
  public:
    using ProgressCallback = std::function<void(const std::string&)>;

    LinuxAgentDeploymentResult deployAndForward(const std::string& serial,
                                                const std::string& applicationDirectory,
                                                const ProgressCallback& progress = {}) const;
    LinuxAgentDeploymentResult reconnect(const std::string& serial,
                                         const ProgressCallback& progress = {}) const;
    void removeForward(const std::string& serial, std::uint16_t localPort) const;
    void removeControlForwards(const std::string& serial) const;
    bool stopRemote(const std::string& serial, std::string& error) const;

    static constexpr std::uint16_t ControlPort = 27183;
    static constexpr const char* RequiredAgentVersion = AgentVersion;
    static constexpr const char* RemoteRoot = "/opt/testtools-agent";
    static constexpr const char* RemoteVersions = "/opt/testtools-agent/versions";
    static constexpr const char* RemoteCurrent = "/opt/testtools-agent/current";
    static constexpr const char* RemotePrevious = "/opt/testtools-agent/previous";
    static constexpr const char* RemoteLog = "/var/log/testtools-agent/agent.log";
    static constexpr const char* RemotePid = "/run/testtools-agent/agent.pid";
    static constexpr const char* RemoteStagingRoot = "/opt/testtools-agent/.staging";
};

} // namespace testtools
