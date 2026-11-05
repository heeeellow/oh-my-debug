#include "application/agent/agent_connection_service.h"
#include "adapters/device/windows_toolbox_host.h"
#include "application/agent/agent_service_status.h"

#include "adapters/agent/agent_client.h"
#include "core/protocol/json_value.h"
#include <exception>

namespace testtools {

std::shared_ptr<IToolboxHost> AgentConnectionService::createToolboxHost(const std::string& directory)
{
    return std::make_shared<WindowsToolboxHost>(directory);
}

namespace {

std::string agentVersion(const std::string& response)
{
    JsonValue document;
    std::string parseError;
    if (!parseJson(response, document, parseError)) {
        return {};
    }
    const JsonValue* result = document.find("result");
    if (result == nullptr || !result->isObject()) {
        return {};
    }
    const JsonValue* version = result->find("agent_version");
    return version != nullptr && version->isString() ? version->stringValue() : std::string{};
}

bool connectAndDiscover(AgentConnectionResult& result,
                        const AgentConnectionService::ProgressCallback& progress)
{
    if (!result.deployment.succeeded) {
        result.error = result.deployment.error;
        return false;
    }

    result.error.clear();
    result.hello.clear();
    result.capabilities.clear();
    result.systemInfo.clear();
    result.observedVersion.clear();
    if (progress)
        progress("正在校验服务连接…");
    result.client = AgentConnectionService::createClient();
    if (!result.client->connect("127.0.0.1", result.deployment.localPort, result.error) ||
        !result.client->request("{\"protocol\":1,\"request_id\":\"ui-hello\","
                                "\"method\":\"session.hello\",\"params\":{}}",
                                result.hello,
                                result.error)) {
        return false;
    }

    result.observedVersion = agentVersion(result.hello);
    if (result.observedVersion != AdbAgentDeployment::RequiredAgentVersion) {
        result.error =
            std::string("设备服务版本不匹配，需要 ") + AdbAgentDeployment::RequiredAgentVersion;
        return false;
    }

    if (progress)
        progress("正在读取服务能力…");
    if (!(result.client->request("{\"protocol\":1,\"request_id\":\"ui-capabilities\","
                                  "\"method\":\"capability.list\",\"params\":{}}",
                                  result.capabilities,
                                  result.error) &&
           validateAgentService(result.hello,
                                result.capabilities,
                                AdbAgentDeployment::RequiredAgentVersion,
                                result.error)))
        return false;
    if (progress)
        progress("正在读取设备信息…");
    return result.client->request("{\"protocol\":1,\"request_id\":\"ui-system-info\","
                                  "\"method\":\"system.info\",\"params\":{}}",
                                  result.systemInfo,
                                  result.error);
}

void closeAndRemoveForward(const std::string& serial,
                           AdbAgentDeployment& deployment,
                           AgentConnectionResult& result)
{
    if (result.client) {
        result.client->close();
    }
    if (result.deployment.localPort != 0) {
        deployment.removeForward(serial, result.deployment.localPort);
        result.deployment.localPort = 0;
    }
}

} // namespace

std::shared_ptr<IAgentConnection> AgentConnectionService::createClient()
{
    return std::make_shared<AgentClient>();
}

bool AgentConnectionService::checkConnection(std::uint16_t localPort, std::string& error)
{
    auto client = std::make_shared<AgentClient>(std::chrono::milliseconds(1500));
    const bool ready = client->connect("127.0.0.1", localPort, error) &&
                       probeAgentService(*client, AdbAgentDeployment::RequiredAgentVersion, error);
    client->close();
    return ready;
}

AgentConnectionResult AgentConnectionService::connectDevice(const std::string& serial,
                                                            const std::string& applicationDirectory,
                                                            bool allowDeploy,
                                                            const ProgressCallback& progress)
{
    AdbAgentDeployment deployment;
    AgentConnectionResult result;
    result.client = createClient();
    try {
        result.deployment = deployment.reconnect(serial, progress);
        result.succeeded = connectAndDiscover(result, progress);

        if (!result.succeeded && allowDeploy) {
            closeAndRemoveForward(serial, deployment, result);
            if (progress)
                progress("设备服务尚未就绪，正在准备当前版本...");
            result.deployment = deployment.deployAndForward(serial, applicationDirectory, progress);
            result.succeeded = connectAndDiscover(result, progress);
        }
    } catch (const std::exception& error) {
        result.succeeded = false;
        result.error = error.what();
    }

    if (!result.succeeded) {
        closeAndRemoveForward(serial, deployment, result);
    }
    return result;
}

} // namespace testtools
