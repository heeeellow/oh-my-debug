#include "adapters/agent/agent_serial_transport.h"

#include "adapters/adb/adb_transport.h"
#include "adapters/agent/agent_client.h"
#include "core/protocol/json_value.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <regex>

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

std::string stringMember(const testtools::JsonValue &object, const char *name)
{
    const auto *field = object.find(name);
    return (field && field->isString()) ? field->stringValue() : std::string{};
}

int intMember(const testtools::JsonValue &object, const char *name)
{
    const auto *field = object.find(name);
    if (!field || !field->isInt64() || field->int64Value() < std::numeric_limits<int>::min() ||
        field->int64Value() > std::numeric_limits<int>::max()) {
        return 0;
    }
    return static_cast<int>(field->int64Value());
}

bool boolMember(const testtools::JsonValue &object, const char *name)
{
    const auto *field = object.find(name);
    return field && field->isBool() && field->boolValue();
}

const testtools::JsonValue *unwrapResult(const std::string &response,
                                         testtools::JsonValue &document,
                                         const std::string &fallbackError,
                                         std::string &error)
{
    std::string parseError;
    if (!testtools::parseJson(response, document, parseError) || !document.isObject()) {
        error = fallbackError;
        return nullptr;
    }
    if (!boolMember(document, "ok")) {
        const auto *errorObject = document.find("error");
        const std::string message =
            errorObject ? stringMember(*errorObject, "message") : std::string{};
        error = message.empty() ? fallbackError : message;
        return nullptr;
    }
    const auto *result = document.find("result");
    if (!result || !result->isObject()) {
        error = fallbackError;
        return nullptr;
    }
    return result;
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
    testtools::JsonValue document;
    const auto *result = unwrapResult(response, document, "Agent 无法枚举板端串口", error);
    if (!result) {
        return {};
    }

    std::vector<AgentSerialPort> ports;
    const auto *endpoints = result->find("endpoints");
    if (endpoints && endpoints->isArray()) {
        for (const auto &item : endpoints->arrayItems()) {
            if (!item.isObject()) {
                continue;
            }
            AgentSerialPort port;
            port.endpoint = stringMember(item, "endpoint");
            if (port.endpoint.empty()) {
                continue;
            }
            port.interfaceName = stringMember(item, "interface");
            port.occupiedBy = stringMember(item, "occupied_by");
            port.readable = boolMember(item, "readable");
            port.writable = boolMember(item, "writable");
            ports.push_back(std::move(port));
        }
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

    const std::string request = testtools::serializeJson(testtools::JsonValue::object({
        {"method", testtools::JsonValue::string("serial.open")},
        {"params",
         testtools::JsonValue::object({
             {"baud_rate", testtools::JsonValue::integer(config.baudRate)},
             {"data_bits", testtools::JsonValue::integer(config.dataBits)},
             {"endpoint", testtools::JsonValue::string(config.endpoint)},
             {"flow_control", testtools::JsonValue::string(flowControlName(config.flowControl))},
             {"parity", testtools::JsonValue::string(parityName(config.parity))},
             {"stop_bits",
              testtools::JsonValue::integer(config.stopBits == StopBits::Two ? 2 : 1)},
         })},
        {"protocol", testtools::JsonValue::integer(1)},
        {"request_id", testtools::JsonValue::string("serial-open")},
    }));
    std::string response;
    if (!control.request(request, response, error)) {
        publishStatus(TransportState::Error, error);
        return;
    }
    testtools::JsonValue document;
    const auto *result = unwrapResult(response, document, "板端 Agent 打开串口失败", error);
    if (!result) {
        publishStatus(TransportState::Error, error);
        return;
    }
    const int streamPort = intMember(*result, "stream_port");
    const std::string sessionId = stringMember(*result, "session_id");
    const int requestedBaudRate = intMember(*result, "baud_rate_requested");
    const int effectiveBaudRate = intMember(*result, "baud_rate_effective");
    const bool baudRateApplied = boolMember(*result, "baud_rate_applied");
    if (streamPort <= 0 || streamPort > 65535 || sessionId.empty()) {
        publishStatus(TransportState::Error, "板端 Agent 返回了无效的串口会话");
        return;
    }
    const auto closeSession = [&] {
        const std::string closeRequest = testtools::serializeJson(testtools::JsonValue::object({
            {"method", testtools::JsonValue::string("serial.close")},
            {"params",
             testtools::JsonValue::object({
                 {"session_id", testtools::JsonValue::string(sessionId)},
             })},
            {"protocol", testtools::JsonValue::integer(1)},
            {"request_id", testtools::JsonValue::string("serial-close")},
        }));
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
