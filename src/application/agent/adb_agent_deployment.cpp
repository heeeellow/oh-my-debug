#include "application/agent/adb_agent_deployment.h"

#include "application/agent/android_agent_deployment.h"
#include "application/agent/linux_agent_deployment.h"

#include "adapters/adb/adb_transport.h"

#include "application/agent/agent_target.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <regex>
#include <sstream>
#include <utility>

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

enum class Platform { Android, Linux, Unknown };

Platform detectPlatform(serialdemo::AdbTransport& adb,
                        const std::string& serial,
                        std::string& detail)
{
    const auto androidProbe =
        adb.runCommand(serial, {"shell", "getprop ro.product.cpu.abi"}, std::chrono::seconds(4));
    const std::string androidAbi = trim(androidProbe.standardOutput);
    if (androidProbe.exitCode == 0 && !androidProbe.timedOut &&
        parseAndroidAgentAbi(androidAbi)) {
        detail.clear();
        return Platform::Android;
    }
    const auto androidSdk = adb.runCommand(
        serial, {"shell", "getprop ro.build.version.sdk"}, std::chrono::seconds(4));
    const std::string sdk = trim(androidSdk.standardOutput);
    if (androidProbe.exitCode == 0 && !androidProbe.timedOut && androidSdk.exitCode == 0 &&
        !androidSdk.timedOut && std::regex_match(sdk, std::regex(R"(^[0-9]+$)"))) {
        detail.clear();
        return Platform::Android;
    }
    const auto linuxProbe =
        adb.runCommand(serial, {"shell", "uname -s"}, std::chrono::seconds(4));
    if (linuxProbe.exitCode == 0 && !linuxProbe.timedOut &&
        trim(linuxProbe.standardOutput) == "Linux") {
        detail.clear();
        return Platform::Linux;
    }
    detail = "无法通过 ADB 识别 Android 或 Linux 用户空间: " + androidAbi + "; " +
             commandDetail(linuxProbe);
    return Platform::Unknown;
}

AdbAgentDeploymentResult fromAndroid(AndroidAgentDeploymentResult value)
{
    AdbAgentDeploymentResult result;
    result.succeeded = value.succeeded;
    result.error = std::move(value.error);
    result.platform = "android";
    result.target = value.abi;
    result.abi = std::move(value.abi);
    result.binaryPath = std::move(value.binaryPath);
    result.remotePath = std::move(value.remotePath);
    result.localPort = value.localPort;
    return result;
}

AdbAgentDeploymentResult fromLinux(LinuxAgentDeploymentResult value)
{
    AdbAgentDeploymentResult result;
    result.succeeded = value.succeeded;
    result.error = std::move(value.error);
    result.platform = "linux";
    result.target = value.architecture + "/" + value.libc;
    result.abi = result.target;
    result.buildId = std::move(value.buildId);
    result.binaryPath = std::move(value.binaryPath);
    result.remotePath = std::move(value.remotePath);
    result.localPort = value.localPort;
    return result;
}

} // namespace

AdbAgentDeploymentResult AdbAgentDeployment::deployAndForward(
    const std::string& serial,
    const std::string& applicationDirectory,
    const ProgressCallback& progress) const
{
    AdbAgentDeploymentResult result;
    if (!validSerial(serial)) {
        result.error = "ADB 设备序列号格式不合法";
        return result;
    }
    serialdemo::AdbTransport adb;
    std::string detectionError;
    switch (detectPlatform(adb, serial, detectionError)) {
    case Platform::Android:
        return fromAndroid(AndroidAgentDeployment().deployAndForward(serial, applicationDirectory,
                                                                      progress));
    case Platform::Linux:
        return fromLinux(LinuxAgentDeployment().deployAndForward(serial, applicationDirectory,
                                                                 progress));
    case Platform::Unknown:
        result.error = detectionError;
        return result;
    }
    result.error = "未知 ADB 设备平台";
    return result;
}

AdbAgentDeploymentResult AdbAgentDeployment::reconnect(
    const std::string& serial, const ProgressCallback& progress) const
{
    AdbAgentDeploymentResult result;
    if (!validSerial(serial)) {
        result.error = "ADB 设备序列号格式不合法";
        return result;
    }
    serialdemo::AdbTransport adb;
    std::string detectionError;
    switch (detectPlatform(adb, serial, detectionError)) {
    case Platform::Android:
        return fromAndroid(AndroidAgentDeployment().reconnect(serial, progress));
    case Platform::Linux:
        return fromLinux(LinuxAgentDeployment().reconnect(serial, progress));
    case Platform::Unknown:
        result.error = detectionError;
        return result;
    }
    result.error = "未知 ADB 设备平台";
    return result;
}

void AdbAgentDeployment::removeForward(const std::string& serial, std::uint16_t localPort) const
{
    if (!validSerial(serial) || localPort == 0) {
        return;
    }
    serialdemo::AdbTransport adb;
    adb.runCommand(serial,
                   {"forward", "--remove", "tcp:" + std::to_string(localPort)},
                   std::chrono::seconds(3));
}

void AdbAgentDeployment::removeControlForwards(const std::string& serial) const
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
        if (owner == serial && remote == "tcp:27183" && local.rfind("tcp:", 0) == 0) {
            adb.runCommand(serial, {"forward", "--remove", local}, std::chrono::seconds(3));
        }
    }
}

bool AdbAgentDeployment::stopRemote(const std::string& serial, std::string& error) const
{
    if (!validSerial(serial)) {
        error = "ADB 设备序列号格式不合法";
        return false;
    }
    serialdemo::AdbTransport adb;
    std::string detectionError;
    switch (detectPlatform(adb, serial, detectionError)) {
    case Platform::Android: {
        return AndroidAgentDeployment().stopRemote(serial, error);
    }
    case Platform::Linux:
        return LinuxAgentDeployment().stopRemote(serial, error);
    case Platform::Unknown:
        error = detectionError;
        return false;
    }
    error = "未知 ADB 设备平台";
    return false;
}

} // namespace testtools
