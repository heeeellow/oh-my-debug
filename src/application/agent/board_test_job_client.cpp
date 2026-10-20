#include "application/agent/board_test_job_client.h"

#include "core/protocol/agent_version.h"

#include <algorithm>
#include <initializer_list>
#include <limits>
#include <utility>

namespace testtools {
namespace {

constexpr std::size_t MaximumResponseBytes = 1024U * 1024U;
constexpr std::size_t MaximumSummaryBytes = 1024;
constexpr std::size_t MaximumLogBytes = 64U * 1024U;
constexpr std::size_t MaximumIdentifierBytes = 128;

const JsonValue* member(const JsonValue& object, const char* key)
{
    return object.isObject() ? object.find(key) : nullptr;
}

bool hasOnlyMembers(const JsonValue& object, std::initializer_list<const char*> allowed)
{
    if (!object.isObject())
        return false;
    for (const auto& item : object.objectItems()) {
        const bool found = std::any_of(
            allowed.begin(), allowed.end(), [&](const char* name) { return item.first == name; });
        if (!found)
            return false;
    }
    return true;
}

bool stringMember(const JsonValue& object, const char* key, std::string& output)
{
    const auto* value = member(object, key);
    if (!value || !value->isString())
        return false;
    output = value->stringValue();
    return true;
}

bool integerMember(const JsonValue& object, const char* key, int& output)
{
    const auto* value = member(object, key);
    if (!value || !value->isInt64() || value->int64Value() < std::numeric_limits<int>::min() ||
        value->int64Value() > std::numeric_limits<int>::max()) {
        return false;
    }
    output = static_cast<int>(value->int64Value());
    return true;
}

bool validIdentifier(const std::string& value)
{
    if (value.empty() || value.size() > MaximumIdentifierBytes || value == "." || value == "..") {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '_' || character == '-' ||
               character == '.' || character == ':';
    });
}

bool validSchemaId(const std::string& value)
{
    if (value.empty() || value.size() > MaximumIdentifierBytes || value.front() == '/' ||
        value.back() == '/') {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '_' || character == '-' ||
               character == '.' || character == '/' || character == ':';
    });
}

bool validJobId(const std::string& value)
{
    if (value.size() < 5 || value.size() > 32 || value.compare(0, 4, "job-") != 0 ||
        value[4] == '0') {
        return false;
    }
    std::uint64_t parsed = 0;
    for (std::size_t index = 4; index < value.size(); ++index) {
        if (value[index] < '0' || value[index] > '9')
            return false;
        const auto digit = static_cast<std::uint64_t>(value[index] - '0');
        if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U)
            return false;
        parsed = parsed * 10U + digit;
    }
    return parsed != 0;
}

bool parseState(const std::string& value, BoardTestJobState& state)
{
    if (value == "running")
        state = BoardTestJobState::Running;
    else if (value == "cancelling")
        state = BoardTestJobState::Cancelling;
    else if (value == "succeeded")
        state = BoardTestJobState::Succeeded;
    else if (value == "failed")
        state = BoardTestJobState::Failed;
    else if (value == "cancelled")
        state = BoardTestJobState::Cancelled;
    else if (value == "timed_out")
        state = BoardTestJobState::TimedOut;
    else
        return false;
    return true;
}

bool parseCompletion(const std::string& value, BoardTestCompletion& completion)
{
    if (value == "completed")
        completion = BoardTestCompletion::Completed;
    else if (value == "error")
        completion = BoardTestCompletion::Error;
    else if (value == "cancelled")
        completion = BoardTestCompletion::Cancelled;
    else if (value == "timed_out")
        completion = BoardTestCompletion::TimedOut;
    else
        return false;
    return true;
}

bool completionMatchesState(BoardTestCompletion completion, BoardTestJobState state)
{
    return (completion == BoardTestCompletion::Completed &&
            state == BoardTestJobState::Succeeded) ||
           (completion == BoardTestCompletion::Error && state == BoardTestJobState::Failed) ||
           (completion == BoardTestCompletion::Cancelled &&
            state == BoardTestJobState::Cancelled) ||
           (completion == BoardTestCompletion::TimedOut && state == BoardTestJobState::TimedOut);
}

bool parseProvider(const JsonValue& value,
                   std::optional<BoardTestProviderIdentity>& provider,
                   std::string& error)
{
    if (value.isNull()) {
        provider.reset();
        return true;
    }
    BoardTestProviderIdentity parsed;
    if (!hasOnlyMembers(value, {"kind", "id", "version"}) ||
        !stringMember(value, "kind", parsed.kind) || !validIdentifier(parsed.kind) ||
        !stringMember(value, "id", parsed.id) || !validIdentifier(parsed.id) ||
        !integerMember(value, "version", parsed.version) || parsed.version <= 0) {
        error = "job result has an invalid provider identity";
        return false;
    }
    provider = std::move(parsed);
    return true;
}

