#include "adapters/adb/adb_transport.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <regex>
#include <set>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace serialdemo {

namespace {

std::string trim(std::string value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) { return std::isspace(ch); });
    value.erase(value.begin(), first);
    return value;
}

std::vector<std::string> splitLines(const std::string &text)
{
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        line = trim(line);
        if (!line.empty()) {
            lines.push_back(std::move(line));
        }
    }
    return lines;
}

std::string commandError(const std::string &operation, const CommandResult &result)
{
    if (result.timedOut) {
        return operation + "超时";
    }
    const std::string detail = trim(result.standardError.empty() ? result.standardOutput : result.standardError);
    return detail.empty() ? operation + "失败，退出码 " + std::to_string(result.exitCode)
                          : operation + "失败: " + detail;
}

#ifdef _WIN32
std::wstring environmentVariable(const wchar_t *name)
{
    // 空缓冲区查询返回的长度已包含结尾空字符；额外多分配一个宽字符防止超长路径被截断。
    const DWORD requiredLength = GetEnvironmentVariableW(name, nullptr, 0);
    if (requiredLength == 0) {
        return {};
    }
    std::wstring value(static_cast<std::size_t>(requiredLength) + 1, L'\0');
    const DWORD valueLength = GetEnvironmentVariableW(
        name, value.data(), static_cast<DWORD>(value.size()));
    if (valueLength == 0) {
        return {};
    }
    value.resize(valueLength);
    return value;
}

std::string wideToUtf8(const std::wstring &text)
{
    if (text.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), length, nullptr, nullptr);
    return result;
}

std::string pathToUtf8(const std::filesystem::path &path)
{
    return wideToUtf8(path.wstring());
}
#else
std::string environmentVariable(const char *name)
{
    const char *value = std::getenv(name);
    return value ? value : "";
}

std::string pathToUtf8(const std::filesystem::path &path)
{
    return path.string();
}
#endif

std::string resolveAdbProgram(const std::string &requested)
{
    if (!requested.empty() && requested != "adb" && requested != "adb.exe") {
        return requested;
    }

#ifdef _WIN32
    // 设备发现、Agent 部署和外部终端统一在此处解析 ADB 可执行程序路径。
    std::vector<std::filesystem::path> candidates;
    const auto adbPath = environmentVariable(L"ADB_PATH");
    if (!adbPath.empty()) {
        const std::filesystem::path configured(adbPath);
        candidates.push_back(configured);
        candidates.push_back(configured / L"adb.exe");
    }
    wchar_t moduleBuffer[32768] = {};
    const DWORD moduleLength = GetModuleFileNameW(nullptr, moduleBuffer,
                                                  static_cast<DWORD>(std::size(moduleBuffer)));
    if (moduleLength > 0 && moduleLength < std::size(moduleBuffer)) {
        const auto executableDirectory = std::filesystem::path(moduleBuffer).parent_path();
        candidates.emplace_back(executableDirectory / L"platform-tools" / L"adb.exe");
    }
    for (const wchar_t *sdkName : {L"ANDROID_SDK_ROOT", L"ANDROID_HOME"}) {
        const auto sdk = environmentVariable(sdkName);
        if (!sdk.empty()) {
            candidates.emplace_back(std::filesystem::path(sdk) / L"platform-tools" / L"adb.exe");
        }
    }
    const auto localAppData = environmentVariable(L"LOCALAPPDATA");
    if (!localAppData.empty()) {
        candidates.emplace_back(std::filesystem::path(localAppData) / L"Android" / L"Sdk" /
                                L"platform-tools" / L"adb.exe");
    }
    for (const auto &candidate : candidates) {
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error)) {
            return pathToUtf8(std::filesystem::absolute(candidate));
        }
    }

    wchar_t searchResult[32768] = {};
    const DWORD searchLength = SearchPathW(nullptr, L"adb.exe", nullptr,
                                           static_cast<DWORD>(std::size(searchResult)),
                                           searchResult, nullptr);
    if (searchLength > 0 && searchLength < std::size(searchResult)) {
        return wideToUtf8(std::wstring(searchResult, searchLength));
    }
#endif
    return requested.empty() ? "adb" : requested;
}

} // namespace

AdbTransport::AdbTransport(std::string adbProgram)
    : adbProgram_(resolveAdbProgram(adbProgram))
{
}

AdbTransport::~AdbTransport()
{
    close();
}

