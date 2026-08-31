#include "application/agent/linux_agent_deployment.h"

#include "adapters/adb/adb_transport.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#endif

namespace testtools {

namespace {

std::string trim(std::string value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    const auto first = std::find_if_not(
        value.begin(), value.end(), [](unsigned char ch) { return std::isspace(ch); });
    value.erase(value.begin(), first);
    return value;
}

bool validSerial(const std::string& serial)
{
    static const std::regex pattern(R"(^[A-Za-z0-9._:-]+$)");
    return std::regex_match(serial, pattern);
}

bool validComponent(const std::string& value)
{
    static const std::regex pattern(R"(^[A-Za-z0-9._-]+$)");
    return std::regex_match(value, pattern);
}

std::string commandDetail(const serialdemo::CommandResult& result)
{
    if (result.timedOut) {
        return "command timed out";
    }
    const std::string detail =
        trim(result.standardError.empty() ? result.standardOutput : result.standardError);
    return detail.empty() ? "exit code " + std::to_string(result.exitCode) : detail;
}

std::string compactJson(std::string value)
{
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char ch) {
                    return std::isspace(ch) != 0;
                }),
                value.end());
    return value;
}

std::optional<std::string> jsonStringAfter(const std::string& json,
                                           std::size_t offset,
                                           const std::string& key)
{
    const std::size_t start = json.find(key, offset);
    if (start == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t valueStart = start + key.size();
    const std::size_t end = json.find('"', valueStart);
    if (end == std::string::npos) {
        return std::nullopt;
    }
    return json.substr(valueStart, end - valueStart);
}

std::optional<std::uintmax_t> jsonNumberAfter(const std::string& json,
                                               std::size_t offset,
                                               const std::string& key)
{
    const std::size_t start = json.find(key, offset);
    if (start == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t valueStart = start + key.size();
    const std::size_t end = json.find_first_not_of("0123456789", valueStart);
    try {
        return std::stoull(json.substr(valueStart, end - valueStart));
    } catch (...) {
        return std::nullopt;
    }
}

#ifdef _WIN32
std::optional<std::string> sha256File(const std::filesystem::path& path, std::string& error)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "无法读取 Agent 文件: " + path.string();
        return std::nullopt;
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD hashLength = 0;
    DWORD resultLength = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0 ||
        BCryptGetProperty(algorithm,
                          BCRYPT_HASH_LENGTH,
                          reinterpret_cast<PUCHAR>(&hashLength),
                          sizeof(hashLength),
                          &resultLength,
                          0) != 0 ||
        BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) != 0) {
        if (hash) {
            BCryptDestroyHash(hash);
        }
        if (algorithm) {
            BCryptCloseAlgorithmProvider(algorithm, 0);
        }
        error = "Windows SHA-256 初始化失败";
        return std::nullopt;
    }

    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0 && BCryptHashData(hash,
                                        reinterpret_cast<PUCHAR>(buffer.data()),
                                        static_cast<ULONG>(count),
                                        0) != 0) {
            BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(algorithm, 0);
            error = "计算 Agent SHA-256 失败";
            return std::nullopt;
        }
    }
    if (input.bad()) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        error = "读取 Agent 文件失败: " + path.string();
        return std::nullopt;
    }

    std::vector<unsigned char> digest(hashLength);
    if (BCryptFinishHash(hash, digest.data(), hashLength, 0) != 0) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        error = "完成 Agent SHA-256 计算失败";
        return std::nullopt;
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const unsigned char byte : digest) {
        output << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return output.str();
}
#else
std::optional<std::string> sha256File(const std::filesystem::path&, std::string& error)
{
    error = "Linux ADB 部署暂未提供 Windows 主机 SHA-256 实现";
    return std::nullopt;
}
#endif

struct LinuxArtifact {
    std::filesystem::path bundleRoot;
    std::filesystem::path binary;
    std::string agentVersion;
    std::string buildId;
    std::string sha256;
    std::uintmax_t size = 0;
};

