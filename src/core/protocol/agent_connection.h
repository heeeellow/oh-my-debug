#pragma once

#include <cstdint>
#include <string>

namespace testtools {

// 板端 Agent 控制连接抽象接口，解耦应用层用例与底层 Socket 传输实现。
class IAgentConnection {
  public:
    virtual ~IAgentConnection() = default;

    virtual bool connect(const std::string& host, std::uint16_t port, std::string& error) = 0;
    virtual void close() = 0;
    virtual bool
    request(const std::string& json, std::string& responseJson, std::string& error) = 0;
};

} // namespace testtools
