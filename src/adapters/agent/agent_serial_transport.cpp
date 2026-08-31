#include "adapters/agent/agent_serial_transport.h"

#include "adapters/adb/adb_transport.h"
#include "adapters/agent/agent_client.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <regex>
#include <sstream>

namespace serialdemo {

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

std::string jsonEscape(const std::string &value)
{
    std::string result;
    result.reserve(value.size() + 8);
    for (const char ch : value) {
        switch (ch) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) >= 0x20) {
                result.push_back(ch);
            }
            break;
        }
    }
    return result;
}

std::string jsonUnescape(const std::string &value)
{
    std::string result;
    result.reserve(value.size());
    bool escaped = false;
    for (const char ch : value) {
        if (!escaped && ch == '\\') {
            escaped = true;
            continue;
        }
        if (escaped) {
            result.push_back(ch == 'n' ? '\n' : ch == 'r' ? '\r' : ch == 't' ? '\t' : ch);
            escaped = false;
        } else {
            result.push_back(ch);
        }
    }
    return result;
}

std::string stringField(const std::string &json, const std::string &name)
{
    const std::regex pattern("\\\"" + name + "\\\"\\s*:\\s*\\\"((?:\\\\.|[^\\\"])*)\\\"");
    std::smatch match;
    return std::regex_search(json, match, pattern) ? jsonUnescape(match[1].str()) : std::string{};
}

int intField(const std::string &json, const std::string &name)
{
    const std::regex pattern("\\\"" + name + "\\\"\\s*:\\s*(\\d+)");
    std::smatch match;
    if (!std::regex_search(json, match, pattern)) {
        return 0;
    }
    try {
        return std::stoi(match[1].str());
    } catch (...) {
        return 0;
    }
}

std::string responseError(const std::string &response, const std::string &fallback)
{
    const std::string message = stringField(response, "message");
    return message.empty() ? fallback : message;
}

std::string parityName(Parity parity)
{
    return parity == Parity::Odd ? "odd" : parity == Parity::Even ? "even" : "none";
}

std::string flowControlName(FlowControl flowControl)
{
    return flowControl == FlowControl::Hardware ? "hardware"
           : flowControl == FlowControl::Software ? "software" : "none";
}

} // namespace

AgentSerialTransport::AgentSerialTransport(std::string adbSerial,
                                           std::uint16_t agentControlPort)
    : adbSerial_(std::move(adbSerial)), agentControlPort_(agentControlPort)
{
}

AgentSerialTransport::~AgentSerialTransport()
{
    close();
}

