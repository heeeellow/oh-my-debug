#include "agent_service.h"
#include "core/protocol/agent_version.h"

#include <iostream>
#include <string>

namespace {

bool contains(const std::string &text, const std::string &value)
{
    return text.find(value) != std::string::npos;
}

int fail(const char *name, const std::string &response)
{
    std::cerr << "FAIL " << name << ": " << response << '\n';
    return 1;
}

} // namespace

int main()
{
    testtools::agent::AgentService service;

    const auto hello = service.handleRequest(
        "{\"protocol\":1,\"request_id\":\"hello\","
        "\"method\":\"session.hello\",\"params\":{}}");
    if (!contains(hello, "\"ok\":true")
        || !contains(hello, std::string("\"agent_version\":\"")
                            + testtools::AgentVersion + "\"")) {
        return fail("valid hello", hello);
    }

    const auto missingProtocol = service.handleRequest(
        "{\"request_id\":\"missing\",\"method\":\"session.hello\",\"params\":{}}");
    if (!contains(missingProtocol, "\"code\":\"invalid_request\"")) {
        return fail("missing protocol", missingProtocol);
    }

    const auto missingParams = service.handleRequest(
        "{\"protocol\":1,\"request_id\":\"missing-params\","
        "\"method\":\"session.hello\"}");
    if (!contains(missingParams, "\"code\":\"invalid_request\"")) {
        return fail("missing params", missingParams);
    }

    const auto stringProtocol = service.handleRequest(
        "{\"protocol\":\"1\",\"request_id\":\"typed\","
        "\"method\":\"session.hello\",\"params\":{}}");
    if (!contains(stringProtocol, "\"code\":\"invalid_request\"")) {
        return fail("string protocol", stringProtocol);
    }

    const auto unsupportedProtocol = service.handleRequest(
        "{\"protocol\":2,\"request_id\":\"future\","
        "\"method\":\"session.hello\",\"params\":{}}");
    if (!contains(unsupportedProtocol, "\"code\":\"unsupported_protocol\"")) {
        return fail("unsupported protocol", unsupportedProtocol);
    }

    const auto invalidParity = service.handleRequest(
        "{\"protocol\":1,\"request_id\":\"bad-parity\","
        "\"method\":\"serial.open\",\"params\":{"
        "\"endpoint\":\"/dev/ttyUSB2\",\"parity\":\"mark\"}}");
    if (!contains(invalidParity, "\"code\":\"invalid_params\"")) {
        return fail("invalid parity", invalidParity);
    }

    const auto invalidFraming = service.handleRequest(
        "{\"protocol\":1,\"request_id\":\"bad-framing\","
        "\"method\":\"serial.open\",\"params\":{"
        "\"endpoint\":\"/dev/ttyUSB2\",\"data_bits\":9}}");
    if (!contains(invalidFraming, "\"code\":\"invalid_params\"")) {
        return fail("invalid framing", invalidFraming);
    }

    std::cout << "PASS agent request validation and version contract\n";
    return 0;
}
