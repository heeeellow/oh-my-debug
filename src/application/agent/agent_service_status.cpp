#include "application/agent/agent_service_status.h"
#include "core/protocol/json_value.h"

#include <algorithm>
#include <cctype>
#include <limits>

namespace testtools {
namespace {

std::string trimAsciiWhitespace(std::string value)
{
    const auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](char ch) {
                    return !isSpace(static_cast<unsigned char>(ch));
                }));
    value.erase(std::find_if(value.rbegin(),
                             value.rend(),
                             [&](char ch) { return !isSpace(static_cast<unsigned char>(ch)); })
                    .base(),
                value.end());
    return value;
}

const JsonValue* resultObject(const JsonValue& document)
{
    if (!document.isObject()) {
        return nullptr;
    }
    const auto* result = document.find("result");
    return (result && result->isObject()) ? result : nullptr;
}

std::string trimmedStringField(const JsonValue& object, const char* key)
{
    const auto* value = object.find(key);
    if (!value || !value->isString()) {
        return {};
    }
    return trimAsciiWhitespace(value->stringValue());
}

int intField(const JsonValue& object, const char* key, int fallback)
{
    const auto* value = object.find(key);
    if (!value || !value->isInt64() || value->int64Value() < std::numeric_limits<int>::min() ||
        value->int64Value() > std::numeric_limits<int>::max()) {
        return fallback;
    }
    return static_cast<int>(value->int64Value());
}

std::int64_t int64Field(const JsonValue& object, const char* key, std::int64_t fallback = 0)
{
    const auto* value = object.find(key);
    return (value && value->isInt64()) ? value->int64Value() : fallback;
}

} // namespace

bool AgentServiceSnapshot::hasCapability(std::string_view capabilityId) const
{
    return std::find(capabilityIds.begin(), capabilityIds.end(), capabilityId) !=
           capabilityIds.end();
}

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

AgentServiceSnapshot parseAgentServiceSnapshot(const std::string& hello,
                                               const std::string& capabilities,
                                               const std::string& systemInfo)
{
    AgentServiceSnapshot snapshot;
    std::string ignoreError;

    JsonValue helloDoc;
    if (parseJson(hello, helloDoc, ignoreError)) {
        if (const auto* result = resultObject(helloDoc)) {
            snapshot.agentVersion = trimmedStringField(*result, "agent_version");
        }
    }

    JsonValue capabilitiesDoc;
    if (parseJson(capabilities, capabilitiesDoc, ignoreError)) {
        if (const auto* result = resultObject(capabilitiesDoc)) {
            const auto* items = result->find("capabilities");
            if (items && items->isArray()) {
                for (const auto& item : items->arrayItems()) {
                    const std::string id = trimmedStringField(item, "id");
                    if (!id.empty() && !snapshot.hasCapability(id)) {
                        snapshot.capabilityIds.push_back(id);
                    }
                }
            }
        }
    }

    JsonValue systemInfoDoc;
    if (parseJson(systemInfo, systemInfoDoc, ignoreError)) {
        if (const auto* result = resultObject(systemInfoDoc)) {
            snapshot.systemInfo.model = trimmedStringField(*result, "model");
            snapshot.systemInfo.manufacturer = trimmedStringField(*result, "manufacturer");
            snapshot.systemInfo.brand = trimmedStringField(*result, "brand");
            snapshot.systemInfo.product = trimmedStringField(*result, "product");
            snapshot.systemInfo.board = trimmedStringField(*result, "board");
            snapshot.systemInfo.hardware = trimmedStringField(*result, "hardware");
            snapshot.systemInfo.architecture = trimmedStringField(*result, "architecture");
            snapshot.systemInfo.cpuCores = intField(*result, "cpu_cores", 0);
            snapshot.systemInfo.memoryTotalKib = int64Field(*result, "memory_total_kib", 0);
            snapshot.systemInfo.osRelease = trimmedStringField(*result, "os_release");
            snapshot.systemInfo.androidSdk = trimmedStringField(*result, "android_sdk");
            snapshot.systemInfo.kernel = trimmedStringField(*result, "kernel");
            snapshot.systemInfo.buildId = trimmedStringField(*result, "build_id");
            snapshot.systemInfo.effectiveUid = intField(*result, "effective_uid", -1);
        }
    }

    return snapshot;
}

} // namespace testtools
