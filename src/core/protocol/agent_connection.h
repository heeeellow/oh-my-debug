#pragma once

#include <cstdint>
#include <string>

namespace testtools {

class IAgentConnection {
  public:
    virtual ~IAgentConnection() = default;

    virtual bool connect(const std::string& host, std::uint16_t port, std::string& error) = 0;
    virtual void close() = 0;
    virtual bool
    request(const std::string& json, std::string& responseJson, std::string& error) = 0;
};

} // namespace testtools
