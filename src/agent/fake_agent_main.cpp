#include "core/agent_protocol.h"

#include <asio/ip/tcp.hpp>
#include <asio/write.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::string jsonStringField(const std::string &json, const std::string &name)
{
    const std::string key = "\"" + name + "\"";
    std::size_t position = json.find(key);
    if (position == std::string::npos) {
        return {};
    }
    position = json.find(':', position + key.size());
    if (position == std::string::npos) {
        return {};
    }
    position = json.find('"', position + 1);
    if (position == std::string::npos) {
        return {};
    }
    const std::size_t end = json.find('"', position + 1);
    return end == std::string::npos ? std::string{} : json.substr(position + 1, end - position - 1);
}

std::string responseFor(const std::string &request)
{
    const std::string requestId = jsonStringField(request, "request_id");
    const std::string method = jsonStringField(request, "method");
    const std::string prefix = "{\"protocol\":1,\"request_id\":\"" + requestId + "\",";
    if (requestId.empty() || method.empty()) {
        return "{\"protocol\":1,\"ok\":false,\"error\":{\"code\":\"invalid_request\","
               "\"message\":\"request_id and method are required\"}}";
    }
    if (method == "session.hello") {
        return prefix
               + "\"ok\":true,\"result\":{\"agent\":\"testtools-agent\",\"agent_version\":\"0.1.0\","
                 "\"protocol_min\":1,\"protocol_max\":1,\"platform\":\"fake-local\"}}";
    }
    if (method == "capability.list") {
        return prefix
               + "\"ok\":true,\"result\":{\"capabilities\":[{\"id\":\"system.info\","
                 "\"name\":\"System information\",\"version\":1,\"risk\":\"read_only\","
                 "\"supports_cancel\":false}]}}";
    }
    return prefix
           + "\"ok\":false,\"error\":{\"code\":\"method_not_found\","
             "\"message\":\"method is not available in the fake agent\"}}";
}

} // namespace

int main(int argc, char **argv)
{
    std::uint16_t port = 27183;
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::string(argv[index]) == "--port") {
            const int parsed = std::stoi(argv[++index]);
            if (parsed < 0 || parsed > 65535) {
                std::cerr << "invalid port\n";
                return 2;
            }
            port = static_cast<std::uint16_t>(parsed);
        }
    }

    try {
        asio::io_context ioContext;
        asio::ip::tcp::acceptor acceptor(ioContext,
            asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), port));
        std::cout << "LISTENING " << acceptor.local_endpoint().port() << std::endl;

        asio::ip::tcp::socket socket(ioContext);
        acceptor.accept(socket);
        testtools::AgentFrameDecoder decoder;
        std::array<std::uint8_t, 8192> buffer{};
        while (true) {
            asio::error_code errorCode;
            const std::size_t received = socket.read_some(asio::buffer(buffer), errorCode);
            if (errorCode == asio::error::eof) {
                break;
            }
            if (errorCode) {
                std::cerr << errorCode.message() << '\n';
                return 3;
            }
            serialdemo::ByteBuffer chunk(buffer.begin(), buffer.begin()
                                         + static_cast<std::ptrdiff_t>(received));
            std::vector<testtools::AgentFrame> frames;
            std::string decodeError;
            if (!decoder.push(chunk, frames, decodeError)) {
                std::cerr << decodeError << '\n';
                return 4;
            }
            for (const auto &frame : frames) {
                if (frame.kind != testtools::AgentFrameKind::Json) {
                    continue;
                }
                const auto encoded = testtools::encodeAgentFrame(
                    testtools::jsonAgentFrame(responseFor(testtools::agentFrameText(frame))));
                asio::write(socket, asio::buffer(encoded));
            }
        }
        return 0;
    } catch (const std::exception &exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