std::vector<AgentSerialPort> AgentSerialTransport::availablePorts(
    std::uint16_t agentControlPort, std::string &error)
{
    if (agentControlPort == 0) {
        error = "请先在设置中部署并连接板端 Agent";
        return {};
    }
    testtools::AgentClient client;
    if (!client.connect("127.0.0.1", agentControlPort, error)) {
        return {};
    }
    std::string response;
    if (!client.request("{\"protocol\":1,\"request_id\":\"serial-list\","
                        "\"method\":\"serial.list\",\"params\":{}}",
                        response, error)) {
        return {};
    }
    if (response.find("\"ok\":true") == std::string::npos) {
        error = responseError(response, "Agent 无法枚举板端串口");
        return {};
    }

    std::vector<AgentSerialPort> ports;
    static const std::regex endpointPattern(
        R"json(\{"endpoint":"((?:\\.|[^"])*)","interface":"((?:\\.|[^"])*)","occupied_by":"((?:\\.|[^"])*)","readable":(true|false),"writable":(true|false)\})json");
    for (std::sregex_iterator iterator(response.begin(), response.end(), endpointPattern), end;
         iterator != end; ++iterator) {
        AgentSerialPort port;
        port.endpoint = jsonUnescape((*iterator)[1].str());
        port.interfaceName = jsonUnescape((*iterator)[2].str());
        port.occupiedBy = jsonUnescape((*iterator)[3].str());
        port.readable = (*iterator)[4].str() == "true";
        port.writable = (*iterator)[5].str() == "true";
        ports.push_back(std::move(port));
    }
    error.clear();
    return ports;
}

void AgentSerialTransport::open(const SerialConfig &config)
{
    close();
    if (agentControlPort_ == 0) {
        publishStatus(TransportState::Error, "请先在设置中部署并连接板端 Agent");
        return;
    }
    static const std::regex endpointPattern(R"(^/dev/tty[A-Za-z0-9._-]+$)");
    if (!std::regex_match(config.endpoint, endpointPattern)) {
        publishStatus(TransportState::Error, "板端串口节点格式不合法");
        return;
    }
    stopping_.store(false);
    worker_ = std::thread(&AgentSerialTransport::workerMain, this, config);
}

void AgentSerialTransport::workerMain(SerialConfig config)
{
    testtools::AgentClient control;
    std::string error;
    publishStatus(TransportState::Progress, "正在请求板端 Agent 打开串口...");
    if (!control.connect("127.0.0.1", agentControlPort_, error)) {
        publishStatus(TransportState::Error, error);
        return;
    }

    std::ostringstream request;
    request << "{\"protocol\":1,\"request_id\":\"serial-open\","
            << "\"method\":\"serial.open\",\"params\":{"
            << "\"endpoint\":\"" << jsonEscape(config.endpoint) << "\","
            << "\"baud_rate\":" << config.baudRate << ','
            << "\"data_bits\":" << config.dataBits << ','
            << "\"parity\":\"" << parityName(config.parity) << "\","
            << "\"stop_bits\":" << (config.stopBits == StopBits::Two ? 2 : 1) << ','
            << "\"flow_control\":\"" << flowControlName(config.flowControl) << "\"}}";
    std::string response;
    if (!control.request(request.str(), response, error)) {
        publishStatus(TransportState::Error, error);
        return;
    }
    if (response.find("\"ok\":true") == std::string::npos) {
        publishStatus(TransportState::Error,
                      responseError(response, "板端 Agent 打开串口失败"));
        return;
    }
    const int streamPort = intField(response, "stream_port");
    const std::string sessionId = stringField(response, "session_id");
    const int requestedBaudRate = intField(response, "baud_rate_requested");
    const int effectiveBaudRate = intField(response, "baud_rate_effective");
    const bool baudRateApplied = response.find("\"baud_rate_applied\":true")
                                 != std::string::npos;
    if (streamPort <= 0 || streamPort > 65535 || sessionId.empty()) {
        publishStatus(TransportState::Error, "板端 Agent 返回了无效的串口会话");
        return;
    }
    const auto closeSession = [&] {
        const std::string closeRequest =
            "{\"protocol\":1,\"request_id\":\"serial-close\",\"method\":\"serial.close\","
            "\"params\":{\"session_id\":\"" + jsonEscape(sessionId) + "\"}}";
        control.request(closeRequest, response, error);
    };
    if (stopping_.load()) {
        closeSession();
        return;
    }

    publishStatus(TransportState::Progress, "正在建立 Agent 串口数据通道...");
    AdbTransport adb;
    const auto forward = adb.runCommand(adbSerial_,
        {"forward", "tcp:0", "tcp:" + std::to_string(streamPort)});
    int localPort = 0;
    if (forward.exitCode == 0 && !forward.timedOut) {
        try {
            localPort = std::stoi(trim(forward.standardOutput));
        } catch (...) {
            localPort = 0;
        }
    }
    if (localPort <= 0 || localPort > 65535) {
        closeSession();
        publishStatus(TransportState::Error, "建立 Agent 串口 ADB 转发失败");
        return;
    }

    try {
        ioContext_ = std::make_unique<asio::io_context>();
        {
            std::lock_guard<std::mutex> lock(socketMutex_);
            socket_ = std::make_unique<asio::ip::tcp::socket>(*ioContext_);
            socket_->connect({asio::ip::make_address("127.0.0.1"),
                              static_cast<unsigned short>(localPort)});
        }
        open_.store(true);
        std::string connectedMessage = "板端 Agent 串口已连接: " + config.endpoint;
        const bool usbSerial = config.endpoint.rfind("/dev/ttyUSB", 0) == 0
                               || config.endpoint.rfind("/dev/ttyACM", 0) == 0;
        if (usbSerial && requestedBaudRate > 0) {
            connectedMessage += "（USB 虚拟串口，速率由驱动管理；请求 "
                                + std::to_string(requestedBaudRate);
            if (effectiveBaudRate > 0) {
                connectedMessage += "，内核报告 " + std::to_string(effectiveBaudRate);
            }
            connectedMessage += "）";
        } else if (!baudRateApplied && requestedBaudRate > 0) {
            connectedMessage += "（波特率未按请求生效";
            connectedMessage += "，请求 " + std::to_string(requestedBaudRate);
            if (effectiveBaudRate > 0) {
                connectedMessage += "，内核报告 " + std::to_string(effectiveBaudRate);
            }
            connectedMessage += "）";
        }
        publishStatus(TransportState::Connected, connectedMessage);

        std::array<std::uint8_t, 16384> buffer{};
        while (!stopping_.load()) {
            asio::error_code errorCode;
            const auto received = socket_->read_some(asio::buffer(buffer), errorCode);
            if (errorCode) {
                if (!stopping_.load()) {
                    publishStatus(TransportState::Error,
                                  "Agent 串口数据通道中断: " + errorCode.message());
                }
                break;
            }
            if (received > 0) {
                publishData(ByteBuffer(buffer.begin(), buffer.begin()
                                       + static_cast<std::ptrdiff_t>(received)));
            }
        }
    } catch (const std::exception &exception) {
        if (!stopping_.load()) {
            publishStatus(TransportState::Error,
                          "连接 Agent 串口数据通道失败: " + std::string(exception.what()));
        }
    }

    open_.store(false);
    {
        std::lock_guard<std::mutex> lock(socketMutex_);
        socket_.reset();
    }
    ioContext_.reset();
    removeForward(localPort);
    closeSession();
    if (stopping_.load()) {
        publishStatus(TransportState::Closed, "板端 Agent 串口已关闭");
    }
}

void AgentSerialTransport::removeForward(int localPort) const
{
    if (localPort <= 0 || adbSerial_.empty()) {
        return;
    }
    AdbTransport adb;
    adb.runCommand(adbSerial_,
                   {"forward", "--remove", "tcp:" + std::to_string(localPort)});
}

void AgentSerialTransport::close()
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

bool AgentSerialTransport::write(const ByteBuffer &data)
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
        publishStatus(TransportState::Error,
                      "Agent 串口发送失败: " + errorCode.message());
        return false;
    }
    return written == data.size();
}

bool AgentSerialTransport::isOpen() const
{
    return open_.load();
}

} // namespace serialdemo