bool AdbTransport::openShellTerminal(const std::string &serial,
                                     std::string &host, std::string &error) const
{
    // 在启动时一次性绑定目标设备序列号；后续界面切换设备不得影响已打开的外部终端会话。
    return ProcessRunner::openTerminal(adbProgram_, {"-s", serial, "shell"}, host, error);
}

CommandResult AdbTransport::runAdb(const std::string &serial,
                                   const std::vector<std::string> &arguments,
                                   std::chrono::milliseconds timeout) const
{
    std::vector<std::string> fullArguments;
    if (!serial.empty()) {
        fullArguments.push_back("-s");
        fullArguments.push_back(serial);
    }
    fullArguments.insert(fullArguments.end(), arguments.begin(), arguments.end());
    return ProcessRunner::run(adbProgram_, fullArguments, timeout);
}

CommandResult AdbTransport::runCommand(const std::string &serial,
                                       const std::vector<std::string> &arguments,
                                       std::chrono::milliseconds timeout) const
{
    return runAdb(serial, arguments, timeout);
}

std::vector<AdbDevice> AdbTransport::availableDevices(std::string &error) const
{
    const auto result = runAdb({}, {"devices", "-l"});
    if (result.exitCode != 0 || result.timedOut) {
        error = commandError("枚举 ADB 设备", result);
        return {};
    }

    std::vector<AdbDevice> devices;
    for (const auto &line : splitLines(result.standardOutput)) {
        if (line.rfind("List of devices", 0) == 0) {
            continue;
        }
        std::istringstream fields(line);
        std::string serial;
        std::string state;
        fields >> serial >> state;
        if (serial.empty() || state.empty()) {
            continue;
        }
        std::string description;
        std::getline(fields, description);
        devices.push_back({serial, state, trim(description)});
    }
    error.clear();
    return devices;
}

std::vector<std::string> AdbTransport::availableRemotePorts(const std::string &serial,
                                                            std::string &error) const
{
    const auto result = runAdb(serial,
                               {"shell", "printf '%s\\n' /dev/ttyS* /dev/ttyUSB* /dev/ttyACM*"});
    if (result.exitCode != 0 || result.timedOut) {
        error = commandError("枚举板端串口", result);
        return {};
    }

    static const std::regex allowed(R"(^/dev/[A-Za-z0-9._-]+$)");
    std::set<std::string> unique;
    for (const auto &line : splitLines(result.standardOutput)) {
        if (std::regex_match(line, allowed)) {
            unique.insert(line);
        }
    }
    error.clear();
    return {unique.begin(), unique.end()};
}

bool AdbTransport::validateConfig(const SerialConfig &config, std::string &error) const
{
    static const std::regex endpointPattern(R"(^/dev/[A-Za-z0-9._-]+$)");
    static const std::regex serialPattern(R"(^[A-Za-z0-9._:-]+$)");
    if (!std::regex_match(config.endpoint, endpointPattern)) {
        error = "板端串口节点格式不合法";
        return false;
    }
    if (!std::regex_match(config.adbSerial, serialPattern)) {
        error = "ADB 设备序列号格式不合法";
        return false;
    }
    if (config.baudRate <= 0 || config.baudRate > 4000000) {
        error = "波特率超出允许范围";
        return false;
    }
    if (config.dataBits < 5 || config.dataBits > 8) {
        error = "数据位必须为 5 到 8";
        return false;
    }
    return true;
}

bool AdbTransport::ensureEndpointAccess(const SerialConfig &config, std::string &error)
{
    const auto exists = runAdb(config.adbSerial,
                               {"shell", "test -e " + config.endpoint},
                               std::chrono::seconds(3));
    if (exists.exitCode != 0 || exists.timedOut) {
        error = "板端串口节点不存在或不可访问: " + config.endpoint;
        return false;
    }

    const auto probeAccess = [this, &config] {
        return runAdb(config.adbSerial,
                      {"shell", "test -r " + config.endpoint + " && test -w " + config.endpoint},
                      std::chrono::seconds(3));
    };
    auto access = probeAccess();
    if (access.exitCode == 0 && !access.timedOut) {
        return true;
    }
    if (!config.allowAdbRoot) {
        error = "板端串口权限不足: " + config.endpoint
                + "。可勾选“权限不足时尝试 ADB root”后重试";
        return false;
    }

    publishStatus(TransportState::Progress, "正在请求 ADB root 以访问板端串口...");
    const auto root = runAdb(config.adbSerial, {"root"}, std::chrono::seconds(8));
    if (root.exitCode != 0 || root.timedOut) {
        error = commandError("请求 ADB root", root)
                + "；量产版本可能禁止 adbd root，请改用板端 Agent";
        return false;
    }
    if (stopping_.load()) {
        error = "连接已取消";
        return false;
    }

    publishStatus(TransportState::Progress, "正在等待 ADB root 设备重新上线...");
    const auto wait = runAdb(config.adbSerial, {"wait-for-device"}, std::chrono::seconds(12));
    if (wait.exitCode != 0 || wait.timedOut) {
        error = commandError("等待 ADB 设备重新上线", wait);
        return false;
    }

    access = probeAccess();
    if (access.exitCode != 0 || access.timedOut) {
        error = "ADB root 后仍无法读写板端串口: " + config.endpoint
                + "，请检查 SELinux、设备节点属组或改用板端 Agent";
        return false;
    }
    return true;
}

