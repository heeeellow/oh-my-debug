#include "agent_service.h"
#include "core/protocol/agent_version.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <map>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/utsname.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif
#endif

namespace testtools::agent {

namespace {

const JsonValue* member(const JsonValue& object, const char* name)
{
    return object.isObject() ? object.find(name) : nullptr;
}

bool stringMember(const JsonValue& object, const char* name, std::string& value)
{
    const auto* field = member(object, name);
    if (!field || !field->isString()) {
        return false;
    }
    value = field->stringValue();
    return true;
}

bool intMember(const JsonValue& object, const char* name, int& value)
{
    const auto* field = member(object, name);
    if (!field || !field->isInt64() || field->int64Value() < std::numeric_limits<int>::min() ||
        field->int64Value() > std::numeric_limits<int>::max()) {
        return false;
    }
    value = static_cast<int>(field->int64Value());
    return true;
}

bool optionalIntMember(const JsonValue& object, const char* name, int fallback, int& value)
{
    value = fallback;
    if (!member(object, name)) {
        return true;
    }
    return intMember(object, name, value);
}

bool hasOnlyMembers(const JsonValue& object, std::initializer_list<const char*> allowed)
{
    if (!object.isObject()) {
        return false;
    }
    for (const auto& entry : object.objectItems()) {
        const bool found = std::any_of(
            allowed.begin(), allowed.end(), [&](const char* name) { return entry.first == name; });
        if (!found) {
            return false;
        }
    }
    return true;
}

bool validIdentifier(const std::string& value, bool allowSlash = false)
{
    if (value.empty() || value.size() > 128 || value == "." || value == ".." ||
        (allowSlash && (value.front() == '/' || value.back() == '/'))) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [allowSlash](unsigned char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '_' || character == '-' ||
               character == '.' || character == ':' || (allowSlash && character == '/');
    });
}

std::vector<BoardCapabilityDefinition>
validatedCapabilities(const AgentService::CapabilityListFunction& capabilityList)
{
    if (!capabilityList)
        return {};
    const auto candidates = capabilityList();
    std::map<std::string, std::size_t> idCounts;
    for (const auto& candidate : candidates)
        ++idCounts[candidate.id];

    std::vector<BoardCapabilityDefinition> definitions;
    for (const auto& candidate : candidates) {
        const bool metadataValid =
            validIdentifier(candidate.id) && candidate.version > 0 &&
            validIdentifier(candidate.provider.kind) && validIdentifier(candidate.provider.id) &&
            candidate.provider.version > 0 && validIdentifier(candidate.observationSchema, true) &&
            !candidate.risks.empty() && !candidate.resources.empty() &&
            candidate.defaultTimeoutMs > 0 && candidate.defaultTimeoutMs <= 60 * 60 * 1000;
        if (metadataValid && idCounts[candidate.id] == 1)
            definitions.push_back(candidate);
    }
    return definitions;
}

std::string readLine(const std::string& path)
{
    std::ifstream input(path);
    std::string value;
    std::getline(input, value);
    while (!value.empty() &&
           (value.back() == '\0' || value.back() == '\r' || value.back() == '\n')) {
        value.pop_back();
    }
    return value;
}

std::string buildProperty(const std::string& key)
{
#ifdef __ANDROID__
    char value[PROP_VALUE_MAX] = {};
    const int length = __system_property_get(key.c_str(), value);
    if (length > 0) {
        return std::string(value, static_cast<std::size_t>(length));
    }
#endif
    for (const char* path : {"/system/build.prop",
                             "/vendor/build.prop",
                             "/product/build.prop",
                             "/system_ext/build.prop",
                             "/odm/build.prop"}) {
        std::ifstream input(path);
        std::string line;
        const std::string prefix = key + '=';
        while (std::getline(input, line)) {
            if (line.rfind(prefix, 0) == 0) {
                return line.substr(prefix.size());
            }
        }
    }
    return {};
}

std::string keyValue(const std::string& path, const std::string& key, char delimiter = '=')
{
    std::ifstream input(path);
    std::string line;
    const std::string prefix = key + delimiter;
    while (std::getline(input, line)) {
        if (line.rfind(prefix, 0) != 0) {
            continue;
        }
        std::string value = line.substr(prefix.size());
        const auto first = value.find_first_not_of(" \t");
        value = first == std::string::npos ? std::string{} : value.substr(first);
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        return value;
    }
    return {};
}

long long memoryTotalKib()
{
    const std::string value = keyValue("/proc/meminfo", "MemTotal", ':');
    try {
        return value.empty() ? 0 : std::stoll(value);
    } catch (...) {
        return 0;
    }
}

std::string selinuxMode()
{
    const std::string enforce = readLine("/sys/fs/selinux/enforce");
    if (enforce == "1") {
        return "Enforcing";
    }
    if (enforce == "0") {
        return "Permissive";
    }
    return "Disabled";
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

JsonValue systemInfoValue()
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
    const long processorCount = 0;
#else
    utsname information{};
    if (uname(&information) == 0) {
        host = information.nodename;
        kernel = std::string(information.sysname) + " " + information.release;
        architecture = information.machine;
    }
    const int uid = static_cast<int>(geteuid());
    const long processorCount = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    std::string model = readLine("/proc/device-tree/model");
    if (model.empty()) {
        model = buildProperty("ro.product.model");
    }
    std::string release = buildProperty("ro.build.version.release");
    if (release.empty()) {
        release = keyValue("/etc/os-release", "PRETTY_NAME");
    }
    const std::string manufacturer = buildProperty("ro.product.manufacturer");
    const std::string brand = buildProperty("ro.product.brand");
    const std::string board = buildProperty("ro.product.board");
    const std::string device = buildProperty("ro.product.device");
    const std::string product = buildProperty("ro.product.name");
    const std::string hardware = buildProperty("ro.hardware");
    const std::string sdk = buildProperty("ro.build.version.sdk");
    const std::string buildId = buildProperty("ro.build.id");
    return JsonValue::object({
        {"android_sdk", JsonValue::string(sdk)},
        {"architecture", JsonValue::string(architecture)},
        {"board", JsonValue::string(board)},
        {"brand", JsonValue::string(brand)},
        {"build_id", JsonValue::string(buildId)},
        {"cpu_cores", JsonValue::integer(std::max(0L, processorCount))},
        {"device", JsonValue::string(device)},
        {"effective_uid", JsonValue::integer(uid)},
        {"hardware", JsonValue::string(hardware)},
        {"hostname", JsonValue::string(host)},
        {"kernel", JsonValue::string(kernel)},
        {"manufacturer", JsonValue::string(manufacturer)},
        {"memory_total_kib", JsonValue::integer(memoryTotalKib())},
        {"model", JsonValue::string(model)},
        {"os_release", JsonValue::string(release)},
        {"platform", JsonValue::string(platformName())},
        {"product", JsonValue::string(product)},
        {"selinux", JsonValue::string(selinuxMode())},
    });
}

std::string
errorResponse(const std::string& requestId, const std::string& code, const std::string& message)
{
    return serializeJson(JsonValue::object({
        {"error",
         JsonValue::object(
             {{"code", JsonValue::string(code)}, {"message", JsonValue::string(message)}})},
        {"ok", JsonValue::boolean(false)},
        {"protocol", JsonValue::integer(AgentProtocolVersion)},
        {"request_id", JsonValue::string(requestId)},
    }));
}

std::string resultResponse(const std::string& requestId, JsonValue result)
{
    return serializeJson(JsonValue::object({
        {"ok", JsonValue::boolean(true)},
        {"protocol", JsonValue::integer(AgentProtocolVersion)},
        {"request_id", JsonValue::string(requestId)},
        {"result", std::move(result)},
    }));
}

std::string jobIdText(std::uint64_t jobId)
{
    return "job-" + std::to_string(jobId);
}

bool parseJobId(const std::string& value, std::uint64_t& jobId)
{
    constexpr char Prefix[] = "job-";
    if (value.size() <= sizeof(Prefix) - 1 || value.compare(0, sizeof(Prefix) - 1, Prefix) != 0) {
        return false;
    }
    std::uint64_t parsed = 0;
    for (std::size_t index = sizeof(Prefix) - 1; index < value.size(); ++index) {
        const unsigned char character = static_cast<unsigned char>(value[index]);
        if (character < '0' || character > '9') {
            return false;
        }
        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            return false;
        }
        parsed = parsed * 10U + digit;
    }
    if (parsed == 0) {
        return false;
    }
    jobId = parsed;
    return true;
}

