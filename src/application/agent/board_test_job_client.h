#pragma once

#include "core/protocol/agent_connection.h"
#include "core/protocol/json_value.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace testtools {

enum class BoardTestJobState {
    Running,
    Cancelling,
    Succeeded,
    Failed,
    Cancelled,
    TimedOut,
};

const char* boardTestJobStateName(BoardTestJobState state) noexcept;

enum class BoardTestCompletion {
    Completed,
    Error,
    Cancelled,
    TimedOut,
};

struct BoardTestProviderIdentity {
    std::string kind;
    std::string id;
    int version = 0;
};

struct BoardTestResult {
    BoardTestCompletion completion = BoardTestCompletion::Error;
    int exitCode = -1;
    std::optional<BoardTestProviderIdentity> provider;
    std::string observationSchema;
    JsonValue observations = JsonValue::object();
    std::optional<std::string> providerClaim;
    std::string summary;
    std::string log;
};

struct BoardTestJobSnapshot {
    std::string jobId;
    std::string capabilityId;
    int capabilityVersion = 0;
    BoardTestJobState state = BoardTestJobState::Failed;
    // A running/cancelling job has no result. Terminal jobs must have one.
    std::optional<BoardTestResult> result;
};

struct BoardTestJobError {
    std::string code;
    std::string message;

    explicit operator bool() const noexcept
    {
        return !code.empty() || !message.empty();
    }
};

struct BoardTestJobCall {
    bool ok = false;
    BoardTestJobSnapshot snapshot;
    BoardTestJobError error;
};

class BoardTestJobClient {
  public:
    explicit BoardTestJobClient(std::shared_ptr<IAgentConnection> connection);

    BoardTestJobClient(const BoardTestJobClient&) = delete;
    BoardTestJobClient& operator=(const BoardTestJobClient&) = delete;

    // These calls are synchronous at the transport boundary. Application/UI
    // callers should invoke them from their existing background task owner.
    BoardTestJobCall start(const std::string& capabilityId);
    BoardTestJobCall status(const std::string& jobId);
    BoardTestJobCall cancel(const std::string& jobId);

  private:
    BoardTestJobCall
    call(const char* method, const std::string& parameterName, const std::string& parameterValue);

    std::shared_ptr<IAgentConnection> connection_;
    std::uint64_t nextRequestId_ = 1;
};

} // namespace testtools
