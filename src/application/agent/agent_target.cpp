#include "application/agent/agent_target.h"

namespace testtools {

std::optional<AgentArchitecture> parseAndroidAgentAbi(std::string_view value)
{
    if (value == "armeabi-v7a") {
        return AgentArchitecture::ArmV7;
    }
    if (value == "arm64-v8a") {
        return AgentArchitecture::Arm64;
    }
    return std::nullopt;
}

std::optional<AgentArchitecture> parseLinuxAgentMachine(std::string_view value)
{
    if (value == "armv7" || value == "armv7l" || value == "armhf") {
        return AgentArchitecture::ArmV7;
    }
    if (value == "aarch64" || value == "arm64") {
        return AgentArchitecture::Arm64;
    }
    return std::nullopt;
}

std::string agentPlatformName(AgentPlatform platform)
{
    return platform == AgentPlatform::Android ? "android" : "linux";
}

std::string agentArchitectureName(const AgentTarget& target)
{
    if (target.platform == AgentPlatform::Android) {
        return target.architecture == AgentArchitecture::Arm64 ? "arm64-v8a" : "armeabi-v7a";
    }
    return target.architecture == AgentArchitecture::Arm64 ? "aarch64" : "armv7";
}

std::filesystem::path agentArtifactPath(const std::filesystem::path& applicationDirectory,
                                        const AgentTarget& target)
{
    return applicationDirectory / "agents" / agentPlatformName(target.platform) /
           agentArchitectureName(target) / "testtools-agent";
}

} // namespace testtools
