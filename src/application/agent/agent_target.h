#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace testtools {

enum class AgentPlatform { Android, Linux };
enum class AgentArchitecture { ArmV7, Arm64 };

struct AgentTarget {
    AgentPlatform platform = AgentPlatform::Android;
    AgentArchitecture architecture = AgentArchitecture::ArmV7;
};

std::optional<AgentArchitecture> parseAndroidAgentAbi(std::string_view value);
std::optional<AgentArchitecture> parseLinuxAgentMachine(std::string_view value);
std::string agentPlatformName(AgentPlatform platform);
std::string agentArchitectureName(const AgentTarget& target);
} // namespace testtools