bool AdbTransport::ensureEndpointNotBusy(const SerialConfig &config, std::string &error) const
{
    const auto result = runAdb(
        config.adbSerial,
        {"shell", "if command -v lsof >/dev/null 2>&1; then lsof " + config.endpoint
                      + " 2>/dev/null; fi"},
        std::chrono::seconds(3));
    if (result.timedOut || result.exitCode != 0) {
        // 进程占用检测属于可选诊断：旧版 Android 镜像未必提供 lsof，命令缺失不得阻塞端口打开。
        return true;
    }
    for (const auto &line : splitLines(result.standardOutput)) {
        if (line.rfind("COMMAND", 0) == 0) {
            continue;
        }
        if (line.find(config.endpoint) != std::string::npos) {
            error = "板端串口正在被系统进程占用: " + line
                    + "。直连可能干扰现有服务，请停止占用进程或选择其他端点";
            return false;
        }
    }
    return true;
}

std::string AdbTransport::sttyCommand(const SerialConfig &config, bool includeBaudRate) const
{
    std::ostringstream command;
    command << "stty -F " << config.endpoint << ' ';
    if (includeBaudRate) {
        command << config.baudRate << ' ';
    }
    command << "raw -echo cs" << config.dataBits << ' ';

    switch (config.parity) {
    case Parity::Odd:
        command << "parenb parodd ";
        break;
    case Parity::Even:
        command << "parenb -parodd ";
        break;
    case Parity::None:
        command << "-parenb ";
        break;
    }
    command << (config.stopBits == StopBits::Two ? "cstopb " : "-cstopb ");
    switch (config.flowControl) {
    case FlowControl::Hardware:
        command << "-ixon -ixoff crtscts";
        break;
    case FlowControl::Software:
        command << "ixon ixoff -crtscts";
        break;
    case FlowControl::None:
        command << "-ixon -ixoff -crtscts";
        break;
    }
    return command.str();
}

void AdbTransport::open(const SerialConfig &config)
{
    close();

    std::string error;
    if (!validateConfig(config, error)) {
        publishStatus(TransportState::Error, error);
        return;
    }

    stopping_.store(false);
    worker_ = std::thread(&AdbTransport::workerMain, this, config);
}

