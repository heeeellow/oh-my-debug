#include "agent_client.h"

#include <asio/connect.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>

#include <array>
#include <vector>

namespace testtools {

AgentClient::AgentClient() = default;

AgentClient::~AgentClient()
{
    close();
}

bool AgentClient::connect(const std::string &host, std::uint16_t port, std::string &error)
{
    close();
    try {
        asio::ip::tcp::resolver resolver(ioContext_);
        const auto endpoints = resolver.resolve(host, std::to_string(port));
        socket_ = std::make_unique<asio::ip::tcp::socket>(ioContext_);
        asio::connect(*socket_, endpoints);
        socket_->set_option(asio::ip::tcp::no_delay(true));
        error.clear();
        return true;
    } catch (const std::exception &exception) {
        error = std::string("Agent connect failed: ") + exception.what();
        close();
        return false;
    }
}

void AgentClient::close()
{
    if (socket_) {
        asio::error_code ignored;
        socket_->shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
        socket_->close(ignored);
        socket_.reset();
    }
    decoder_.reset();
    ioContext_.restart();
}

bool AgentClient::request(const std::string &json, std::string &responseJson, std::string &error)
{
    if (!isConnected()) {
        error = "Agent is not connected";
        return false;
    }
    try {
        const auto encoded = encodeAgentFrame(jsonAgentFrame(json));
        if (encoded.empty()) {
            error = "Agent request is too large";
            return false;
        }
        asio::write(*socket_, asio::buffer(encoded));

        std::array<std::uint8_t, 8192> buffer{};
        while (true) {
            asio::error_code errorCode;
            const std::size_t received = socket_->read_some(asio::buffer(buffer), errorCode);
            if (errorCode) {
                error = std::string("Agent receive failed: ") + errorCode.message();
                return false;
            }
            serialdemo::ByteBuffer chunk(buffer.begin(), buffer.begin()
                                         + static_cast<std::ptrdiff_t>(received));
            std::vector<AgentFrame> frames;
            if (!decoder_.push(chunk, frames, error)) {
                return false;
            }
            for (const auto &frame : frames) {
                if (frame.kind == AgentFrameKind::Json) {
                    responseJson = agentFrameText(frame);
                    error.clear();
                    return true;
                }
            }
        }
    } catch (const std::exception &exception) {
        error = std::string("Agent request failed: ") + exception.what();
        return false;
    }
}

bool AgentClient::isConnected() const
{
    return socket_ && socket_->is_open();
}

} // namespace testtools