std::optional<LinuxArtifact> loadArtifact(const std::string& applicationDirectory,
                                           std::string& error)
{
    const std::filesystem::path application(applicationDirectory);
    const std::filesystem::path bundle = application / "agent-bundle";
    const std::filesystem::path manifestPath = bundle / "manifest.json";
    std::ifstream manifestFile(manifestPath, std::ios::binary);
    if (!manifestFile) {
        error = "未找到 Linux Agent Bundle: " + manifestPath.string();
        return std::nullopt;
    }
    std::string manifest((std::istreambuf_iterator<char>(manifestFile)),
                         std::istreambuf_iterator<char>());
    manifest = compactJson(std::move(manifest));

    const auto schema = jsonNumberAfter(manifest, 0, "\"schema_version\":");
    const auto agentVersion = jsonStringAfter(manifest, 0, "\"agent_version\":\"");
    const auto topBuild = manifest.find("\"build\":{");
    const auto buildId = jsonStringAfter(manifest, topBuild, "\"build_id\":\"");
    const auto artifact = manifest.find("\"platform\":\"linux\",\"architecture\":\"aarch64\"");
    if (!schema || *schema != 1 || !agentVersion || !buildId || artifact == std::string::npos) {
        error = "Linux Agent manifest 缺少有效的 schema、版本、构建标识或 aarch64 产物";
        return std::nullopt;
    }
    const auto path = jsonStringAfter(manifest, artifact, "\"path\":\"");
    const auto size = jsonNumberAfter(manifest, artifact, "\"size\":");
    const auto sha256 = jsonStringAfter(manifest, artifact, "\"sha256\":\"");
    const auto artifactBuildId = jsonStringAfter(manifest, artifact, "\"build_id\":\"");
    if (!path || !size || !sha256 || !artifactBuildId || *artifactBuildId != *buildId ||
        *path != "agents/linux/aarch64/glibc-2.31/testtools-agent" ||
        !std::regex_match(*sha256, std::regex(R"(^[0-9a-fA-F]{64}$)"))) {
        error = "Linux Agent manifest 的 aarch64/glibc-2.31 产物字段无效";
        return std::nullopt;
    }
    if (!validComponent(*agentVersion) || !validComponent(*buildId)) {
        error = "Linux Agent manifest 的版本或构建标识包含非法路径字符";
        return std::nullopt;
    }

    LinuxArtifact result;
    result.bundleRoot = bundle;
    result.binary = bundle / std::filesystem::path(*path);
    result.agentVersion = *agentVersion;
    result.buildId = *buildId;
    result.sha256 = *sha256;
    result.size = *size;
    std::error_code fileError;
    if (!std::filesystem::is_regular_file(result.binary, fileError)) {
        error = "Linux Agent Bundle 缺少产物: " + result.binary.string();
        return std::nullopt;
    }
    if (std::filesystem::file_size(result.binary, fileError) != result.size || fileError) {
        error = "Linux Agent 文件大小与 manifest 不一致";
        return std::nullopt;
    }
    const auto actualSha256 = sha256File(result.binary, error);
    if (!actualSha256 || actualSha256->size() != result.sha256.size() ||
        !std::equal(actualSha256->begin(), actualSha256->end(), result.sha256.begin(),
                                     [](char lhs, char rhs) {
                                         return std::tolower(static_cast<unsigned char>(lhs)) ==
                                                std::tolower(static_cast<unsigned char>(rhs));
                                     })) {
        if (error.empty()) {
            error = "Linux Agent SHA-256 与 manifest 不一致";
        }
        return std::nullopt;
    }
    return result;
}

bool versionAtLeast(std::string value, int requiredMajor, int requiredMinor)
{
    const auto dot = value.find('.');
    if (dot == std::string::npos) {
        return false;
    }
    try {
        const int major = std::stoi(value.substr(0, dot));
        const int minor = std::stoi(value.substr(dot + 1));
        return major > requiredMajor || (major == requiredMajor && minor >= requiredMinor);
    } catch (...) {
        return false;
    }
}

std::optional<std::string> detectGlibcVersion(const std::string& text)
{
    static const std::regex pattern(R"((?:GLIBC|GNU libc)[^0-9]*([0-9]+\.[0-9]+))",
                                    std::regex::icase);
    std::smatch match;
    if (!std::regex_search(text, match, pattern)) {
        return std::nullopt;
    }
    return match[1].str();
}