JsonValue boardTestSnapshotValue(const BoardTestJobSnapshot& snapshot)
{
    const bool terminal = snapshot.state == BoardTestJobState::succeeded ||
                          snapshot.state == BoardTestJobState::failed ||
                          snapshot.state == BoardTestJobState::cancelled ||
                          snapshot.state == BoardTestJobState::timed_out;
    JsonValue::Object result{
        {"capability_id", JsonValue::string(snapshot.capabilityId)},
        {"capability_version", JsonValue::integer(snapshot.capabilityVersion)},
        {"job_id", JsonValue::string(jobIdText(snapshot.jobId))},
        {"state", JsonValue::string(boardTestJobStateName(snapshot.state))},
    };
    if (terminal) {
        const std::string completion = snapshot.state == BoardTestJobState::succeeded ? "completed"
                                       : snapshot.state == BoardTestJobState::cancelled ? "cancelled"
                                       : snapshot.state == BoardTestJobState::timed_out ? "timed_out"
                                                                                       : "error";
        JsonValue provider = JsonValue::null();
        if (!snapshot.result.provider.kind.empty() && !snapshot.result.provider.id.empty() &&
            snapshot.result.provider.version > 0) {
            provider = JsonValue::object({
                {"id", JsonValue::string(snapshot.result.provider.id)},
                {"kind", JsonValue::string(snapshot.result.provider.kind)},
                {"version", JsonValue::integer(snapshot.result.provider.version)},
            });
        }
        JsonValue observations = JsonValue::null();
        if (!snapshot.result.observationSchema.empty()) {
            observations = JsonValue::object({
                {"data", snapshot.result.observations},
                {"schema", JsonValue::string(snapshot.result.observationSchema)},
            });
        }
        result["result"] = JsonValue::object({
            {"completion", JsonValue::string(completion)},
            {"exit_code", JsonValue::integer(snapshot.result.exitCode)},
            {"log", JsonValue::string(snapshot.result.log)},
            {"observations", std::move(observations)},
            {"provider", std::move(provider)},
            {"provider_claim",
             snapshot.result.providerClaim ? JsonValue::string(*snapshot.result.providerClaim)
                                           : JsonValue::null()},
            {"summary", JsonValue::string(snapshot.result.summary)},
        });
    } else {
        result["result"] = JsonValue::null();
    }
    return JsonValue::object(std::move(result));
}

} // namespace

