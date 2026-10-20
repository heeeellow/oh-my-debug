#include "application/agent/agent_target.h"

#include <algorithm>
#include <cctype>

namespace testtools {

namespace {

std::optional<std::string> versionAfterMarker(const std::string& text,
                                              std::string_view marker)
{
    auto position = text.find(marker);
    if (position == std::string::npos) {
        return std::nullopt;
    }
    position += marker.size();
    while (position < text.size()) {
        if (std::isdigit(static_cast<unsigned char>(text[position]))) {
            const auto begin = position;
            while (position < text.size() &&
                   std::isdigit(static_cast<unsigned char>(text[position]))) {
                ++position;
            }
            if (position < text.size() && text[position] == '.') {
                ++position;
                const auto minorBegin = position;
                while (position < text.size() &&
                       std::isdigit(static_cast<unsigned char>(text[position]))) {
                    ++position;
                }
                if (position > minorBegin) {
                    return text.substr(begin, position - begin);
                }
            }
        }
        ++position;
    }
    return std::nullopt;
}

} // namespace

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

std::optional<std::string> parseGlibcVersion(std::string_view value)
{
    std::string normalized(value);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    for (const std::string_view marker : {"glibc", "gnu libc", "gnu c library"}) {
        if (const auto version = versionAfterMarker(normalized, marker)) {
            return version;
        }
    }
    return std::nullopt;
}

bool glibcVersionAtLeast(std::string_view value, int requiredMajor, int requiredMinor)
{
    const auto dot = value.find('.');
    if (dot == std::string_view::npos) {
        return false;
    }
    try {
        const int major = std::stoi(std::string(value.substr(0, dot)));
        const int minor = std::stoi(std::string(value.substr(dot + 1)));
        return major > requiredMajor || (major == requiredMajor && minor >= requiredMinor);
    } catch (...) {
        return false;
    }
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

} // namespace testtools
