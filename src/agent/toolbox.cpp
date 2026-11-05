#include "toolbox.h"
#include "core/toolbox/toolbox_contract.h"
#include <algorithm>
#include <chrono>
#include <stdexcept>
#if defined(__linux__)
#include <time.h>
#endif

namespace testtools::agent {
namespace {
ToolReply failure(const std::string& code, const std::string& message)
{
    ToolReply reply;
    reply.code = code; reply.message = message;
    return reply;
}
ToolReply success(JsonValue result)
{
    ToolReply reply; reply.ok = true; reply.result = std::move(result); return reply;
}
std::string stateName(BoardTestJobState state) { return boardTestJobStateName(state); }
bool active(BoardTestJobState state)
{
    return state == BoardTestJobState::running || state == BoardTestJobState::cancelling;
}
}
std::int64_t toolClockMs()
{
#if defined(__linux__)
    timespec value{};
    if (::clock_gettime(CLOCK_BOOTTIME, &value) == 0)
        return static_cast<std::int64_t>(value.tv_sec) * 1000 + value.tv_nsec / 1000000;
#endif
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
bool ToolContext::expired() const
{
    const auto now = toolClockMs();
    return now >= leaseUntil.load() || now >= deadline.load();
}
void ToolContext::update(const std::string& phase, JsonValue values, const std::string& message)
{
    std::lock_guard<std::mutex> lock(mutex);
    stage = phase; data = std::move(values); detail = message;
}
bool ToolContext::appendEvent(JsonValue event)
{
    std::lock_guard<std::mutex> lock(mutex);
    // 消费由带游标的确认完成；满队列停止，不覆盖尚未传送的观测。
    if (events.size() >= 4096) { complete = false; return false; }
    event.objectItems()["sequence"] = JsonValue::integer(++eventSequence);
    events.push_back(std::move(event));
    return true;
}
void ToolContext::finish(const BoardTestJobSnapshot& value)
{
    std::lock_guard<std::mutex> lock(mutex);
    terminal = value; finished = true;
    stage = value.state == BoardTestJobState::succeeded ? "completed" : stateName(value.state);
}
ToolboxAgent::ToolboxAgent(BoardTestJobManager& jobs, std::shared_ptr<IToolboxPlatform> platform)
    : jobs_(jobs), platform_(platform ? std::move(platform) : createToolboxPlatform()), instance_(makeToolToken())
{
}
bool ToolboxAgent::handles(const std::string& method, const JsonValue& params) const
{
    if (method.rfind("toolbox.", 0) == 0 || method == "gpio.chip.list" || method == "gpio.board.list" ||
        method == "network.interface.list" || method == "job.progress" ||
        method == "job.renew" || method == "gpio.hold.extend") return true;
    if (method == "job.start") return isToolCapability(jsonString(params, "capability_id"));
    if (method != "job.cancel") return false;
    if (params.find("owner_id") || params.find("instance_id")) return true;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto id = jsonString(params, "job_id");
    return std::any_of(tickets_.begin(), tickets_.end(), [&](const auto& ticket) {
        return ticket->started && "job-" + std::to_string(ticket->jobId) == id;
    });
}
std::vector<BoardCapabilityDefinition> ToolboxAgent::capabilities() const
{
    std::vector<BoardCapabilityDefinition> result;
    for (const std::string id : {"toolbox.v1", "gpio.chip.list", "gpio.board.list", "gpio.board.read", "gpio.board.set", "network.interface.list", "gpio.pin.read",
                                "gpio.pin.write", "gpio.pin.pulse", "gpio.edge.monitor", "network.iperf3.server"}) {
        if (id == "network.iperf3.server" && !platform_->hasRunner()) continue;
        BoardCapabilityDefinition definition;
        definition.id = id; definition.name = id; definition.observationSchema = id + "/v1";
        definition.provider = {id.rfind("gpio.board.", 0) == 0 ? "sdk_bsp" : id == "network.iperf3.server" ? "runner_pack" : "agent_native", "toolbox_native", 1};
        definition.risks = {id == "network.iperf3.server" ? "network_load" :
            (id == "gpio.pin.write" || id == "gpio.pin.pulse" || id == "gpio.edge.monitor" || id == "gpio.board.set") ? "hardware_control" : "read_only"};
        definition.resources = {isToolCapability(id) ? "toolbox.task" : "toolbox.inventory"};
        definition.supportsCancel = isToolCapability(id); definition.defaultTimeoutMs = 320000;
        definition.recovery = id == "gpio.board.set" ? "Board GPIO settings persist after task completion; no automatic restoration." :
            "Lease expiry closes owned handles and processes; electrical state after release is not guaranteed.";
        result.push_back(std::move(definition));
    }
    return result;
}
JsonValue ToolboxAgent::progress(const std::shared_ptr<ToolContext>& context, std::uint64_t cursor) const
{
    const auto current = jobs_.snapshot(context->jobId);
    std::lock_guard<std::mutex> lock(context->mutex);
    const auto state = current ? *current : context->terminal;
    const bool running = context->started && !context->finished && (!current || active(current->state));
    JsonValue::Array events;
    bool gap = false;
    if (!context->events.empty()) {
        const auto first = jsonInteger(context->events.front(), "sequence");
        gap = cursor + 1 < static_cast<std::uint64_t>(first);
    }
    std::uint64_t next = cursor;
    for (const auto& event : context->events) {
        const auto sequence = static_cast<std::uint64_t>(jsonInteger(event, "sequence"));
        if (sequence > cursor && events.size() < 256) { events.push_back(event); next = sequence; }
    }
    // 已确认游标仅释放更旧数据，不影响本批及重复查询的可重放性。
    while (!context->events.empty() &&
           static_cast<std::uint64_t>(jsonInteger(context->events.front(), "sequence")) < cursor)
        context->events.pop_front();
    JsonValue terminal = JsonValue::null();
    if (context->finished || (current && !active(current->state))) {
        terminal = JsonValue::object({
            {"summary", JsonValue::string(state.result.summary)}, {"log", JsonValue::string(state.result.log)},
            {"exit_code", JsonValue::integer(state.result.exitCode)},
            {"schema", JsonValue::string(state.result.observationSchema)}, {"observations", state.result.observations}});
    }
    return JsonValue::object({
        {"schema", JsonValue::string("toolbox.job/v1")}, {"instance_id", JsonValue::string(instance_)},
        {"operation_id", JsonValue::string(context->operation)},
        {"job_id", JsonValue::string(context->started ? "job-" + std::to_string(context->jobId) : "")},
        {"capability_id", JsonValue::string(context->capability)},
        {"state", JsonValue::string(context->started ? stateName(state.state) : context->abandoned ? "aborted" : "prepared")},
        {"active", JsonValue::boolean(running)}, {"complete", JsonValue::boolean(context->complete && !gap)},
        {"stage", JsonValue::string(context->stage)}, {"detail", JsonValue::string(context->detail)},
        {"data", context->data}, {"events", JsonValue::array(std::move(events))},
        {"next_cursor", JsonValue::integer(static_cast<std::int64_t>(next))}, {"gap", JsonValue::boolean(gap)},
        {"remaining_ms", JsonValue::integer(std::max<std::int64_t>(0, context->deadline.load() - toolClockMs()))},
        {"result", std::move(terminal)}});
}
ToolReply ToolboxAgent::handle(const std::string& method, const JsonValue& params)
{
    if (method == "toolbox.info") {
        if (!onlyFields(params, {})) return failure("invalid_params", "toolbox.info takes no arguments");
        return success(JsonValue::object({{"schema", JsonValue::string("toolbox.info/v1")},
            {"instance_id", JsonValue::string(instance_)}, {"lease_ms", JsonValue::integer(5000)},
            {"runner_available", JsonValue::boolean(platform_->hasRunner())}}));
    }
    try {
        if (method == "gpio.board.list") {
            if (!onlyFields(params, {})) return failure("invalid_params", "gpio.board.list takes no arguments");
            return success(platform_->boardGpioInventory());
        }
        if (method == "gpio.chip.list") {
            if (!onlyFields(params, {"chip", "cursor", "limit"}) ||
                (params.find("chip") && !params.find("chip")->isString()) ||
                (params.find("cursor") && !params.find("cursor")->isInt64()) ||
                (params.find("limit") && !params.find("limit")->isInt64()) ||
                jsonInteger(params, "cursor") < 0 || jsonInteger(params, "cursor") > 65536 ||
                jsonInteger(params, "limit", 128) < 1 || jsonInteger(params, "limit", 128) > 256)
                return failure("invalid_params", "invalid GPIO inventory page");
            return success(platform_->gpioInventory(jsonString(params, "chip"), jsonInteger(params, "cursor"),
                static_cast<int>(jsonInteger(params, "limit", 128))));
        }
        if (method == "network.interface.list") {
            if (!onlyFields(params, {})) return failure("invalid_params", "network.interface.list takes no arguments");
            return success(platform_->networkInventory());
        }
    } catch (const std::exception& error) { return failure("inventory_failed", error.what()); }
    const auto owner = jsonString(params, "owner_id");
    if (!validToolToken(owner) || jsonString(params, "instance_id") != instance_)
        return failure("stale_owner", "tool owner or Agent instance does not match");
    std::lock_guard<std::mutex> lock(mutex_);
    if (method == "toolbox.prepare") {
        if (!onlyFields(params, {"owner_id", "instance_id"})) return failure("invalid_params", "unexpected prepare field");
        while (tickets_.size() >= 32) {
            const auto old = std::find_if(tickets_.begin(), tickets_.end(), [](const auto& item) {
                std::lock_guard<std::mutex> guard(item->mutex); return !item->started || item->finished;
            });
            if (old == tickets_.end()) return failure("resource_busy", "operation ticket limit reached");
            tickets_.erase(old);
        }
        auto context = std::make_shared<ToolContext>();
        context->owner = owner; context->instance = instance_; context->issuedAt = toolClockMs();
        context->operation = "op-" + std::to_string(nextOperation_++);
        tickets_.push_back(context);
        return success(JsonValue::object({{"operation_id", JsonValue::string(context->operation)},
            {"instance_id", JsonValue::string(instance_)}}));
    }
    std::shared_ptr<ToolContext> context;
    for (const auto& item : tickets_) {
        if ((!jsonString(params, "operation_id").empty() && jsonString(params, "operation_id") == item->operation) ||
            (item->started && jsonString(params, "job_id") == "job-" + std::to_string(item->jobId))) { context = item; break; }
    }
    if (!context) return failure("operation_expired", "operation is unknown or its deduplication record expired");
    if (context->owner != owner) return failure("stale_owner", "operation belongs to another owner");
    if (method == "toolbox.abort") {
        if (!onlyFields(params, {"owner_id", "instance_id", "operation_id"})) return failure("invalid_params", "unexpected abort field");
        context->abandoned = true;
        if (context->started) jobs_.cancel(context->jobId);
        else { std::lock_guard<std::mutex> guard(context->mutex); context->stage = "aborted"; }
        return success(progress(context, 0));
    }
    if (method == "job.start") {
        if (context->abandoned) return failure("operation_aborted", "operation was abandoned and cannot start later");
        const auto* arguments = params.find("arguments");
        const auto id = jsonString(params, "capability_id");
        std::string error;
        if (!onlyFields(params, {"owner_id", "instance_id", "operation_id", "capability_id", "arguments"}) ||
            !arguments || !validateToolArguments(id, *arguments, error))
            return failure("invalid_params", error.empty() ? "invalid tool start parameters" : error);
        const auto text = serializeJson(*arguments);
        if (context->started) {
            if (context->capability != id || context->argumentsText != text)
                return failure("operation_conflict", "operation was already used with different parameters");
            return success(progress(context, 0));
        }
        if (toolClockMs() - context->issuedAt >= 30000) return failure("operation_expired", "prepare ticket expired");
        if (id == "network.iperf3.server" && !platform_->hasRunner()) return failure("capability_unavailable", "bundled iperf3 runner is missing");
        context->capability = id; context->arguments = *arguments; context->argumentsText = text;
        context->leaseUntil = toolClockMs() + 5000;
        const auto duration = id == "gpio.pin.write" ? jsonInteger(*arguments, "hold_sec") * 1000 :
            id == "gpio.edge.monitor" ? jsonInteger(*arguments, "duration_sec") * 1000 :
            id == "network.iperf3.server" ? (jsonInteger(*arguments, "duration_sec") + 20) * 1000 :
            id == "gpio.pin.pulse" ? jsonInteger(*arguments, "pulse_ms") + 3000 : 3000;
        context->deadline = toolClockMs() + duration;
        context->execute = [platform = platform_](const BoardTestRunRequest& request, const std::atomic_bool& cancel) {
            return platform->run(request, cancel);
        };
        const auto state = jobs_.start({id, 1, context});
        if (!state) return failure("resource_busy", "another board task is active");
        context->jobId = state->jobId; context->started = true;
        return success(progress(context, 0));
    }
    if (method == "job.progress") {
        if (!onlyFields(params, {"owner_id", "instance_id", "operation_id", "job_id", "cursor"}) ||
            (params.find("cursor") && !params.find("cursor")->isInt64()) || jsonInteger(params, "cursor") < 0)
            return failure("invalid_params", "invalid progress cursor");
        if ((params.find("job_id") != nullptr) == (params.find("operation_id") != nullptr))
            return failure("invalid_params", "supply exactly one job or operation identity");
        {
            std::lock_guard<std::mutex> guard(context->mutex);
            if (static_cast<std::uint64_t>(jsonInteger(params, "cursor")) > context->eventSequence)
                return failure("invalid_params", "progress cursor is ahead of the event stream");
        }
        return success(progress(context, static_cast<std::uint64_t>(jsonInteger(params, "cursor"))));
    }
    if (!context->started) return failure("job_not_active", "operation has not started");
    const auto state = jobs_.snapshot(context->jobId);
    if (!state || !active(state->state)) return failure("job_not_active", "tool task is no longer active");
    if (method == "job.cancel") {
        if (!onlyFields(params, {"owner_id", "instance_id", "job_id"})) return failure("invalid_params", "unexpected cancel field");
        jobs_.cancel(context->jobId);
        return success(progress(context, 0));
    }
    if (state->state != BoardTestJobState::running || context->expired())
        return failure("lease_expired", "expired or cancelling task cannot be renewed");
    if (method == "job.renew") {
        if (!onlyFields(params, {"owner_id", "instance_id", "job_id"})) return failure("invalid_params", "unexpected renew field");
        context->leaseUntil = toolClockMs() + 5000;
        return success(JsonValue::object({{"renewed", JsonValue::boolean(true)}}));
    }
    if (method == "gpio.hold.extend") {
        const auto* seconds = params.find("hold_sec");
        if (!onlyFields(params, {"owner_id", "instance_id", "job_id", "hold_sec"}) ||
            !seconds || !seconds->isInt64() || seconds->int64Value() < 1 || seconds->int64Value() > 300 ||
            context->capability != "gpio.pin.write") return failure("invalid_params", "invalid hold extension");
        context->deadline = toolClockMs() + seconds->int64Value() * 1000;
        return success(JsonValue::object({{"remaining_ms", JsonValue::integer(seconds->int64Value() * 1000)}}));
    }
    return failure("unknown_method", "unknown toolbox method");
}
} // namespace testtools::agent
