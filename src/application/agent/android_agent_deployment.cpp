#include "application/agent/android_agent_deployment.h"

#include "application/agent/agent_bundle.h"
#include "application/agent/agent_target.h"

#include "adapters/adb/adb_transport.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <thread>

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

std::string commandDetail(const serialdemo::CommandResult& result)
{
    if (result.timedOut) {
        return "command timed out";
    }
    const std::string detail =
        trim(result.standardError.empty() ? result.standardOutput : result.standardError);
    return detail.empty() ? "exit code " + std::to_string(result.exitCode) : detail;
}

bool ensureRootAdb(serialdemo::AdbTransport& adb,
                   const std::string& serial,
                   const AndroidAgentDeployment::ProgressCallback& progress,
                   std::string& error)
{
    const auto readUid = [&] {
        return adb.runCommand(serial, {"shell", "id -u"}, std::chrono::seconds(4));
    };
    auto uid = readUid();
    if (uid.exitCode == 0 && !uid.timedOut && trim(uid.standardOutput) == "0") {
        return true;
    }

    if (progress) {
        progress("正在请求 root ADB shell...");
    }
    const auto root = adb.runCommand(serial, {"root"}, std::chrono::seconds(8));
    if (root.exitCode != 0 || root.timedOut) {
        error = "Android Agent 需要 root shell 访问板端串口；ADB root 失败: " + commandDetail(root);
        return false;
    }

    const auto wait = adb.runCommand(serial, {"wait-for-device"}, std::chrono::seconds(12));
    if (wait.exitCode != 0 || wait.timedOut) {
        error = "ADB root 后等待设备重新上线失败: " + commandDetail(wait);
        return false;
    }

    uid = readUid();
    if (uid.exitCode != 0 || uid.timedOut || trim(uid.standardOutput) != "0") {
        const std::string actualUid = trim(uid.standardOutput);
        error = "ADB root 后 shell 仍不是 root";
        if (!actualUid.empty()) {
            error += "，当前 UID: " + actualUid;
        }
        return false;
    }
    return true;
}

std::string rootAgentProbe(const std::string& pidFile)
{
    return "if [ -r " + pidFile + " ]; then pid=$(cat " + pidFile +
           "); if [ -d /proc/$pid ] && grep -q testtools-agent "
           "/proc/$pid/cmdline 2>/dev/null; then "
           "uid=$(sed -n 's/^Uid:[[:space:]]*\\([0-9]*\\).*/\\1/p' "
           "/proc/$pid/status 2>/dev/null); "
           "if [ \"$uid\" = \"0\" ]; then echo RUNNING_ROOT; "
           "else echo RUNNING_UID=$uid; fi; fi; fi";
}

std::string normalizeAndroidAbi(std::string abi)
{
    abi = trim(std::move(abi));
    const auto architecture = parseAndroidAgentAbi(abi);
    if (!architecture) {
        return {};
    }
    return agentArchitectureName({AgentPlatform::Android, *architecture});
}

AndroidAgentDeploymentResult createForward(serialdemo::AdbTransport& adb,
                                           const std::string& serial,
                                           const std::string& abi,
                                           const std::string& binaryPath,
                                           const AndroidAgentDeployment::ProgressCallback& progress)
{
    AndroidAgentDeploymentResult result;
    result.abi = abi;
    result.binaryPath = binaryPath;
    result.remotePath = AndroidAgentDeployment::RemoteBinary;
    if (progress) {
        progress("正在建立 Agent 控制通道...");
    }
    const auto forward = adb.runCommand(
        serial,
        {"forward", "tcp:0", "tcp:" + std::to_string(AndroidAgentDeployment::ControlPort)},
        std::chrono::seconds(5));
    if (forward.exitCode != 0 || forward.timedOut) {
        result.error = "建立 Agent ADB 转发失败: " + commandDetail(forward);
        return result;
    }
    try {
        const int port = std::stoi(trim(forward.standardOutput));
        if (port <= 0 || port > 65535) {
            throw std::out_of_range("port");
        }
        result.localPort = static_cast<std::uint16_t>(port);
    } catch (...) {
        result.error = "ADB 未返回有效的 Agent 本地端口";
        return result;
    }
    result.succeeded = true;
    return result;
}

} // namespace

