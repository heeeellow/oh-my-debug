#pragma once

#include "application/agent/adb_agent_deployment.h"
#include "core/protocol/agent_connection.h"
#include "core/toolbox/toolbox_host.h"

#include <functional>
#include <memory>
#include <string>

namespace testtools {

// 设备调试服务连接结果（含握手、能力清单、系统信息及活跃连接客户端）。
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

// 板端 Agent 连接编排服务：支持只读重连探测、按需部署升级与连接健康检查。
class AgentConnectionService {
  public:
    using ProgressCallback = std::function<void(const std::string&)>;

    static std::shared_ptr<IAgentConnection> createClient();
    static std::shared_ptr<IToolboxHost> createToolboxHost(const std::string& applicationDirectory);
    static bool checkConnection(std::uint16_t localPort, std::string& error);
    static AgentConnectionResult connectDevice(const std::string& serial,
                                                const std::string& applicationDirectory,
                                                bool allowDeploy,
                                                const ProgressCallback& progress = {});
};

} // namespace testtools
