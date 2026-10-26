#pragma once
#include <chrono>

#include "core/protocol/agent_connection.h"
#include "core/protocol/agent_protocol.h"

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace testtools {

// 基于 Asio TCP Socket 的板端 Agent 控制通道客户端实现（带超时与长度前缀帧编解码）。
class AgentClient final : public IAgentConnection {
  public:
    explicit AgentClient(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));
    ~AgentClient() override;
    AgentClient(const AgentClient&) = delete;
    AgentClient& operator=(const AgentClient&) = delete;

    bool connect(const std::string& host, std::uint16_t port, std::string& error) override;
    void close() override;
    bool request(const std::string& json, std::string& responseJson, std::string& error) override;
    bool isConnected() const;

  private:
    std::chrono::milliseconds timeout_;
    asio::io_context ioContext_;
    std::unique_ptr<asio::ip::tcp::socket> socket_;
    AgentFrameDecoder decoder_;
};

} // namespace testtools
