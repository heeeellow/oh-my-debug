#pragma once

#include "core/agent_version.h"

#include "core/serial_types.h"

#include <cstdint>
#include <functional>
#include <string>

namespace testtools {

struct AgentDeploymentResult {
    bool succeeded = false;
    std::string error;
    std::string abi;
    std::string binaryPath;
    std::string remotePath;
    std::uint16_t localPort = 0;
};

class AgentDeployment {
public:
    using ProgressCallback = std::function<void(const std::string &)>;

    AgentDeploymentResult deployAndForward(const std::string &serial,
                                           const std::string &applicationDirectory,
                                           const ProgressCallback &progress = {}) const;
    AgentDeploymentResult reconnect(const std::string &serial,
                                    const ProgressCallback &progress = {}) const;
    void removeForward(const std::string &serial, std::uint16_t localPort) const;
    void removeControlForwards(const std::string &serial) const;
    bool stopRemote(const std::string &serial, std::string &error) const;

    static constexpr std::uint16_t ControlPort = 27183;
    static constexpr const char *RequiredAgentVersion = AgentVersion;
    static constexpr const char *RemoteDirectory = "/data/local/tmp/testtools-agent";
    static constexpr const char *RemoteBinary = "/data/local/tmp/testtools-agent/testtools-agent";
};

} // namespace testtools