AndroidAgentDeploymentResult
AndroidAgentDeployment::deployAndForward(const std::string& serial,
                                         const std::string& applicationDirectory,
                                         const ProgressCallback& progress) const
{
    AndroidAgentDeploymentResult result;
    if (!validSerial(serial)) {
        result.error = "ADB 设备序列号格式不合法";
        return result;
    }
    serialdemo::AdbTransport adb;
    if (!ensureRootAdb(adb, serial, progress, result.error)) {
        return result;
    }
    if (progress) {
        progress("正在识别板端 ABI...");
    }
    const auto abiResult =
        adb.runCommand(serial, {"shell", "getprop ro.product.cpu.abi"}, std::chrono::seconds(4));
    if (abiResult.exitCode != 0 || abiResult.timedOut) {
        result.error = "读取板端 ABI 失败: " + commandDetail(abiResult);
        return result;
    }
    result.abi = normalizeAndroidAbi(abiResult.standardOutput);
    if (result.abi.empty()) {
        result.error = "暂不支持板端 ABI: " + trim(abiResult.standardOutput);
        return result;
    }

    std::string bundleError;
    const auto bundle = loadAgentBundleFromApplication(applicationDirectory, bundleError);
    const auto* package = bundle ? selectAndroidAgentPackage(*bundle, result.abi) : nullptr;
    if (!package) {
        result.error = bundleError.empty() ? "Bundle 中缺少匹配 Android ABI 的程序包" : bundleError;
        return result;
    }
    const auto* agentFile = package->agentFile();
    result.binaryPath = agentFile->sourcePath.string();
    result.remotePath = RemoteBinary;
    const std::string remoteAgent = std::string(RemoteCurrent) + "/" + agentFile->path;
    if (adb.runCommand(
               serial, {"shell", "command -v sha256sum >/dev/null 2>&1"}, std::chrono::seconds(3))
            .exitCode != 0) {
        result.error = "Android 板端缺少 sha256sum，无法执行安全部署";
        return result;
    }

    if (progress) {
        progress("正在部署 Android Agent Bundle...");
    }
    const auto prepare = adb.runCommand(
        serial,
        {"shell", std::string("mkdir -p ") + RemoteVersions + " " + RemoteDirectory + "/.staging"},
        std::chrono::seconds(4));
    if (prepare.exitCode != 0 || prepare.timedOut) {
        result.error = "创建 Agent 目录失败: " + commandDetail(prepare);
        return result;
    }
    const std::string versionDirectory =
        std::string(RemoteVersions) + "/" + bundle->agentVersion + "-" + bundle->buildId;
    const std::string stagingDirectory =
        std::string(RemoteDirectory) + "/.staging/" + bundle->agentVersion + "-" + bundle->buildId;
    const auto prepareStaging =
        adb.runCommand(serial,
                       {"shell", "rm -rf " + stagingDirectory + " && mkdir -p " + stagingDirectory},
                       std::chrono::seconds(4));
    if (prepareStaging.exitCode != 0 || prepareStaging.timedOut) {
        result.error = "准备 Android Bundle 临时目录失败";
        return result;
    }
    for (const auto& file : package->files) {
        const std::string temporary = stagingDirectory + "/" + file.path;
        const auto prepareFile = adb.runCommand(
            serial, {"shell", "mkdir -p $(dirname " + temporary + ")"}, std::chrono::seconds(4));
        if (prepareFile.exitCode != 0 || prepareFile.timedOut) {
            adb.runCommand(
                serial, {"shell", "rm -rf " + stagingDirectory}, std::chrono::seconds(3));
            result.error = "准备 Android Bundle 文件目录失败: " + file.path;
            return result;
        }
        const auto push = adb.runCommand(
            serial, {"push", file.sourcePath.string(), temporary}, std::chrono::seconds(20));
        if (push.exitCode != 0 || push.timedOut) {
            adb.runCommand(
                serial, {"shell", "rm -rf " + stagingDirectory}, std::chrono::seconds(3));
            result.error = "推送 Android Bundle 文件失败: " + commandDetail(push);
            return result;
        }
        const auto remoteHash =
            adb.runCommand(serial, {"shell", "sha256sum " + temporary}, std::chrono::seconds(5));
        const std::string hash = trim(remoteHash.standardOutput);
        if (remoteHash.exitCode != 0 || remoteHash.timedOut || hash.size() < file.sha256.size() ||
            !std::equal(
                file.sha256.begin(), file.sha256.end(), hash.begin(), [](char lhs, char rhs) {
                    return std::tolower(static_cast<unsigned char>(lhs)) ==
                           std::tolower(static_cast<unsigned char>(rhs));
                })) {
            adb.runCommand(
                serial, {"shell", "rm -rf " + stagingDirectory}, std::chrono::seconds(3));
            result.error = "Android Bundle 文件远端 SHA-256 校验失败: " + file.path;
            return result;
        }
        const auto permissions = adb.runCommand(
            serial,
            {"shell", std::string(file.executable ? "chmod 755 " : "chmod 644 ") + temporary},
            std::chrono::seconds(3));
        if (permissions.exitCode != 0 || permissions.timedOut) {
            adb.runCommand(
                serial, {"shell", "rm -rf " + stagingDirectory}, std::chrono::seconds(3));
            result.error = "设置 Android Bundle 文件权限失败: " + file.path;
            return result;
        }
    }
    const std::string install = "rm -rf " + versionDirectory + " && mv " + stagingDirectory + " " +
                                versionDirectory + " && rm -f " + RemotePrevious + " && if [ -L " +
                                RemoteCurrent + " ] || [ -e " + RemoteCurrent + " ]; then mv " +
                                RemoteCurrent + " " + RemotePrevious + "; fi && ln -s " +
                                versionDirectory + " " + RemoteCurrent;
    const auto installResult = adb.runCommand(serial, {"shell", install}, std::chrono::seconds(8));
    if (installResult.exitCode != 0 || installResult.timedOut) {
        adb.runCommand(serial,
                       {"shell",
                        "if [ ! -e " + std::string(RemoteCurrent) + " ] && ([ -L " +
                            RemotePrevious + " ] || [ -e " + RemotePrevious + " ]); then mv " +
                            RemotePrevious + " " + RemoteCurrent + "; fi"},
                       std::chrono::seconds(4));
        result.error = "安装 Android Agent Bundle 失败: " + commandDetail(installResult);
        return result;
    }

    if (progress) {
        progress("正在启动真实板端 Agent...");
    }
    const std::string pidFile = std::string(RemoteDirectory) + "/agent.pid";
    const std::string stop = "if [ -r " + pidFile + " ]; then kill $(cat " + pidFile +
                             ") 2>/dev/null || true; fi; "
                             "for pid in $(pidof testtools-agent 2>/dev/null); do kill -9 $pid "
                             "2>/dev/null || true; done; "
                             "rm -f " +
                             pidFile;
    adb.runCommand(serial, {"shell", stop}, std::chrono::seconds(3));
    const std::string rollback = "if [ -r " + pidFile + " ]; then kill \"$(cat " + pidFile +
                                 ")\" 2>/dev/null || true; fi; rm -f " + pidFile + " " +
                                 RemoteCurrent + "; if [ -L " + RemotePrevious + " ] || [ -e " +
                                 RemotePrevious + " ]; then mv " + RemotePrevious + " " +
                                 RemoteCurrent + "; fi";
    const std::string log = std::string(RemoteDirectory) + "/agent.log";
    // Android toybox provides setsid on some images, but its option set is not
    // consistent (the RK3562 image, for example, has no `setsid -f`).  nohup
    // plus an explicit background job works across both toybox and Ubuntu.
    const std::string launch = ": >" + log + "; nohup " + remoteAgent + " --port 27183 >" + log +
                               " 2>&1 </dev/null & echo $! >" + pidFile;
    const auto launchResult = adb.runCommand(serial, {"shell", launch}, std::chrono::seconds(5));
    if (launchResult.exitCode != 0 || launchResult.timedOut) {
        adb.runCommand(serial, {"shell", rollback}, std::chrono::seconds(5));
        result.error = "启动 Agent 失败: " + commandDetail(launchResult);
        return result;
    }

    bool running = false;
    for (int attempt = 0; attempt < 10; ++attempt) {
        const auto probe = adb.runCommand(
            serial, {"shell", "cat " + log + " 2>/dev/null"}, std::chrono::seconds(2));
        if (probe.standardOutput.find("LISTENING 27183") != std::string::npos) {
            running = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    if (!running) {
        const auto logResult = adb.runCommand(
            serial, {"shell", "cat " + log + " 2>/dev/null"}, std::chrono::seconds(2));
        adb.runCommand(serial, {"shell", rollback}, std::chrono::seconds(5));
        result.error =
            "Agent 未正常监听: " + trim(logResult.standardOutput + logResult.standardError);
        return result;
    }
    const auto identity =
        adb.runCommand(serial, {"shell", rootAgentProbe(pidFile)}, std::chrono::seconds(3));
    if (identity.standardOutput.find("RUNNING_ROOT") == std::string::npos) {
        adb.runCommand(serial, {"shell", rollback}, std::chrono::seconds(5));
        result.error = "Agent 已启动但未以 root 身份运行: " +
                       trim(identity.standardOutput + identity.standardError);
        return result;
    }
    return createForward(adb, serial, result.abi, result.binaryPath, progress);
}

AndroidAgentDeploymentResult
AndroidAgentDeployment::reconnect(const std::string& serial, const ProgressCallback& progress) const
{
    AndroidAgentDeploymentResult result;
    if (!validSerial(serial)) {
        result.error = "ADB 设备序列号格式不合法";
        return result;
    }
    serialdemo::AdbTransport adb;
    if (progress) {
        progress("正在探测已运行的板端 Agent...");
    }
    const std::string pidFile = std::string(RemoteDirectory) + "/agent.pid";
    const auto probe =
        adb.runCommand(serial, {"shell", rootAgentProbe(pidFile)}, std::chrono::seconds(3));
    if (probe.standardOutput.find("RUNNING_ROOT") == std::string::npos) {
        result.error = probe.standardOutput.find("RUNNING_UID=") != std::string::npos
                           ? "板端 Agent 不是 root 身份，请重新部署"
                           : "板端 Agent 未运行，请重新部署";
        return result;
    }
    const auto abiResult =
        adb.runCommand(serial, {"shell", "getprop ro.product.cpu.abi"}, std::chrono::seconds(3));
    if (abiResult.exitCode != 0 || abiResult.timedOut) {
        result.error = "读取板端 ABI 失败: " + commandDetail(abiResult);
        return result;
    }
    const std::string abi = normalizeAndroidAbi(abiResult.standardOutput);
    if (abi.empty()) {
        result.error = "暂不支持板端 ABI: " + trim(abiResult.standardOutput);
        return result;
    }
    return createForward(adb, serial, abi, RemoteBinary, progress);
}

void AndroidAgentDeployment::removeForward(const std::string& serial, std::uint16_t localPort) const
{
    if (!validSerial(serial) || localPort == 0) {
        return;
    }
    serialdemo::AdbTransport adb;
    adb.runCommand(serial,
                   {"forward", "--remove", "tcp:" + std::to_string(localPort)},
                   std::chrono::seconds(3));
}

void AndroidAgentDeployment::removeControlForwards(const std::string& serial) const
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

bool AndroidAgentDeployment::stopRemote(const std::string& serial, std::string& error) const
{
    if (!validSerial(serial)) {
        error = "ADB 设备序列号格式不合法";
        return false;
    }
    serialdemo::AdbTransport adb;
    if (!ensureRootAdb(adb, serial, {}, error)) {
        return false;
    }
    const std::string pidFile = std::string(RemoteDirectory) + "/agent.pid";
    const std::string command = "if [ -r " + pidFile + " ]; then kill $(cat " + pidFile +
                                ") 2>/dev/null || true; fi; "
                                "for pid in $(pidof testtools-agent 2>/dev/null); do kill -9 $pid "
                                "2>/dev/null || true; done; "
                                "rm -f " +
                                pidFile;
    const auto result = adb.runCommand(serial, {"shell", command}, std::chrono::seconds(4));
    if (result.exitCode != 0 || result.timedOut) {
        error = "停止 Agent 失败: " + commandDetail(result);
        return false;
    }
    error.clear();
    return true;
}

} // namespace testtools
