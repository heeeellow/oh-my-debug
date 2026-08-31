#pragma once

#include "core/protocol/agent_connection.h"
#include "core/protocol/agent_protocol.h"

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace testtools {

class AgentClient final : public IAgentConnection {
  public:
    AgentClient();
    ~AgentClient() override;
    AgentClient(const AgentClient&) = delete;
    AgentClient& operator=(const AgentClient&) = delete;

    bool connect(const std::string& host, std::uint16_t port, std::string& error) override;
    void close() override;
    bool request(const std::string& json, std::string& responseJson, std::string& error) override;
    bool isConnected() const;

  private:
    asio::io_context ioContext_;
    std::unique_ptr<asio::ip::tcp::socket> socket_;
    AgentFrameDecoder decoder_;
};

} // namespace testtools
