#pragma once

#include "agent_protocol.h"

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace testtools {

class AgentClient {
public:
    AgentClient();
    ~AgentClient();
    AgentClient(const AgentClient &) = delete;
    AgentClient &operator=(const AgentClient &) = delete;

    bool connect(const std::string &host, std::uint16_t port, std::string &error);
    void close();
    bool request(const std::string &json, std::string &responseJson, std::string &error);
    bool isConnected() const;

private:
    asio::io_context ioContext_;
    std::unique_ptr<asio::ip::tcp::socket> socket_;
    AgentFrameDecoder decoder_;
};

} // namespace testtools
