#include "agent_deployment.h"

#include "core/adb_transport.h"

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
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch);
    });
    value.erase(value.begin(), first);
    return value;
}

bool validSerial(const std::string &serial)
{
    static const std::regex pattern(R"(^[A-Za-z0-9._:-]+$)");
    return std::regex_match(serial, pattern);
}

std::string commandDetail(const serialdemo::CommandResult &result)
{
    if (result.timedOut) {
        return "command timed out";
    }
    const std::string detail = trim(result.standardError.empty()
                                        ? result.standardOutput : result.standardError);
    return detail.empty() ? "exit code " + std::to_string(result.exitCode) : detail;
}

std::string normalizeAbi(std::string abi)
{
    abi = trim(std::move(abi));
    if (abi == "armeabi-v7a" || abi == "arm64-v8a" || abi == "x86" || abi == "x86_64") {
        return abi;
    }
    return {};
}

std::filesystem::path agentBinary(const std::filesystem::path &applicationDirectory,
                                  const std::string &abi)
{
    return applicationDirectory / "agents" / "android" / abi / "testtools-agent";
}

AgentDeploymentResult createForward(serialdemo::AdbTransport &adb, const std::string &serial,
                                    const std::string &abi, const std::string &binaryPath,
                                    const AgentDeployment::ProgressCallback &progress)
{
    AgentDeploymentResult result;
    result.abi = abi;
    result.binaryPath = binaryPath;
    result.remotePath = AgentDeployment::RemoteBinary;
    if (progress) {
        progress("正在建立 Agent 控制通道...");
    }
    const auto forward = adb.runCommand(serial,
        {"forward", "tcp:0", "tcp:" + std::to_string(AgentDeployment::ControlPort)},
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

AgentDeploymentResult AgentDeployment::deployAndForward(
    const std::string &serial, const std::string &applicationDirectory,
    const ProgressCallback &progress) const
{
    AgentDeploymentResult result;
    if (!validSerial(serial)) {
        result.error = "ADB 设备序列号格式不合法";
        return result;
    }
    serialdemo::AdbTransport adb;
    if (progress) {
        progress("正在识别板端 ABI...");
    }
    const auto abiResult = adb.runCommand(serial,
        {"shell", "getprop ro.product.cpu.abi"}, std::chrono::seconds(4));
    if (abiResult.exitCode != 0 || abiResult.timedOut) {
        result.error = "读取板端 ABI 失败: " + commandDetail(abiResult);
        return result;
    }
    result.abi = normalizeAbi(abiResult.standardOutput);
    if (result.abi.empty()) {
        result.error = "暂不支持板端 ABI: " + trim(abiResult.standardOutput);
        return result;
    }

    const std::filesystem::path binary = agentBinary(applicationDirectory, result.abi);
    std::error_code fileError;
    if (!std::filesystem::is_regular_file(binary, fileError)) {
        result.error = "缺少 " + result.abi + " Agent: " + binary.string();
        return result;
    }
    result.binaryPath = binary.string();
    result.remotePath = RemoteBinary;

    if (progress) {
        progress("正在部署真实板端 Agent...");
    }
    const auto prepare = adb.runCommand(serial,
        {"shell", std::string("mkdir -p ") + RemoteDirectory}, std::chrono::seconds(4));
    if (prepare.exitCode != 0 || prepare.timedOut) {
        result.error = "创建 Agent 目录失败: " + commandDetail(prepare);
        return result;
    }
    const std::string temporary = std::string(RemoteBinary) + ".new";
    const auto push = adb.runCommand(serial, {"push", result.binaryPath, temporary},
                                     std::chrono::seconds(20));
    if (push.exitCode != 0 || push.timedOut) {
        result.error = "推送 Agent 失败: " + commandDetail(push);
        return result;
    }
    const std::string install = "chmod 755 " + temporary + " && mv -f " + temporary
                                + " " + RemoteBinary;
    const auto installResult = adb.runCommand(serial, {"shell", install}, std::chrono::seconds(5));
    if (installResult.exitCode != 0 || installResult.timedOut) {
        result.error = "安装 Agent 失败: " + commandDetail(installResult);
        return result;
    }

    if (progress) {
        progress("正在启动真实板端 Agent...");
    }
    const std::string pidFile = std::string(RemoteDirectory) + "/agent.pid";
    const std::string stop = "if [ -r " + pidFile + " ]; then kill $(cat " + pidFile
        + ") 2>/dev/null || true; fi; "
          "for pid in $(pidof testtools-agent 2>/dev/null); do kill -9 $pid 2>/dev/null || true; done; "
          "rm -f " + pidFile;
    adb.runCommand(serial, {"shell", stop}, std::chrono::seconds(3));
    const std::string log = std::string(RemoteDirectory) + "/agent.log";
    // Android toybox provides setsid on some images, but its option set is not
    // consistent (the RK3562 image, for example, has no `setsid -f`).  nohup
    // plus an explicit background job works across both toybox and Ubuntu.
    const std::string launch = ": >" + log + "; nohup " + std::string(RemoteBinary)
        + " --port 27183 >" + log + " 2>&1 </dev/null & echo $! >" + pidFile;
    const auto launchResult = adb.runCommand(serial, {"shell", launch}, std::chrono::seconds(5));
    if (launchResult.exitCode != 0 || launchResult.timedOut) {
        result.error = "启动 Agent 失败: " + commandDetail(launchResult);
        return result;
    }

    bool running = false;
    for (int attempt = 0; attempt < 10; ++attempt) {
        const auto probe = adb.runCommand(serial,
            {"shell", "cat " + log + " 2>/dev/null"}, std::chrono::seconds(2));
        if (probe.standardOutput.find("LISTENING 27183") != std::string::npos) {
            running = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    if (!running) {
        const auto logResult = adb.runCommand(serial,
            {"shell", "cat " + log + " 2>/dev/null"}, std::chrono::seconds(2));
        result.error = "Agent 未正常监听: " + trim(logResult.standardOutput + logResult.standardError);
        return result;
    }
    return createForward(adb, serial, result.abi, result.binaryPath, progress);
}

AgentDeploymentResult AgentDeployment::reconnect(const std::string &serial,
                                                  const ProgressCallback &progress) const
{
    AgentDeploymentResult result;
    if (!validSerial(serial)) {
        result.error = "ADB 设备序列号格式不合法";
        return result;
    }
    serialdemo::AdbTransport adb;
    const auto probe = adb.runCommand(serial,
        {"shell", "cat " + std::string(RemoteDirectory) + "/agent.log 2>/dev/null"},
        std::chrono::seconds(3));
    if (probe.standardOutput.find("LISTENING 27183") == std::string::npos) {
        result.error = "板端 Agent 未运行，请重新部署";
        return result;
    }
    return createForward(adb, serial, {}, RemoteBinary, progress);
}

void AgentDeployment::removeForward(const std::string &serial, std::uint16_t localPort) const
{
    if (!validSerial(serial) || localPort == 0) {
        return;
    }
    serialdemo::AdbTransport adb;
    adb.runCommand(serial, {"forward", "--remove", "tcp:" + std::to_string(localPort)},
                   std::chrono::seconds(3));
}

void AgentDeployment::removeControlForwards(const std::string &serial) const
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
        if (owner == serial && remote == "tcp:" + std::to_string(ControlPort)
            && local.rfind("tcp:", 0) == 0) {
            adb.runCommand(serial, {"forward", "--remove", local}, std::chrono::seconds(3));
        }
    }
}

bool AgentDeployment::stopRemote(const std::string &serial, std::string &error) const
{
    if (!validSerial(serial)) {
        error = "ADB 设备序列号格式不合法";
        return false;
    }
    serialdemo::AdbTransport adb;
    const std::string pidFile = std::string(RemoteDirectory) + "/agent.pid";
    const std::string command = "if [ -r " + pidFile + " ]; then kill $(cat " + pidFile
        + ") 2>/dev/null || true; fi; "
          "for pid in $(pidof testtools-agent 2>/dev/null); do kill -9 $pid 2>/dev/null || true; done; "
          "rm -f " + pidFile;
    const auto result = adb.runCommand(serial, {"shell", command}, std::chrono::seconds(4));
    if (result.exitCode != 0 || result.timedOut) {
        error = "停止 Agent 失败: " + commandDetail(result);
        return false;
    }
    error.clear();
    return true;
}

} // namespace testtools
