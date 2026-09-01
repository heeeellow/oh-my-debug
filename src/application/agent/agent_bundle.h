#pragma once

#include "application/agent/agent_target.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace testtools {

struct AgentBundleFile {
    std::string path;
    std::string role;
    bool executable = false;
    std::uintmax_t size = 0;
    std::string sha256;
    std::filesystem::path sourcePath;
};

struct AgentBundlePackage {
    std::string id;
    AgentPlatform platform = AgentPlatform::Android;
    AgentArchitecture architecture = AgentArchitecture::ArmV7;
    std::string abi;
    std::string libc;
    std::string libcMinimumVersion;
    std::string root;
    std::vector<AgentBundleFile> files;

    const AgentBundleFile* agentFile() const;
};

struct AgentBundle {
    std::filesystem::path root;
    std::string agentVersion;
    std::string buildId;
    int protocolMinimum = 0;
    int protocolMaximum = 0;
    std::vector<AgentBundlePackage> packages;
};

std::optional<AgentBundle> loadAgentBundle(const std::filesystem::path& bundleRoot,
                                           std::string& error);
std::optional<AgentBundle>
loadAgentBundleFromApplication(const std::filesystem::path& applicationDirectory,
                               std::string& error);
const AgentBundlePackage* selectAndroidAgentPackage(const AgentBundle& bundle,
                                                    std::string_view abi);
const AgentBundlePackage* selectLinuxAgentPackage(const AgentBundle& bundle,
                                                  std::string_view machine,
                                                  std::string_view libcVersion);

} // namespace testtools