AgentService::AgentService()
    : AgentService(
          [](const BoardTestRunRequest& request, const std::atomic_bool& cancelRequested) {
              return runBoardCapability(request, cancelRequested);
          },
          [] { return availableBoardCapabilities(); })
{
}

AgentService::AgentService(BoardTestJobManager::RunFunction boardTestRunner,
                           CapabilityListFunction capabilityList)
    : boardTestJobs_(std::move(boardTestRunner)), capabilityList_(std::move(capabilityList))
{
}

AgentService::~AgentService()
{
    stop();
}

std::string AgentService::handleRequest(const std::string& request)
{
    JsonLimits limits;
    limits.max_input_bytes = 1024U * 1024U;
    limits.max_depth = 16;
    limits.max_string_bytes = 64U * 1024U;
    limits.max_array_items = 256;
    limits.max_object_members = 256;
    JsonValue root;
    JsonError parseError;
    if (!parseJson(request, root, parseError, limits) || !root.isObject()) {
        return errorResponse({}, "invalid_request", "request must be valid bounded JSON object");
    }
    std::string requestId;
    std::string method;
    const JsonValue* params = member(root, "params");
    if (!stringMember(root, "request_id", requestId) || requestId.empty() ||
        requestId.size() > 128 || !stringMember(root, "method", method) || method.empty() ||
        method.size() > 64 || !params || !params->isObject() ||
        !hasOnlyMembers(root, {"protocol", "request_id", "method", "params"})) {
        return errorResponse(
            requestId, "invalid_request", "request_id, method and params object are required");
    }
    int protocol = 0;
    if (!intMember(root, "protocol", protocol)) {
        return errorResponse(requestId, "invalid_request", "protocol must be an integer");
    }
    if (protocol != AgentProtocolVersion) {
        return errorResponse(
            requestId, "unsupported_protocol", "only protocol version 1 is supported");
    }
    if (method == "session.hello") {
        if (!params->objectItems().empty()) {
            return errorResponse(requestId, "invalid_params", "session.hello takes no parameters");
        }
        return resultResponse(requestId,
                              JsonValue::object({
                                  {"agent", JsonValue::string("testtools-agent")},
                                  {"agent_version", JsonValue::string(AgentVersion)},
                                  {"platform", JsonValue::string(platformName())},
                                  {"protocol_max", JsonValue::integer(AgentProtocolVersion)},
                                  {"protocol_min", JsonValue::integer(AgentProtocolVersion)},
                              }));
    }
    if (method == "capability.list") {
        if (!params->objectItems().empty()) {
            return errorResponse(
                requestId, "invalid_params", "capability.list takes no parameters");
        }
        JsonValue::Array capabilities{
            JsonValue::object({
                {"id", JsonValue::string("system.info")},
                {"name", JsonValue::string("System information")},
                {"risks", JsonValue::array({JsonValue::string("read_only")})},
                {"supports_cancel", JsonValue::boolean(false)},
                {"version", JsonValue::integer(1)},
            }),
            JsonValue::object({
                {"id", JsonValue::string("serial.stream")},
                {"name", JsonValue::string("Managed serial stream")},
                {"risks", JsonValue::array({JsonValue::string("device_io")})},
                {"supports_cancel", JsonValue::boolean(true)},
                {"version", JsonValue::integer(1)},
            }),
        };
        {
            for (const auto& definition : validatedCapabilities(capabilityList_)) {
                JsonValue::Array risks;
                JsonValue::Array resources;
                for (const auto& risk : definition.risks)
                    risks.emplace_back(risk);
                for (const auto& resource : definition.resources)
                    resources.emplace_back(resource);
                capabilities.push_back(JsonValue::object({
                    {"default_timeout_ms", JsonValue::integer(definition.defaultTimeoutMs)},
                    {"id", JsonValue::string(definition.id)},
                    {"name", JsonValue::string(definition.name)},
                    {"observation_schema", JsonValue::string(definition.observationSchema)},
                    {"provider",
                     JsonValue::object({
                         {"id", JsonValue::string(definition.provider.id)},
                         {"kind", JsonValue::string(definition.provider.kind)},
                         {"version", JsonValue::integer(definition.provider.version)},
                     })},
                    {"recovery", JsonValue::string(definition.recovery)},
                    {"resources", JsonValue::array(std::move(resources))},
                    {"risks", JsonValue::array(std::move(risks))},
                    {"supports_cancel", JsonValue::boolean(definition.supportsCancel)},
                    {"version", JsonValue::integer(definition.version)},
                }));
            }
        }
        return resultResponse(
            requestId,
            JsonValue::object({{"capabilities", JsonValue::array(std::move(capabilities))}}));
    }
    if (method == "system.info") {
        if (!params->objectItems().empty()) {
            return errorResponse(requestId, "invalid_params", "system.info takes no parameters");
        }
        return resultResponse(requestId, systemInfoValue());
    }
    if (method == "job.start") {
        return boardTestStartResponse(requestId, *params);
    }
    if (method == "job.status") {
        return boardTestStatusResponse(requestId, *params);
    }
    if (method == "job.cancel") {
        return boardTestCancelResponse(requestId, *params);
    }
    if (method == "serial.list") {
        if (!params->objectItems().empty()) {
            return errorResponse(requestId, "invalid_params", "serial.list takes no parameters");
        }
        return serialListResponse(requestId);
    }
    if (method == "serial.open") {
        return serialOpenResponse(requestId, *params);
    }
    if (method == "serial.close") {
        return serialCloseResponse(requestId, *params);
    }
    return errorResponse(requestId, "method_not_found", "method is not available");
}

