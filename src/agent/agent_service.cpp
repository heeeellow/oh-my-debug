#include "agent_service.h"
#include "core/agent_version.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/utsname.h>
#include <unistd.h>
#endif

namespace testtools::agent {

namespace {

std::string jsonEscape(const std::string &value)
{
    std::string result;
    result.reserve(value.size() + 8);
    for (const unsigned char ch : value) {
        switch (ch) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (ch >= 0x20) {
                result.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    return result;
}

std::string stringField(const std::string &json, const std::string &name)
{
    const std::string key = "\"" + name + "\"";
    std::size_t position = json.find(key);
    if (position == std::string::npos) {
        return {};
    }
    position = json.find(':', position + key.size());
    if (position == std::string::npos) {
        return {};
    }
    ++position;
    while (position < json.size() && std::isspace(static_cast<unsigned char>(json[position]))) {
        ++position;
    }
    if (position >= json.size() || json[position] != '"') {
        return {};
    }
    std::string result;
    bool escaped = false;
    for (++position; position < json.size(); ++position) {
        const char ch = json[position];
        if (escaped) {
            switch (ch) {
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            default: return {};
            }
            escaped = false;
        } else if (ch == '\\') {
            escaped = true;
        } else if (ch == '"') {
            return result;
        } else if (static_cast<unsigned char>(ch) >= 0x20) {
            result.push_back(ch);
        } else {
            return {};
        }
    }
    return {};
}

bool objectField(const std::string &json, const std::string &name)
{
    const std::string key = "\"" + name + "\"";
    std::size_t position = json.find(key);
    if (position == std::string::npos) {
        return false;
    }
    position = json.find(':', position + key.size());
    if (position == std::string::npos) {
        return false;
    }
    ++position;
    while (position < json.size() && std::isspace(static_cast<unsigned char>(json[position]))) {
        ++position;
    }
    return position < json.size() && json[position] == '{';
}

enum class IntFieldStatus {
    Missing,
    Valid,
    Invalid,
};

IntFieldStatus intField(const std::string &json, const std::string &name, int &value)
{
    const std::string key = "\"" + name + "\"";
    std::size_t position = json.find(key);
    if (position == std::string::npos) {
        return IntFieldStatus::Missing;
    }
    position = json.find(':', position + key.size());
    if (position == std::string::npos) {
        return IntFieldStatus::Invalid;
    }
    ++position;
    while (position < json.size() && std::isspace(static_cast<unsigned char>(json[position]))) {
        ++position;
    }
    std::size_t end = position;
    if (end < json.size() && json[end] == '-') {
        ++end;
    }
    const std::size_t digitsBegin = end;
    while (end < json.size() && std::isdigit(static_cast<unsigned char>(json[end]))) {
        ++end;
    }
    if (end == digitsBegin) {
        return IntFieldStatus::Invalid;
    }
    std::size_t delimiter = end;
    while (delimiter < json.size()
           && std::isspace(static_cast<unsigned char>(json[delimiter]))) {
        ++delimiter;
    }
    if (delimiter < json.size() && json[delimiter] != ',' && json[delimiter] != '}'
        && json[delimiter] != ']') {
        return IntFieldStatus::Invalid;
    }
    try {
        value = std::stoi(json.substr(position, end - position));
        return IntFieldStatus::Valid;
    } catch (...) {
        return IntFieldStatus::Invalid;
    }
}

bool optionalIntField(const std::string &json, const std::string &name,
                      int fallback, int &value)
{
    value = fallback;
    const auto status = intField(json, name, value);
    return status != IntFieldStatus::Invalid;
}

std::string readLine(const std::string &path)
{
    std::ifstream input(path);
    std::string value;
    std::getline(input, value);
    while (!value.empty() && (value.back() == '\0' || value.back() == '\r' || value.back() == '\n')) {
        value.pop_back();
    }
    return value;
}

std::string buildProperty(const std::string &key)
{
    std::ifstream input("/system/build.prop");
    std::string line;
    const std::string prefix = key + '=';
    while (std::getline(input, line)) {
        if (line.rfind(prefix, 0) == 0) {
            return line.substr(prefix.size());
        }
    }
    return {};
}

std::string platformName()
{
#ifdef __ANDROID__
    return "android";
#elif defined(_WIN32)
    return "windows";
#else
    return "linux";
#endif
}

std::string systemInfoJson()
{
    std::string host;
    std::string kernel;
    std::string architecture;
#ifdef _WIN32
    char buffer[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD size = static_cast<DWORD>(sizeof(buffer));
    if (GetComputerNameA(buffer, &size)) {
        host.assign(buffer, size);
    }
    kernel = "windows";
#if defined(_M_X64)
    architecture = "x86_64";
#elif defined(_M_ARM64)
    architecture = "arm64";
#else
    architecture = "unknown";
#endif
    const int uid = 0;
#else
    utsname information{};
    if (uname(&information) == 0) {
        host = information.nodename;
        kernel = std::string(information.sysname) + " " + information.release;
        architecture = information.machine;
    }
    const int uid = static_cast<int>(geteuid());
#endif
    std::string model = readLine("/proc/device-tree/model");
    if (model.empty()) {
        model = buildProperty("ro.product.model");
    }
    std::string release = buildProperty("ro.build.version.release");
    if (release.empty()) {
        release = readLine("/etc/os-release");
    }
    return "{\"platform\":\"" + jsonEscape(platformName())
           + "\",\"hostname\":\"" + jsonEscape(host)
           + "\",\"kernel\":\"" + jsonEscape(kernel)
           + "\",\"architecture\":\"" + jsonEscape(architecture)
           + "\",\"model\":\"" + jsonEscape(model)
           + "\",\"os_release\":\"" + jsonEscape(release)
           + "\",\"effective_uid\":" + std::to_string(uid) + "}";
}

std::string errorResponse(const std::string &requestId, const std::string &code,
                          const std::string &message)
{
    return "{\"protocol\":1,\"request_id\":\"" + jsonEscape(requestId)
           + "\",\"ok\":false,\"error\":{\"code\":\"" + jsonEscape(code)
           + "\",\"message\":\"" + jsonEscape(message) + "\"}}";
}

std::string resultResponse(const std::string &requestId, const std::string &result)
{
    return "{\"protocol\":1,\"request_id\":\"" + jsonEscape(requestId)
           + "\",\"ok\":true,\"result\":" + result + "}";
}

} // namespace

AgentService::~AgentService()
{
    stop();
}

std::string AgentService::handleRequest(const std::string &request)
{
    const std::string requestId = stringField(request, "request_id");
    const std::string method = stringField(request, "method");
    if (requestId.empty() || method.empty() || !objectField(request, "params")) {
        return errorResponse(requestId, "invalid_request",
                             "request_id, method and params object are required");
    }
    int protocol = 0;
    const auto protocolStatus = intField(request, "protocol", protocol);
    if (protocolStatus != IntFieldStatus::Valid) {
        return errorResponse(requestId, "invalid_request", "protocol must be an integer");
    }
    if (protocol != AgentProtocolVersion) {
        return errorResponse(requestId, "unsupported_protocol",
                             "only protocol version 1 is supported");
    }
    if (method == "session.hello") {
        return resultResponse(requestId,
            std::string("{\"agent\":\"testtools-agent\",\"agent_version\":\"")
            + AgentVersion + "\","
            "\"protocol_min\":1,\"protocol_max\":1,\"platform\":\""
            + platformName() + "\"}");
    }
    if (method == "capability.list") {
        return resultResponse(requestId,
            "{\"capabilities\":["
            "{\"id\":\"system.info\",\"name\":\"System information\",\"version\":1,"
            "\"risk\":\"read_only\",\"supports_cancel\":false},"
            "{\"id\":\"serial.stream\",\"name\":\"Managed serial stream\",\"version\":1,"
            "\"risk\":\"device_io\",\"supports_cancel\":true}]}");
    }
    if (method == "system.info") {
        return resultResponse(requestId, systemInfoJson());
    }
    if (method == "serial.list") {
        return serialListResponse(requestId);
    }
    if (method == "serial.open") {
        return serialOpenResponse(requestId, request);
    }
    if (method == "serial.close") {
        return serialCloseResponse(requestId, request);
    }
    return errorResponse(requestId, "method_not_found", "method is not available");
}

std::string AgentService::serialListResponse(const std::string &requestId) const
{
    const auto endpoints = listSerialEndpoints();
    std::ostringstream result;
    result << "{\"endpoints\":[";
    for (std::size_t index = 0; index < endpoints.size(); ++index) {
        if (index > 0) {
            result << ',';
        }
        const auto &endpoint = endpoints[index];
        result << "{\"endpoint\":\"" << jsonEscape(endpoint.endpoint)
               << "\",\"interface\":\"" << jsonEscape(endpoint.interfaceName)
               << "\",\"occupied_by\":\"" << jsonEscape(endpoint.occupiedBy)
               << "\",\"readable\":" << (endpoint.readable ? "true" : "false")
               << ",\"writable\":" << (endpoint.writable ? "true" : "false") << '}';
    }
    result << "]}";
    return resultResponse(requestId, result.str());
}

std::string AgentService::serialOpenResponse(const std::string &requestId,
                                             const std::string &request)
{
    serialdemo::SerialConfig config;
    config.endpoint = stringField(request, "endpoint");
    int stopBits = 1;
    if (config.endpoint.empty()) {
        return errorResponse(requestId, "invalid_params", "endpoint is required");
    }
    if (!optionalIntField(request, "baud_rate", 115200, config.baudRate)
        || !optionalIntField(request, "data_bits", 8, config.dataBits)
        || !optionalIntField(request, "stop_bits", 1, stopBits)) {
        return errorResponse(requestId, "invalid_params",
                             "baud_rate, data_bits and stop_bits must be integers");
    }
    if (config.baudRate <= 0 || config.dataBits < 5 || config.dataBits > 8
        || (stopBits != 1 && stopBits != 2)) {
        return errorResponse(requestId, "invalid_params",
                             "serial framing parameters are out of range");
    }
    const std::string parity = stringField(request, "parity");
    if (!parity.empty() && parity != "none" && parity != "odd" && parity != "even") {
        return errorResponse(requestId, "invalid_params", "unsupported parity value");
    }
    config.parity = parity == "odd" ? serialdemo::Parity::Odd
                    : parity == "even" ? serialdemo::Parity::Even : serialdemo::Parity::None;
    config.stopBits = stopBits == 2
                        ? serialdemo::StopBits::Two : serialdemo::StopBits::One;
    const std::string flow = stringField(request, "flow_control");
    if (!flow.empty() && flow != "none" && flow != "hardware" && flow != "software") {
        return errorResponse(requestId, "invalid_params", "unsupported flow_control value");
    }
    config.flowControl = flow == "hardware" ? serialdemo::FlowControl::Hardware
                         : flow == "software" ? serialdemo::FlowControl::Software
                                               : serialdemo::FlowControl::None;

    std::lock_guard<std::mutex> lock(serialMutex_);
    if (serialBridge_.isRunning()) {
        return errorResponse(requestId, "resource_busy",
                             "another serial session is already active");
    }
    std::uint16_t port = 0;
    std::string sessionId;
    int effectiveBaudRate = 0;
    bool baudRateApplied = false;
    std::string error;
    if (!serialBridge_.start(config, port, sessionId, effectiveBaudRate,
                             baudRateApplied, error)) {
        return errorResponse(requestId, "serial_open_failed", error);
    }
    activeSerialSessionId_ = sessionId;
    return resultResponse(requestId,
        "{\"session_id\":\"" + jsonEscape(sessionId)
        + "\",\"stream_port\":" + std::to_string(port)
        + ",\"endpoint\":\"" + jsonEscape(config.endpoint)
        + "\",\"baud_rate_requested\":" + std::to_string(config.baudRate)
        + ",\"baud_rate_effective\":" + std::to_string(effectiveBaudRate)
        + ",\"baud_rate_applied\":" + (baudRateApplied ? "true" : "false") + "}");
}

std::string AgentService::serialCloseResponse(const std::string &requestId,
                                              const std::string &request)
{
    std::lock_guard<std::mutex> lock(serialMutex_);
    const std::string sessionId = stringField(request, "session_id");
    if (activeSerialSessionId_.empty() || sessionId != activeSerialSessionId_) {
        return errorResponse(requestId, "session_not_found",
                             "serial session_id is missing or no longer active");
    }
    serialBridge_.stop();
    activeSerialSessionId_.clear();
    return resultResponse(requestId, "{\"closed\":true}");
}

void AgentService::stop()
{
    std::lock_guard<std::mutex> lock(serialMutex_);
    serialBridge_.stop();
    activeSerialSessionId_.clear();
}

} // namespace testtools::agent
