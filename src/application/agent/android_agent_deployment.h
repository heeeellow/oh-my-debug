#pragma once

#include "core/protocol/agent_version.h"

#include "core/serial/serial_types.h"

#include <cstdint>
#include <functional>
#include <string>

namespace testtools {

struct AndroidAgentDeploymentResult {
    bool succeeded = false;
    std::string error;
    std::string abi;
    std::string binaryPath;
    std::string remotePath;
    std::uint16_t localPort = 0;
};

class AndroidAgentDeployment {
  public:
    using ProgressCallback = std::function<void(const std::string&)>;

    AndroidAgentDeploymentResult deployAndForward(const std::string& serial,
                                                  const std::string& applicationDirectory,
                                                  const ProgressCallback& progress = {}) const;
    AndroidAgentDeploymentResult reconnect(const std::string& serial,
                                           const ProgressCallback& progress = {}) const;
    void removeForward(const std::string& serial, std::uint16_t localPort) const;
    void removeControlForwards(const std::string& serial) const;
    bool stopRemote(const std::string& serial, std::string& error) const;

    static constexpr std::uint16_t ControlPort = 27183;
    static constexpr const char* RequiredAgentVersion = AgentVersion;
    static constexpr const char* RemoteDirectory = "/data/local/tmp/testtools-agent";
    static constexpr const char* RemoteVersions = "/data/local/tmp/testtools-agent/versions";
    static constexpr const char* RemoteCurrent = "/data/local/tmp/testtools-agent/current";
    static constexpr const char* RemotePrevious = "/data/local/tmp/testtools-agent/previous";
    static constexpr const char* RemoteBinary =
        "/data/local/tmp/testtools-agent/current/testtools-agent";
};

} // namespace testtools