std::string AgentService::boardTestStartResponse(const std::string& requestId,
                                                 const JsonValue& params)
{
    std::string capabilityId;
    if (!hasOnlyMembers(params, {"capability_id"}) ||
        !stringMember(params, "capability_id", capabilityId) || !validIdentifier(capabilityId)) {
        return errorResponse(requestId, "invalid_params", "capability_id is invalid");
    }
    std::optional<BoardCapabilityDefinition> selected;
    {
        for (const auto& definition : validatedCapabilities(capabilityList_)) {
            if (definition.id == capabilityId) {
                selected = definition;
                break;
            }
        }
    }
    if (!selected) {
        return errorResponse(
            requestId, "capability_unavailable", "the requested capability is unavailable");
    }
    const auto snapshot = boardTestJobs_.start({capabilityId, selected->version});
    if (!snapshot) {
        return errorResponse(
            requestId, "resource_busy", "another board test job is already active");
    }
    return resultResponse(requestId, boardTestSnapshotValue(*snapshot));
}

std::string AgentService::boardTestStatusResponse(const std::string& requestId,
                                                  const JsonValue& params) const
{
    std::string jobIdTextValue;
    std::uint64_t jobId = 0;
    if (!hasOnlyMembers(params, {"job_id"}) || !stringMember(params, "job_id", jobIdTextValue) ||
        !parseJobId(jobIdTextValue, jobId)) {
        return errorResponse(requestId, "invalid_params", "job_id must use the job-N form");
    }
    const auto snapshot = boardTestJobs_.snapshot(jobId);
    if (!snapshot) {
        return errorResponse(requestId, "job_not_found", "board test job does not exist");
    }
    return resultResponse(requestId, boardTestSnapshotValue(*snapshot));
}