bool parseObservations(const JsonValue& value, BoardTestResult& result, std::string& error)
{
    if (value.isNull()) {
        result.observationSchema.clear();
        result.observations = JsonValue::object();
        return true;
    }
    std::string schema;
    const auto* data = member(value, "data");
    if (!hasOnlyMembers(value, {"schema", "data"}) || !stringMember(value, "schema", schema) ||
        !validSchemaId(schema) || !data || !data->isObject()) {
        error = "job result has invalid observations";
        return false;
    }
    result.observationSchema = std::move(schema);
    result.observations = *data;
    return true;
}

bool parseTerminalResult(const JsonValue& value,
                         BoardTestJobState state,
                         BoardTestResult& result,
                         std::string& error)
{
    if (!hasOnlyMembers(value,
                        {"completion",
                         "exit_code",
                         "provider",
                         "observations",
                         "provider_claim",
                         "summary",
                         "log"})) {
        error = "job result contains unsupported fields";
        return false;
    }
    std::string completion;
    if (!stringMember(value, "completion", completion) ||
        !parseCompletion(completion, result.completion) ||
        !completionMatchesState(result.completion, state) ||
        !integerMember(value, "exit_code", result.exitCode) ||
        !stringMember(value, "summary", result.summary) ||
        !stringMember(value, "log", result.log)) {
        error = "job result has invalid or inconsistent terminal fields";
        return false;
    }
    if (result.summary.size() > MaximumSummaryBytes || result.log.size() > MaximumLogBytes) {
        error = "job result exceeds summary or log limits";
        return false;
    }
    const auto* provider = member(value, "provider");
    const auto* observations = member(value, "observations");
    const auto* claim = member(value, "provider_claim");
    if (!provider || !parseProvider(*provider, result.provider, error) || !observations ||
        !parseObservations(*observations, result, error) || !claim) {
        if (error.empty())
            error = "job result is missing structured result fields";
        return false;
    }
    if (claim->isNull()) {
        result.providerClaim.reset();
    } else if (claim->isString() && claim->stringValue().size() <= MaximumSummaryBytes) {
        result.providerClaim = claim->stringValue();
    } else {
        error = "job result has an invalid provider_claim";
        return false;
    }
    if (result.completion == BoardTestCompletion::Completed &&
        (!result.provider || result.observationSchema.empty())) {
        error = "completed job result must identify its provider and observation schema";
        return false;
    }
    return true;
}

bool parseSnapshot(const JsonValue& value, BoardTestJobSnapshot& snapshot, std::string& error)
{
    if (!hasOnlyMembers(value,
                        {"job_id", "capability_id", "capability_version", "state", "result"})) {
        error = "job snapshot contains unsupported fields";
        return false;
    }
    std::string state;
    if (!stringMember(value, "job_id", snapshot.jobId) || !validJobId(snapshot.jobId) ||
        !stringMember(value, "capability_id", snapshot.capabilityId) ||
        !validIdentifier(snapshot.capabilityId) ||
        !integerMember(value, "capability_version", snapshot.capabilityVersion) ||
        snapshot.capabilityVersion <= 0 || !stringMember(value, "state", state) ||
        !parseState(state, snapshot.state)) {
        error = "job snapshot has invalid identity or state fields";
        return false;
    }
    const auto* resultValue = member(value, "result");
    if (!resultValue) {
        error = "job snapshot is missing result";
        return false;
    }
    const bool terminal = snapshot.state == BoardTestJobState::Succeeded ||
                          snapshot.state == BoardTestJobState::Failed ||
                          snapshot.state == BoardTestJobState::Cancelled ||
                          snapshot.state == BoardTestJobState::TimedOut;
    if (!terminal) {
        if (!resultValue->isNull()) {
            error = "active job result must be null";
            return false;
        }
        snapshot.result.reset();
        return true;
    }
    if (!resultValue->isObject()) {
        error = "terminal job result must be an object";
        return false;
    }
    BoardTestResult result;
    if (!parseTerminalResult(*resultValue, snapshot.state, result, error))
        return false;
    snapshot.result = std::move(result);
    return true;
}

