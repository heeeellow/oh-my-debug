#pragma once

#include "serial_bridge.h"

#include <mutex>
#include <string>

namespace testtools::agent {

class AgentService {
public:
    AgentService() = default;
    ~AgentService();

    std::string handleRequest(const std::string &request);
    void stop();

private:
    std::string serialListResponse(const std::string &requestId) const;
    std::string serialOpenResponse(const std::string &requestId, const std::string &request);
    std::string serialCloseResponse(const std::string &requestId, const std::string &request);

    mutable std::mutex serialMutex_;
    SerialBridge serialBridge_;
    std::string activeSerialSessionId_;
};

} // namespace testtools::agent