std::string AgentService::boardTestCancelResponse(const std::string& requestId,
                                                  const JsonValue& params)
{
    std::string jobIdTextValue;
    std::uint64_t jobId = 0;
    if (!hasOnlyMembers(params, {"job_id"}) || !stringMember(params, "job_id", jobIdTextValue) ||
        !parseJobId(jobIdTextValue, jobId)) {
        return errorResponse(requestId, "invalid_params", "job_id must use the job-N form");
    }
    const auto snapshot = boardTestJobs_.cancel(jobId);
    if (!snapshot) {
        return errorResponse(requestId, "job_not_active", "board test job is not active");
    }
    return resultResponse(requestId, boardTestSnapshotValue(*snapshot));
}

std::string AgentService::serialListResponse(const std::string& requestId) const
{
    const auto endpoints = listSerialEndpoints();
    JsonValue::Array values;
    values.reserve(endpoints.size());
    for (const auto& endpoint : endpoints) {
        values.push_back(JsonValue::object({
            {"endpoint", JsonValue::string(endpoint.endpoint)},
            {"interface", JsonValue::string(endpoint.interfaceName)},
            {"occupied_by", JsonValue::string(endpoint.occupiedBy)},
            {"readable", JsonValue::boolean(endpoint.readable)},
            {"writable", JsonValue::boolean(endpoint.writable)},
        }));
    }
    return resultResponse(requestId,
                          JsonValue::object({{"endpoints", JsonValue::array(std::move(values))}}));
}

