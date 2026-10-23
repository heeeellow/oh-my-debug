#pragma once

#include "application/agent/adb_agent_deployment.h"
#include "core/protocol/agent_connection.h"

#include <functional>
#include <memory>
#include <string>

namespace testtools {

struct AgentConnectionResult {
    bool succeeded = false;
    std::string error;
    std::string hello;
    std::string capabilities;
    std::string systemInfo;
    std::string observedVersion;
    AdbAgentDeploymentResult deployment;
    std::shared_ptr<IAgentConnection> client;
};

class AgentConnectionService {
  public:
    using ProgressCallback = std::function<void(const std::string&)>;

    static std::shared_ptr<IAgentConnection> createClient();
    static bool checkConnection(std::uint16_t localPort, std::string& error);
    static AgentConnectionResult connectDevice(const std::string& serial,
                                                const std::string& applicationDirectory,
                                                bool allowDeploy,
                                                const ProgressCallback& progress = {});
};

} // namespace testtools
