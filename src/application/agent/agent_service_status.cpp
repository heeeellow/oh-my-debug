#include "application/agent/agent_service_status.h"
#include "core/protocol/json_value.h"

namespace testtools {
bool validateAgentService(const std::string& hello,
                          const std::string& capabilities,
                          const std::string& requiredVersion,
                          std::string& error)
{
    JsonValue helloDocument, capabilityDocument;
    if (!parseJson(hello, helloDocument, error) ||
        !parseJson(capabilities, capabilityDocument, error))
        return false;
    const auto successful = [](const JsonValue& document) {
        const auto* ok = document.find("ok");
        return ok && ok->isBool() && ok->boolValue();
    };
    if (!successful(helloDocument) || !successful(capabilityDocument)) {
        error = "调试服务拒绝了连接检查，请查看服务状态并重试";
        return false;
    }
    const auto* result = helloDocument.find("result");
    const auto* version = result ? result->find("agent_version") : nullptr;
    if (!version || !version->isString() || version->stringValue() != requiredVersion) {
        error = "调试服务版本不兼容，请重新连接并准备当前版本";
        return false;
    }
    result = capabilityDocument.find("result");
    const auto* values = result ? result->find("capabilities") : nullptr;
    if (values && values->isArray()) {
        for (const auto& capability : values->arrayItems()) {
            const auto* id = capability.find("id");
            if (id && id->isString() && id->stringValue() == "serial.stream") {
                error.clear();
                return true;
            }
        }
    }
    error = "调试服务未提供串口能力，请检查服务版本并重试";
    return false;
}
bool probeAgentService(IAgentConnection& connection,
                       const std::string& requiredVersion,
                       std::string& error)
{
    std::string hello, capabilities;
    return connection.request("{\"protocol\":1,\"request_id\":\"health-hello\",\"method\":"
                              "\"session.hello\",\"params\":{}}",
                              hello,
                              error) &&
           connection.request("{\"protocol\":1,\"request_id\":\"health-capabilities\",\"method\":"
                              "\"capability.list\",\"params\":{}}",
                              capabilities,
                              error) &&
           validateAgentService(hello, capabilities, requiredVersion, error);
}
} // namespace testtools
