#pragma once

#include "board_capability.h"
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>

namespace testtools::agent {
std::int64_t toolClockMs();

struct ToolContext {
    mutable std::mutex mutex;
    std::string owner, operation, instance, capability, argumentsText;
    JsonValue arguments = JsonValue::object();
    std::uint64_t jobId = 0, eventSequence = 0;
    std::int64_t issuedAt = 0;
    std::atomic<std::int64_t> leaseUntil{0}, deadline{0};
    bool started = false, finished = false, complete = true, abandoned = false;
    std::string stage = "preparing", detail;
    JsonValue data = JsonValue::object();
    std::deque<JsonValue> events;
    BoardTestJobSnapshot terminal;
    std::function<BoardTestRunResult(const BoardTestRunRequest&, const std::atomic_bool&)> execute;

    bool expired() const;
    void update(const std::string& phase, JsonValue values = JsonValue::object(), const std::string& message = {});
    bool appendEvent(JsonValue event);
    void finish(const BoardTestJobSnapshot& result);
};

struct ToolReply {
    bool ok = false;
    JsonValue result = JsonValue::object();
    std::string code, message;
};

class IToolboxPlatform {
  public:
    virtual ~IToolboxPlatform() = default;
    virtual JsonValue gpioInventory(const std::string& chip, std::int64_t cursor, int limit) = 0;
    virtual JsonValue boardGpioInventory() { return JsonValue::object({{"schema", JsonValue::string("gpio.board.inventory/v1")}, {"endpoints", JsonValue::array()}}); }
    virtual JsonValue networkInventory() = 0;
    virtual bool hasRunner() const = 0;
    virtual BoardTestRunResult run(const BoardTestRunRequest&, const std::atomic_bool&) = 0;
};
std::shared_ptr<IToolboxPlatform> createToolboxPlatform();

class ToolboxAgent {
  public:
    explicit ToolboxAgent(BoardTestJobManager& jobs, std::shared_ptr<IToolboxPlatform> platform = {});
    bool handles(const std::string& method, const JsonValue& params) const;
    ToolReply handle(const std::string& method, const JsonValue& params);
    std::vector<BoardCapabilityDefinition> capabilities() const;
    const std::string& instance() const { return instance_; }
  private:
    JsonValue progress(const std::shared_ptr<ToolContext>& context, std::uint64_t cursor) const;
    BoardTestJobManager& jobs_;
    std::shared_ptr<IToolboxPlatform> platform_;
    std::string instance_;
    mutable std::mutex mutex_;
    std::deque<std::shared_ptr<ToolContext>> tickets_;
    std::uint64_t nextOperation_ = 1;
};
} // namespace testtools::agent