LinuxAgentDeploymentResult createForward(serialdemo::AdbTransport& adb,
                                          const std::string& serial,
                                          const std::string& binaryPath,
                                          const std::string& buildId,
                                          const LinuxAgentDeployment::ProgressCallback& progress)
{
    LinuxAgentDeploymentResult result;
    result.architecture = "aarch64";
    result.libc = "glibc-2.31";
    result.buildId = buildId;
    result.binaryPath = binaryPath;
    result.remotePath = std::string(LinuxAgentDeployment::RemoteCurrent) + "/testtools-agent";
    if (progress) {
        progress("正在建立 Linux Agent ADB 控制通道...");
    }
    const auto forward = adb.runCommand(
        serial,
        {"forward", "tcp:0", "tcp:" + std::to_string(LinuxAgentDeployment::ControlPort)},
        std::chrono::seconds(5));
    if (forward.exitCode != 0 || forward.timedOut) {
        result.error = "建立 Linux Agent ADB 转发失败: " + commandDetail(forward);
        return result;
    }
    try {
        const int port = std::stoi(trim(forward.standardOutput));
        if (port <= 0 || port > 65535) {
            throw std::out_of_range("port");
        }
        result.localPort = static_cast<std::uint16_t>(port);
    } catch (...) {
        result.error = "ADB 未返回有效的 Linux Agent 本地端口";
        return result;
    }
    result.succeeded = true;
    return result;
}

} // namespace

