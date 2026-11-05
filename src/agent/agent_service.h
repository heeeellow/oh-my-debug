#pragma once

#include "board_capability.h"
#include "board_test_job.h"
#include "core/protocol/json_value.h"
#include "serial_bridge.h"

#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace testtools::agent {

class ToolboxAgent;

// 板端 Agent 核心协议调度服务：统一处理 session.hello、capability.list、system.info、
// 板端只读诊断任务（job.*）与受管串口流（serial.*）请求。
class AgentService {
  public:
    using CapabilityListFunction = std::function<std::vector<BoardCapabilityDefinition>()>;

    AgentService();
    AgentService(BoardTestJobManager::RunFunction boardTestRunner,
                 CapabilityListFunction capabilityList);
    ~AgentService();

    std::string handleRequest(const std::string& request);
    void stop();

  private:
    std::string boardTestStartResponse(const std::string& requestId, const JsonValue& params);
    std::string boardTestStatusResponse(const std::string& requestId,
                                        const JsonValue& params) const;
    std::string boardTestCancelResponse(const std::string& requestId, const JsonValue& params);
    std::string serialListResponse(const std::string& requestId) const;
    std::string serialOpenResponse(const std::string& requestId, const JsonValue& params);
    std::string serialCloseResponse(const std::string& requestId, const JsonValue& params);

    BoardTestJobManager boardTestJobs_;
    std::unique_ptr<ToolboxAgent> toolbox_;
    CapabilityListFunction capabilityList_;
    mutable std::mutex serialMutex_;
    SerialBridge serialBridge_;
    std::string activeSerialSessionId_;
};

} // namespace testtools::agent