BoardTestJobError parseEnvelope(const std::string& response,
                                const std::string& expectedRequestId,
                                BoardTestJobSnapshot& snapshot)
{
    JsonLimits limits;
    limits.max_input_bytes = MaximumResponseBytes;
    limits.max_depth = 32;
    limits.max_string_bytes = MaximumLogBytes;
    limits.max_array_items = 256;
    limits.max_object_members = 256;
    JsonValue root;
    JsonError jsonError;
    if (!parseJson(response, root, jsonError, limits) || !root.isObject()) {
        return {"invalid_response",
                jsonError.message.empty() ? "Agent response must be a valid bounded JSON object"
                                          : jsonError.message};
    }
    int protocol = 0;
    std::string requestId;
    const auto* okValue = member(root, "ok");
    if (!integerMember(root, "protocol", protocol) || protocol != AgentProtocolVersion ||
        !stringMember(root, "request_id", requestId) || requestId != expectedRequestId ||
        !okValue || !okValue->isBool()) {
        return {"invalid_response", "Agent response envelope is invalid"};
    }
    if (!okValue->boolValue()) {
        if (!hasOnlyMembers(root, {"protocol", "request_id", "ok", "error"})) {
            return {"invalid_response", "Agent error response contains unsupported fields"};
        }
        const auto* errorValue = member(root, "error");
        std::string code;
        std::string message;
        if (!errorValue || !hasOnlyMembers(*errorValue, {"code", "message"}) ||
            !stringMember(*errorValue, "code", code) || !validIdentifier(code) ||
            !stringMember(*errorValue, "message", message) ||
            message.size() > MaximumSummaryBytes) {
            return {"invalid_response", "Agent error response is invalid"};
        }
        return {std::move(code), std::move(message)};
    }
    if (!hasOnlyMembers(root, {"protocol", "request_id", "ok", "result"})) {
        return {"invalid_response", "Agent success response contains unsupported fields"};
    }
    const auto* result = member(root, "result");
    if (!result)
        return {"invalid_response", "Agent response is missing result"};
    std::string error;
    if (!parseSnapshot(*result, snapshot, error))
        return {"invalid_response", std::move(error)};
    return {};
}

} // namespace

const char* boardTestJobStateName(BoardTestJobState state) noexcept
{
    switch (state) {
    case BoardTestJobState::Running:
        return "running";
    case BoardTestJobState::Cancelling:
        return "cancelling";
    case BoardTestJobState::Succeeded:
        return "succeeded";
    case BoardTestJobState::Failed:
        return "failed";
    case BoardTestJobState::Cancelled:
        return "cancelled";
    case BoardTestJobState::TimedOut:
        return "timed_out";
    }
    return "failed";
}

BoardTestJobClient::BoardTestJobClient(std::shared_ptr<IAgentConnection> connection)
    : connection_(std::move(connection))
{
}

BoardTestJobCall BoardTestJobClient::start(const std::string& capabilityId)
{
    if (!validIdentifier(capabilityId)) {
        return {false, {}, {"invalid_params", "capability_id is invalid"}};
    }
    return call("job.start", "capability_id", capabilityId);
}

BoardTestJobCall BoardTestJobClient::status(const std::string& jobId)
{
    if (!validJobId(jobId))
        return {false, {}, {"invalid_params", "job_id is invalid"}};
    return call("job.status", "job_id", jobId);
}

BoardTestJobCall BoardTestJobClient::cancel(const std::string& jobId)
{
    if (!validJobId(jobId))
        return {false, {}, {"invalid_params", "job_id is invalid"}};
    return call("job.cancel", "job_id", jobId);
}

BoardTestJobCall BoardTestJobClient::call(const char* method,
                                          const std::string& parameterName,
                                          const std::string& parameterValue)
{
    if (!connection_) {
        return {false, {}, {"not_connected", "Agent connection is not available"}};
    }
    const std::string requestId = "host-job-" + std::to_string(nextRequestId_++);
    const std::string request = serializeJson(JsonValue::object({
        {"method", JsonValue::string(method)},
        {"params", JsonValue::object({{parameterName, JsonValue::string(parameterValue)}})},
        {"protocol", JsonValue::integer(AgentProtocolVersion)},
        {"request_id", JsonValue::string(requestId)},
    }));
    if (request.empty()) {
        return {false, {}, {"invalid_request", "Agent request could not be serialized"}};
    }
    std::string response;
    std::string transportError;
    if (!connection_->request(request, response, transportError)) {
        return {
            false,
            {},
            {"transport_error", transportError.empty() ? "Agent request failed" : transportError}};
    }
    BoardTestJobSnapshot snapshot;
    const auto error = parseEnvelope(response, requestId, snapshot);
    if (error)
        return {false, {}, error};
    return {true, std::move(snapshot), {}};
}

} // namespace testtools
