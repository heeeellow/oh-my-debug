#pragma once
#include "core/protocol/agent_connection.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace testtools {

// 桌面端维护的调试服务连接状态机。
enum class AgentServiceState { Disconnected, Connecting, Ready, Failed };

// 板端 system.info 返回的设备身份与硬件规格摘要。
struct AgentSystemInfo {
    std::string model;
    std::string manufacturer;
    std::string brand;
    std::string product;
    std::string board;
    std::string hardware;
    std::string architecture;
    int cpuCores = 0;
    std::int64_t memoryTotalKib = 0;
    std::string osRelease;
    std::string androidSdk;
    std::string kernel;
    std::string buildId;
    int effectiveUid = -1;
};

// 从 session.hello、capability.list 与 system.info 报文提取的结构化服务快照。
struct AgentServiceSnapshot {
    std::string agentVersion;
    std::vector<std::string> capabilityIds;
    AgentSystemInfo systemInfo;

    bool hasCapability(std::string_view capabilityId) const;
};

// 固定只读协议健康检查，绝不复用活动的串口流连接。
bool validateAgentService(const std::string& hello,
                          const std::string& capabilities,
                          const std::string& requiredVersion,
                          std::string& error);
bool probeAgentService(IAgentConnection& connection,
                       const std::string& requiredVersion,
                       std::string& error);

// 解析握手、能力列表与系统信息 JSON 报文，供界面与用例层直接使用。
AgentServiceSnapshot parseAgentServiceSnapshot(const std::string& hello,
                                               const std::string& capabilities,
                                               const std::string& systemInfo);

} // namespace testtools