LinuxAgentDeploymentResult LinuxAgentDeployment::deployAndForward(
    const std::string& serial,
    const std::string& applicationDirectory,
    const ProgressCallback& progress) const
{
    LinuxAgentDeploymentResult result;
    if (!validSerial(serial)) {
        result.error = "ADB 设备序列号格式不合法";
        return result;
    }
    std::string error;
    const auto artifact = loadArtifact(applicationDirectory, error);
    if (!artifact) {
        result.error = error;
        return result;
    }
    result.architecture = "aarch64";
    result.libc = "glibc-2.31";
    result.buildId = artifact->buildId;
    result.binaryPath = artifact->binary.string();

    serialdemo::AdbTransport adb;
    if (progress) {
        progress("正在识别 Linux 板端架构与 glibc...");
    }
    const auto machine = adb.runCommand(serial, {"shell", "uname -m"}, std::chrono::seconds(4));
    if (machine.exitCode != 0 || machine.timedOut || trim(machine.standardOutput) != "aarch64") {
        result.error = "Linux 板端不是受支持的 aarch64: " + commandDetail(machine);
        return result;
    }
    const auto glibc = adb.runCommand(
        serial, {"shell", "ldd --version 2>&1 | head -1"}, std::chrono::seconds(4));
    const auto glibcVersion = detectGlibcVersion(glibc.standardOutput + glibc.standardError);
    if (glibc.exitCode != 0 || glibc.timedOut || !glibcVersion ||
        !versionAtLeast(*glibcVersion, 2, 31)) {
        result.error = "Linux 板端 glibc 版本不足或无法识别: " +
                       trim(glibc.standardOutput + glibc.standardError);
        return result;
    }
    const auto uid = adb.runCommand(serial, {"shell", "id -u"}, std::chrono::seconds(4));
    if (uid.exitCode != 0 || uid.timedOut || trim(uid.standardOutput) != "0") {
        result.error = "Linux ADB 自动部署需要 root adbd，以写入 /opt、/run 和 /var/log";
        return result;
    }
    const auto hasSha256sum = adb.runCommand(
        serial, {"shell", "command -v sha256sum >/dev/null 2>&1"}, std::chrono::seconds(3));
    if (hasSha256sum.exitCode != 0 || hasSha256sum.timedOut) {
        result.error = "Linux 板端缺少 sha256sum，无法执行安全部署";
        return result;
    }

    const std::string versionDirectory = std::string(RemoteVersions) + "/" +
                                         artifact->agentVersion + "-" + artifact->buildId;
    const std::string temporary = std::string(RemoteTemporaryRoot) + "/testtools-agent-" +
                                  artifact->buildId + ".new";
    result.remotePath = versionDirectory + "/testtools-agent";
    if (progress) {
        progress("正在准备 Linux Agent 版本目录...");
    }
    const auto prepare = adb.runCommand(
        serial,
        {"shell",
         "mkdir -p " + std::string(RemoteVersions) + " " + RemoteTemporaryRoot +
             " /run/testtools-agent /var/log/testtools-agent && rm -f " + temporary},
        std::chrono::seconds(5));
    if (prepare.exitCode != 0 || prepare.timedOut) {
        result.error = "准备 Linux Agent 目录失败: " + commandDetail(prepare);
        return result;
    }
    if (progress) {
        progress("正在推送并校验 Linux Agent...");
    }
    const auto push = adb.runCommand(serial, {"push", result.binaryPath, temporary},
                                     std::chrono::seconds(30));
    if (push.exitCode != 0 || push.timedOut) {
        result.error = "推送 Linux Agent 失败: " + commandDetail(push);
        return result;
    }
    const auto remoteHash = adb.runCommand(
        serial, {"shell", "sha256sum " + temporary}, std::chrono::seconds(5));
    const std::string remoteHashOutput = trim(remoteHash.standardOutput);
    const std::size_t remoteHashEnd = remoteHashOutput.find_first_of(" \t\r\n");
    const std::string remoteHashValue = remoteHashOutput.substr(0, remoteHashEnd);
    if (remoteHash.exitCode != 0 || remoteHash.timedOut || remoteHashValue.empty() ||
        remoteHashValue.size() != artifact->sha256.size() ||
        !std::equal(remoteHashValue.begin(), remoteHashValue.end(), artifact->sha256.begin(),
                    [](char lhs, char rhs) {
                        return std::tolower(static_cast<unsigned char>(lhs)) ==
                               std::tolower(static_cast<unsigned char>(rhs));
                    })) {
        adb.runCommand(serial, {"shell", "rm -f " + temporary}, std::chrono::seconds(3));
        result.error = "Linux Agent 远端 SHA-256 校验失败";
        return result;
    }

    if (progress) {
        progress("正在切换 Linux Agent 当前版本...");
    }
    const std::string stop = "if [ -r " + std::string(RemotePid) +
                             " ]; then kill \"$(cat " + RemotePid +
                             ")\" 2>/dev/null || true; fi; rm -f " + RemotePid;
    adb.runCommand(serial, {"shell", stop}, std::chrono::seconds(4));
    const std::string install = "mkdir -p " + versionDirectory + " && chmod 755 " + temporary +
                                " && mv -f " + temporary + " " + versionDirectory +
                                "/testtools-agent && rm -f " + std::string(RemotePrevious) +
                                " && if [ -L " + RemoteCurrent + " ] || [ -e " + RemoteCurrent +
                                " ]; then mv " + RemoteCurrent + " " + RemotePrevious +
                                "; fi && ln -s " + versionDirectory + " " + RemoteCurrent;
    const auto installResult = adb.runCommand(serial, {"shell", install}, std::chrono::seconds(8));
    if (installResult.exitCode != 0 || installResult.timedOut) {
        result.error = "安装 Linux Agent 版本失败: " + commandDetail(installResult);
        return result;
    }

    if (progress) {
        progress("正在启动 Linux Agent...");
    }
    const std::string launch = ": >" + std::string(RemoteLog) +
                               "; if command -v start-stop-daemon >/dev/null 2>&1; then "
                               "start-stop-daemon --start --background --make-pidfile --pidfile " +
                               std::string(RemotePid) + " --startas /bin/sh -- -c \"exec " +
                               RemoteCurrent + "/testtools-agent --port 27183 >>" + RemoteLog +
                               " 2>&1\"; else setsid -f sh -c 'exec " + RemoteCurrent +
                               "/testtools-agent --port 27183 >>" + RemoteLog +
                               " 2>&1' </dev/null >/dev/null 2>&1; for attempt in 1 2 3 4 5; do "
                               "pid=$(pidof testtools-agent 2>/dev/null | cut -d ' ' -f1); if [ -n \"$pid\" ]; then "
                               "echo \"$pid\" >" + RemotePid + "; break; fi; sleep 1; done; fi";
    const auto launchResult = adb.runCommand(serial, {"shell", launch}, std::chrono::seconds(5));
    if (launchResult.exitCode != 0 || launchResult.timedOut) {
        result.error = "启动 Linux Agent 失败: " + commandDetail(launchResult);
        return result;
    }
    bool running = false;
    for (int attempt = 0; attempt < 12; ++attempt) {
        const auto probe = adb.runCommand(
            serial, {"shell", "cat " + std::string(RemoteLog) + " 2>/dev/null"},
            std::chrono::seconds(2));
        if (probe.standardOutput.find("LISTENING 27183") != std::string::npos) {
            running = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    if (!running) {
        const std::string rollback =
            "if [ -r " + std::string(RemotePid) + " ]; then kill \"$(cat " + RemotePid +
            ")\" 2>/dev/null || true; fi; rm -f " + RemotePid + " " + RemoteCurrent +
            "; if [ -L " + RemotePrevious + " ] || [ -e " + RemotePrevious +
            " ]; then mv " + RemotePrevious + " " + RemoteCurrent + "; fi";
        adb.runCommand(serial, {"shell", rollback}, std::chrono::seconds(5));
        const auto log = adb.runCommand(
            serial, {"shell", "cat " + std::string(RemoteLog) + " 2>/dev/null"},
            std::chrono::seconds(3));
        result.error = "Linux Agent 未正常监听: " +
                       trim(log.standardOutput + log.standardError);
        return result;
    }
    return createForward(adb, serial, result.binaryPath, result.buildId, progress);
}

LinuxAgentDeploymentResult LinuxAgentDeployment::reconnect(
    const std::string& serial, const ProgressCallback& progress) const
{
    LinuxAgentDeploymentResult result;
    if (!validSerial(serial)) {
        result.error = "ADB 设备序列号格式不合法";
        return result;
    }
    serialdemo::AdbTransport adb;
    if (progress) {
        progress("正在探测已运行的 Linux Agent...");
    }
    const std::string probeCommand =
        "if [ -L " + std::string(RemoteCurrent) + " ] && [ -x " + RemoteCurrent +
        "/testtools-agent ] && [ -r " + RemotePid + " ]; then pid=$(cat " + RemotePid +
        "); if kill -0 \"$pid\" 2>/dev/null && grep -q testtools-agent \"/proc/$pid/cmdline\" 2>/dev/null; then echo RUNNING; fi; fi";
    const auto probe = adb.runCommand(serial, {"shell", probeCommand}, std::chrono::seconds(4));
    if (probe.exitCode != 0 || probe.timedOut || probe.standardOutput.find("RUNNING") == std::string::npos) {
        result.error = "Linux 板端 Agent 未运行，请重新部署";
        return result;
    }
    return createForward(adb, serial, std::string(RemoteCurrent) + "/testtools-agent", {}, progress);
}

void LinuxAgentDeployment::removeForward(const std::string& serial, std::uint16_t localPort) const
{
    if (!validSerial(serial) || localPort == 0) {
        return;
    }
    serialdemo::AdbTransport adb;
    adb.runCommand(serial,
                   {"forward", "--remove", "tcp:" + std::to_string(localPort)},
                   std::chrono::seconds(3));
}

void LinuxAgentDeployment::removeControlForwards(const std::string& serial) const
{
    if (!validSerial(serial)) {
        return;
    }
    serialdemo::AdbTransport adb;
    const auto list = adb.runCommand({}, {"forward", "--list"}, std::chrono::seconds(3));
    if (list.exitCode != 0 || list.timedOut) {
        return;
    }
    std::istringstream lines(list.standardOutput);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        std::string owner;
        std::string local;
        std::string remote;
        fields >> owner >> local >> remote;
        if (owner == serial && remote == "tcp:" + std::to_string(ControlPort) &&
            local.rfind("tcp:", 0) == 0) {
            adb.runCommand(serial, {"forward", "--remove", local}, std::chrono::seconds(3));
        }
    }
}

bool LinuxAgentDeployment::stopRemote(const std::string& serial, std::string& error) const
{
    if (!validSerial(serial)) {
        error = "ADB 设备序列号格式不合法";
        return false;
    }
    serialdemo::AdbTransport adb;
    const std::string command = "if [ -r " + std::string(RemotePid) +
                                " ]; then kill \"$(cat " + RemotePid +
                                ")\" 2>/dev/null || true; fi; rm -f " + RemotePid;
    const auto result = adb.runCommand(serial, {"shell", command}, std::chrono::seconds(4));
    if (result.exitCode != 0 || result.timedOut) {
        error = "停止 Linux Agent 失败: " + commandDetail(result);
        return false;
    }
    error.clear();
    return true;
}

} // namespace testtools
