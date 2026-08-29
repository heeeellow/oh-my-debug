#include "agent_service.h"
#include "core/agent_protocol.h"

#include <asio/ip/tcp.hpp>
#include <asio/write.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

void serveClient(asio::ip::tcp::socket socket,
                 const std::shared_ptr<testtools::agent::AgentService> &service)
{
    testtools::AgentFrameDecoder decoder;
    std::array<std::uint8_t, 8192> buffer{};
    while (true) {
        asio::error_code errorCode;
        const std::size_t received = socket.read_some(asio::buffer(buffer), errorCode);
        if (errorCode == asio::error::eof) {
            return;
        }
        if (errorCode) {
            return;
        }

        serialdemo::ByteBuffer chunk(buffer.begin(), buffer.begin()
                                     + static_cast<std::ptrdiff_t>(received));
        std::vector<testtools::AgentFrame> frames;
        std::string decodeError;
        if (!decoder.push(chunk, frames, decodeError)) {
            return;
        }
        for (const auto &frame : frames) {
            if (frame.kind != testtools::AgentFrameKind::Json) {
                continue;
            }
            const auto response = service->handleRequest(testtools::agentFrameText(frame));
            const auto encoded = testtools::encodeAgentFrame(testtools::jsonAgentFrame(response));
            asio::write(socket, asio::buffer(encoded), errorCode);
            if (errorCode) {
                return;
            }
        }
    }
}

} // namespace

int main(int argc, char **argv)
{
    std::uint16_t port = 27183;
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::string(argv[index]) == "--port") {
            const int parsed = std::stoi(argv[++index]);
            if (parsed <= 0 || parsed > 65535) {
                std::cerr << "invalid port\n";
                return 2;
            }
            port = static_cast<std::uint16_t>(parsed);
        }
    }

    auto service = std::make_shared<testtools::agent::AgentService>();
    try {
        asio::io_context ioContext;
        asio::ip::tcp::acceptor acceptor(ioContext,
            asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), port));
        acceptor.set_option(asio::socket_base::reuse_address(true));
        std::cout << "LISTENING " << acceptor.local_endpoint().port() << std::endl;

        while (true) {
            asio::ip::tcp::socket socket(ioContext);
            asio::error_code errorCode;
            acceptor.accept(socket, errorCode);
            if (errorCode) {
                std::cerr << errorCode.message() << '\n';
                break;
            }
            std::thread(serveClient, std::move(socket), service).detach();
        }
    } catch (const std::exception &exception) {
        std::cerr << exception.what() << '\n';
        service->stop();
        return 1;
    }
    service->stop();
    return 0;
}
