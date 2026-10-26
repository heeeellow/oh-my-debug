#pragma once

#include "application/agent/agent_target.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace testtools {

// 随桌面端分发的 Agent Bundle 单文件清单项（含路径、角色、可执行标记与 SHA-256 校验值）。
struct AgentBundleFile {
    std::string path;
    std::string role;
    bool executable = false;
    std::uintmax_t size = 0;
    std::string sha256;
    std::filesystem::path sourcePath;
};

// 面向特定目标平台与架构的 Agent 部署包描述。
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

// 经过 manifest.json Schema v2 与文件完整性校验后的完整 Agent Bundle。
struct AgentBundle {
    std::filesystem::path root;
    std::string agentVersion;
    std::string buildId;
    int protocolMinimum = 0;
    int protocolMaximum = 0;
    std::vector<AgentBundlePackage> packages;
};

// 加载并严格校验 Agent Bundle 目录（包括协议版本、构建版本与全量文件 SHA-256）。
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