std::string AgentService::serialOpenResponse(const std::string& requestId, const JsonValue& params)
{
    testtools::SerialConfig config;
    if (!hasOnlyMembers(
            params,
            {"endpoint", "baud_rate", "data_bits", "stop_bits", "parity", "flow_control"}) ||
        !stringMember(params, "endpoint", config.endpoint)) {
        return errorResponse(requestId, "invalid_params", "serial parameters are invalid");
    }
    int stopBits = 1;
    if (config.endpoint.empty()) {
        return errorResponse(requestId, "invalid_params", "endpoint is required");
    }
    if (!optionalIntMember(params, "baud_rate", 115200, config.baudRate) ||
        !optionalIntMember(params, "data_bits", 8, config.dataBits) ||
        !optionalIntMember(params, "stop_bits", 1, stopBits)) {
        return errorResponse(
            requestId, "invalid_params", "baud_rate, data_bits and stop_bits must be integers");
    }
    if (config.baudRate <= 0 || config.dataBits < 5 || config.dataBits > 8 ||
        (stopBits != 1 && stopBits != 2)) {
        return errorResponse(
            requestId, "invalid_params", "serial framing parameters are out of range");
    }
    std::string parity;
    if (member(params, "parity") && !stringMember(params, "parity", parity)) {
        return errorResponse(requestId, "invalid_params", "parity must be a string");
    }
    if (!parity.empty() && parity != "none" && parity != "odd" && parity != "even") {
        return errorResponse(requestId, "invalid_params", "unsupported parity value");
    }
    config.parity = parity == "odd"    ? testtools::Parity::Odd
                    : parity == "even" ? testtools::Parity::Even
                                       : testtools::Parity::None;
    config.stopBits = stopBits == 2 ? testtools::StopBits::Two : testtools::StopBits::One;
    std::string flow;
    if (member(params, "flow_control") && !stringMember(params, "flow_control", flow)) {
        return errorResponse(requestId, "invalid_params", "flow_control must be a string");
    }
    if (!flow.empty() && flow != "none" && flow != "hardware" && flow != "software") {
        return errorResponse(requestId, "invalid_params", "unsupported flow_control value");
    }
    config.flowControl = flow == "hardware"   ? testtools::FlowControl::Hardware
                         : flow == "software" ? testtools::FlowControl::Software
                                              : testtools::FlowControl::None;

    std::lock_guard<std::mutex> lock(serialMutex_);
    if (serialBridge_.isRunning()) {
        return errorResponse(
            requestId, "resource_busy", "another serial session is already active");
    }
    std::uint16_t port = 0;
    std::string sessionId;
    int effectiveBaudRate = 0;
    bool baudRateApplied = false;
    std::string error;
    if (!serialBridge_.start(config, port, sessionId, effectiveBaudRate, baudRateApplied, error)) {
        return errorResponse(requestId, "serial_open_failed", error);
    }
    activeSerialSessionId_ = sessionId;
    return resultResponse(requestId,
                          JsonValue::object({
                              {"baud_rate_applied", JsonValue::boolean(baudRateApplied)},
                              {"baud_rate_effective", JsonValue::integer(effectiveBaudRate)},
                              {"baud_rate_requested", JsonValue::integer(config.baudRate)},
                              {"endpoint", JsonValue::string(config.endpoint)},
                              {"session_id", JsonValue::string(sessionId)},
                              {"stream_port", JsonValue::integer(port)},
                          }));
}

std::string AgentService::serialCloseResponse(const std::string& requestId, const JsonValue& params)
{
    std::lock_guard<std::mutex> lock(serialMutex_);
    std::string sessionId;
    if (!hasOnlyMembers(params, {"session_id"}) || !stringMember(params, "session_id", sessionId)) {
        return errorResponse(requestId, "invalid_params", "session_id must be a string");
    }
    if (activeSerialSessionId_.empty() || sessionId != activeSerialSessionId_) {
        return errorResponse(
            requestId, "session_not_found", "serial session_id is missing or no longer active");
    }
    serialBridge_.stop();
    activeSerialSessionId_.clear();
    return resultResponse(requestId, JsonValue::object({{"closed", JsonValue::boolean(true)}}));
}

void AgentService::stop()
{
    boardTestJobs_.stop();
    std::lock_guard<std::mutex> lock(serialMutex_);
    serialBridge_.stop();
    activeSerialSessionId_.clear();
}

} // namespace testtools::agent