void AdbTransport::workerMain(SerialConfig config)
{
    std::string accessError;
    if (!ensureEndpointAccess(config, accessError)) {
        publishStatus(TransportState::Error, accessError);
        return;
    }
    if (stopping_.load()) {
        return;
    }

    if (!ensureEndpointNotBusy(config, accessError)) {
        publishStatus(TransportState::Error, accessError);
        return;
    }

    publishStatus(TransportState::Progress, "正在配置板端串口...");
    auto configureResult = runAdb(config.adbSerial,
                                  {"shell", sttyCommand(config)},
                                  std::chrono::seconds(4));
    if (configureResult.exitCode != 0 || configureResult.timedOut) {
        const bool usbSerial = config.endpoint.rfind("/dev/ttyUSB", 0) == 0
                               || config.endpoint.rfind("/dev/ttyACM", 0) == 0;
        if (!usbSerial) {
            publishStatus(TransportState::Error, commandError("配置板端串口", configureResult));
            return;
        }

        // 部分 Android USB 调制解调器驱动虽暴露 termios 波特率字段，但因底层物理链路为 USB
        // 而拒绝修改速率；此时去掉波特率参数重试其余有效的串口帧格式标志。
        configureResult = runAdb(config.adbSerial,
                                 {"shell", sttyCommand(config, false)},
                                 std::chrono::seconds(4));
        if (configureResult.exitCode != 0 || configureResult.timedOut) {
            publishStatus(TransportState::Error, commandError("配置 USB 串口", configureResult));
            return;
        }
        publishStatus(TransportState::Progress,
                      "正在按 USB 链路继续连接（驱动不接受波特率设置）...");
    }
    if (stopping_.load()) {
        return;
    }

    publishStatus(TransportState::Progress, "正在建立 ADB 设备节点转发...");
    const auto forwardResult = runAdb(config.adbSerial,
                                      {"forward", "tcp:0", "dev:" + config.endpoint},
                                      std::chrono::seconds(4));
    if (forwardResult.exitCode != 0 || forwardResult.timedOut) {
        publishStatus(TransportState::Error, commandError("建立 ADB 转发", forwardResult));
        return;
    }

    int localPort = 0;
    try {
        localPort = std::stoi(trim(forwardResult.standardOutput));
    } catch (...) {
        publishStatus(TransportState::Error, "ADB 未返回有效的本地转发端口");
        return;
    }

    try {
        ioContext_ = std::make_unique<asio::io_context>();
        {
            std::lock_guard<std::mutex> lock(socketMutex_);
            socket_ = std::make_unique<asio::ip::tcp::socket>(*ioContext_);
            socket_->connect({asio::ip::make_address("127.0.0.1"), static_cast<unsigned short>(localPort)});
        }
        open_.store(true);
        publishStatus(TransportState::Connected,
                      "ADB 串口已连接: " + config.endpoint + " (localhost:" + std::to_string(localPort) + ")");

        std::array<std::uint8_t, 16384> buffer{};
        while (!stopping_.load()) {
            asio::error_code errorCode;
            const auto received = socket_->read_some(asio::buffer(buffer), errorCode);
            if (errorCode) {
                if (!stopping_.load()) {
                    publishStatus(TransportState::Error,
                                  "ADB 串口连接中断: " + errorCode.message());
                }
                break;
            }
            if (received > 0) {
                publishData(ByteBuffer(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(received)));
            }
        }
    } catch (const std::exception &exception) {
        if (!stopping_.load()) {
            publishStatus(TransportState::Error,
                          "连接 ADB 转发端口失败: " + std::string(exception.what()));
        }
    }

    open_.store(false);
    {
        std::lock_guard<std::mutex> lock(socketMutex_);
        socket_.reset();
    }
    ioContext_.reset();
    removeForward(config.adbSerial, localPort);
    if (stopping_.load()) {
        publishStatus(TransportState::Closed, "ADB 串口已关闭");
    }
}

void AdbTransport::removeForward(const std::string &serial, int localPort) const
{
    if (localPort <= 0) {
        return;
    }

    const std::string localEndpoint = "tcp:" + std::to_string(localPort);

    // ADB 通常会在设备断开时自动移除转发。此处保持幂等清理：先查询宿主侧转发注册表，
    // 仅移除当前实例创建的精确端口映射；绝不使用 --remove-all 以免破坏其他工具的映射。
    const auto listResult = runAdb({}, {"forward", "--list"}, std::chrono::seconds(2));
    if (listResult.exitCode != 0 || listResult.timedOut) {
        return;
    }

    bool mappingExists = false;
    for (const auto &line : splitLines(listResult.standardOutput)) {
        std::istringstream fields(line);
        std::string ownerSerial;
        std::string local;
        fields >> ownerSerial >> local;
        if (ownerSerial == serial && local == localEndpoint) {
            mappingExists = true;
            break;
        }
    }

    if (mappingExists) {
        runAdb(serial,
               {"forward", "--remove", localEndpoint},
               std::chrono::seconds(3));
    }
}

void AdbTransport::close()
{
    stopping_.store(true);
    open_.store(false);
    {
        std::lock_guard<std::mutex> lock(socketMutex_);
        if (socket_) {
            asio::error_code ignored;
            socket_->shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
            socket_->close(ignored);
        }
    }
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
        worker_.join();
    }
}

bool AdbTransport::write(const ByteBuffer &data)
{
    if (data.empty()) {
        return true;
    }
    std::lock_guard<std::mutex> lock(socketMutex_);
    if (!open_.load() || !socket_ || !socket_->is_open()) {
        return false;
    }
    asio::error_code errorCode;
    const auto written = asio::write(*socket_, asio::buffer(data), errorCode);
    if (errorCode) {
        publishStatus(TransportState::Error, "ADB 串口发送失败: " + errorCode.message());
        return false;
    }
    return written == data.size();
}

bool AdbTransport::isOpen() const
{
    return open_.load();
}

} // namespace serialdemo
