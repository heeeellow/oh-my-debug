#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace testtools {

// 板端 Agent 目标操作系统与 CPU 架构枚举及版本解析辅助函数。
enum class AgentPlatform { Android, Linux };
enum class AgentArchitecture { ArmV7, Arm64 };

struct AgentTarget {
    AgentPlatform platform = AgentPlatform::Android;
    AgentArchitecture architecture = AgentArchitecture::ArmV7;
};

std::optional<AgentArchitecture> parseAndroidAgentAbi(std::string_view value);
std::optional<AgentArchitecture> parseLinuxAgentMachine(std::string_view value);
std::optional<std::string> parseGlibcVersion(std::string_view value);
bool glibcVersionAtLeast(std::string_view value, int requiredMajor, int requiredMinor);
std::string agentPlatformName(AgentPlatform platform);
std::string agentArchitectureName(const AgentTarget& target);
} // namespace testtools
