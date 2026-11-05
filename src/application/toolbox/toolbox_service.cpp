#include "toolbox_service.h"
#include "core/toolbox/toolbox_contract.h"
#include "core/toolbox/iperf_json.h"
#include "core/protocol/agent_version.h"
#include <algorithm>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>

namespace testtools {
namespace {
using Clock = std::chrono::steady_clock;
class RpcFailure final : public std::runtime_error {
  public:
    RpcFailure(std::string code, const std::string& message) : std::runtime_error("[" + code + "] " + message), code(std::move(code)) {}
    std::string code;
};
JsonValue identity(const std::string& owner, const std::string& instance)
{
    return JsonValue::object({{"owner_id", JsonValue::string(owner)}, {"instance_id", JsonValue::string(instance)}});
}
std::shared_ptr<IAgentConnection> open(const ToolboxSession& session)
{
    std::string error;
    auto connection = session.openConnection ? session.openConnection(error) : nullptr;
    if (!connection) throw std::runtime_error(error.empty() ? "板端连接不可用" : error);
    return connection;
}
class RemoteJob {
  public:
    RemoteJob(const ToolboxSession& session, std::string owner) : connection(open(session)), owner(std::move(owner))
    {
        const auto info = toolboxRpc(*connection, "toolbox.info", JsonValue::object());
        instance = jsonString(info, "instance_id");
        if (!validToolToken(instance) || jsonString(info, "schema") != "toolbox.info/v1") throw std::runtime_error("板端工具接口版本不匹配");
    }
    ~RemoteJob() { connection->close(); }
    void start(const std::string& id, const JsonValue& arguments)
    {
        capability = id;
        auto prepared = toolboxRpc(*connection, "toolbox.prepare", identity(owner, instance));
        operation = jsonString(prepared, "operation_id");
        if (operation.empty() || jsonString(prepared, "instance_id") != instance) throw std::runtime_error("操作标识不匹配");
        auto params = identity(owner, instance);
        params.objectItems()["operation_id"] = JsonValue::string(operation);
        params.objectItems()["capability_id"] = JsonValue::string(id);
        params.objectItems()["arguments"] = arguments;
        mayHaveStarted = true;
        last = toolboxRpc(*connection, "job.start", params); validate();
        cursor = jsonInteger(last, "next_cursor");
        lastRenew = Clock::now();
    }
    void validate()
    {
        const bool abortedBeforeStart = jsonString(last, "state") == "aborted" && jsonString(last, "job_id").empty() &&
            !jsonBoolean(last, "active") && jsonString(last, "capability_id").empty();
        if (jsonString(last, "schema") != "toolbox.job/v1" || jsonString(last, "instance_id") != instance ||
            jsonString(last, "operation_id") != operation || (jsonString(last, "capability_id") != capability && !abortedBeforeStart))
            throw std::runtime_error("板端任务结果归属不匹配");
        job = jsonString(last, "job_id");
        const auto* running = last.find("active"); const auto* complete = last.find("complete");
        if (!running || !running->isBool() || !complete || !complete->isBool() || !last.find("result"))
            throw std::runtime_error("板端任务状态格式不匹配");
    }
    JsonValue parameters(bool useOperation = false) const
    {
        auto params = identity(owner, instance);
        params.objectItems()[useOperation || job.empty() ? "operation_id" : "job_id"] = JsonValue::string(useOperation || job.empty() ? operation : job);
        return params;
    }
    void poll(bool stop, int extend = 0)
    {
        if (stop && !stopSent && !job.empty() && running()) {
            try { toolboxRpc(*connection, "job.cancel", parameters()); }
            catch (const RpcFailure& error) { if (error.code != "job_not_active") throw; }
            stopSent = true;
        }
        if (!stop && !stopSent && running() && Clock::now() - lastRenew >= std::chrono::seconds(1)) {
            try { toolboxRpc(*connection, "job.renew", parameters()); }
            catch (const RpcFailure& error) {
                if (error.code != "job_not_active" && error.code != "lease_expired") throw;
                if (error.code == "lease_expired") stopSent = true;
            }
            lastRenew = Clock::now();
        }
        if (extend && !stop && running()) {
            auto params = parameters(); params.objectItems()["hold_sec"] = JsonValue::integer(extend);
            toolboxRpc(*connection, "gpio.hold.extend", params);
        }
        auto params = parameters(true); params.objectItems()["cursor"] = JsonValue::integer(cursor);
        last = toolboxRpc(*connection, "job.progress", params); validate();
        const auto next = jsonInteger(last, "next_cursor", -1);
        if (next < cursor) throw std::runtime_error("板端事件游标倒退");
        cursor = next;
    }
    bool running() const { return jsonBoolean(last, "active"); }
    bool stopAndConfirm()
    {
        if (!mayHaveStarted) return true;
        auto abort = identity(owner, instance); abort.objectItems()["operation_id"] = JsonValue::string(operation);
        toolboxRpc(*connection, "toolbox.abort", abort);
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        do {
            poll(true);
            if (!running()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } while (Clock::now() < deadline);
        return false;
    }
    std::shared_ptr<IAgentConnection> connection;
    std::string owner, instance, operation, job, capability;
    JsonValue last = JsonValue::object();
    std::int64_t cursor = 0;
    Clock::time_point lastRenew{};
    bool mayHaveStarted = false, stopSent = false;
};
}
JsonValue toolboxRpc(IAgentConnection& connection, const std::string& method, const JsonValue& params)
{
    static std::atomic<std::uint64_t> sequence{0};
    const auto id = "toolbox-" + std::to_string(++sequence);
    const auto request = JsonValue::object({{"protocol", JsonValue::integer(1)},
        {"request_id", JsonValue::string(id)}, {"method", JsonValue::string(method)}, {"params", params}});
    std::string response, error;
    if (!connection.request(serializeJson(request), response, error)) throw std::runtime_error(error.empty() ? "板端通信失败" : error);
    JsonValue root;
    if (!parseJson(response, root, error) || jsonString(root, "request_id") != id || jsonInteger(root, "protocol", -1) != 1 ||
        !onlyFields(root, {"protocol", "request_id", "ok", "result", "error"}) || !root.find("ok") || !root.find("ok")->isBool())
        throw std::runtime_error("板端响应格式或请求标识不匹配");
    if (!jsonBoolean(root, "ok")) {
        const auto* value = root.find("error");
        throw RpcFailure(value ? jsonString(*value, "code") : "invalid_error", value ? jsonString(*value, "message") : "板端操作失败");
    }
    const auto* result = root.find("result");
    if (!result || !result->isObject()) throw std::runtime_error("板端响应缺少结果对象");
    return *result;
}
std::string toolStageText(const std::string& phase)
{
    if (phase == "holding") return "正在保持输出";
    if (phase == "pulsing") return "正在输出脉冲";
    if (phase == "monitoring") return "正在监听边沿";
    if (phase == "starting_server") return "正在准备网速测试";
    if (phase == "ready") return "板端已就绪";
    if (phase == "running") return "正在测速";
    if (phase == "releasing" || phase == "cleaning") return "正在释放资源";
    if (phase == "cancelling") return "正在停止";
    if (phase == "completed") return "已完成";
    return "正在处理";
}
ToolboxService::ToolboxService(std::shared_ptr<IToolboxHost> host) : host_(std::move(host)), owner_(makeToolToken()) {}
bool ToolboxService::prepare(ToolboxRequest request, bool conflict, std::string& error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.active || state_.remotePending || executing_) { error = "请先停止当前工具任务并确认结束"; return false; }
    if (conflict) { error = "设备正在执行其他任务，请稍后再试"; return false; }
    if (request.session.serial.empty() || !request.session.openConnection) { error = "请先连接板端服务"; return false; }
    if (request.networkTest) {
        auto& net = request.network;
        request.capability = "network.iperf3.server";
        request.arguments = JsonValue::object({{"interface", JsonValue::string(net.boardInterface)},
            {"bind_ip", JsonValue::string(net.boardIp)}, {"peer_ip", JsonValue::string(net.sourceIp)},
            {"port", JsonValue::integer(net.port)}, {"duration_sec", JsonValue::integer(net.seconds)},
            {"protocol", JsonValue::string(net.protocol)}, {"parallel", JsonValue::integer(net.parallel)},
            {"udp_bps", JsonValue::integer(static_cast<std::int64_t>(net.udpMbps) * 1000000)}});
        if (!host_ || net.localInterface == 0 || net.direction < 0 || net.direction > 2) { error = "请选择有效的电脑网卡与方向"; return false; }
    }
    if (!validateToolArguments(request.capability, request.arguments, error)) return false;
    const auto id = state_.id + 1;
    state_ = {}; state_.id = id; state_.serial = request.session.serial;
    state_.generation = request.session.generation; state_.capability = request.capability;
    state_.active = true; state_.state = "preparing"; state_.message = "正在准备工具任务";
    state_.report = JsonValue::object({{"schema", JsonValue::string("ohmydebug.toolbox.report/v1")},
        {"device", JsonValue::string(state_.serial)}, {"capability", JsonValue::string(state_.capability)},
        {"agent_version", JsonValue::string(AgentVersion)}, {"arguments", request.arguments},
        {"started_unix_ms", JsonValue::integer(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count())}});
    request_ = std::move(request); cancel_ = false; extend_ = 0; return true;
}
void ToolboxService::executePrepared()
{
    ToolboxRequest request;
    { std::lock_guard<std::mutex> lock(mutex_); if (!state_.active || executing_) return; executing_ = true; request = request_; }
    std::unique_ptr<RemoteJob> remote;
    std::atomic_bool localCancel{false};
    std::future<ToolProcessResult> localFuture;
    JsonValue::Array events, runs;
    std::size_t eventBytes = 0;
    std::string finalState = "failed", finalDetail;
    bool pending = false, complete = true;
    int currentDirection = 0;
    const auto observe = [&] {
        if (!remote) return;
        const auto* batch = remote->last.find("events");
        if (batch && batch->isArray()) for (const auto& event : batch->arrayItems()) {
            eventBytes += serializeJson(event).size();
            if (eventBytes > 7U * 1024 * 1024 || events.size() >= 65536) { complete = false; cancel_ = true; break; }
            events.push_back(event);
        }
        complete = complete && jsonBoolean(remote->last, "complete", false);
        std::lock_guard<std::mutex> lock(mutex_);
        state_.instance = remote->instance; state_.operation = remote->operation; state_.job = remote->job;
        state_.progress = remote->last; state_.message = toolStageText(jsonString(remote->last, "stage"));
        if (request.capability.rfind("gpio.board.", 0) == 0) state_.message += " · GPIO";
        else if (request.capability.rfind("gpio.", 0) == 0) state_.message += " · " + jsonString(request.arguments, "chip") + " / 行 " + std::to_string(jsonInteger(request.arguments, "offset"));
        state_.detail = jsonString(remote->last, "detail"); state_.complete = complete;
    };
    try {
        if (request.networkTest) {
            std::string error;
            if (!host_->available(error) || !host_->validateRoute(request.network.localInterface, request.network.sourceIp, request.network.boardIp, error))
                throw std::runtime_error(error);
        }
        const int directions = request.networkTest && request.network.direction == 2 ? 2 : 1;
        for (int index = 0; index < directions; ++index) {
            currentDirection = index;
            if (cancel_) { finalState = "cancelled"; break; }
            remote = std::make_unique<RemoteJob>(request.session, owner_);
            // 在可能失去启动响应前保存操作标识；取消/复核始终针对原实例。
            try { remote->start(request.capability, request.arguments); } catch (...) {
                std::lock_guard<std::mutex> lock(mutex_); state_.instance = remote->instance;
                state_.operation = remote->operation; state_.job = remote->job; throw;
            }
            observe();
            if (!request.networkTest) {
                auto stopDeadline = Clock::time_point::max();
                auto expiryDeadline = Clock::time_point::max();
                while (remote->running()) {
                    if (cancel_ && stopDeadline == Clock::time_point::max()) stopDeadline = Clock::now() + std::chrono::seconds(5);
                    remote->poll(cancel_, extend_.exchange(0)); observe();
                    if (jsonInteger(remote->last, "remaining_ms", -1) == 0 && expiryDeadline == Clock::time_point::max()) expiryDeadline = Clock::now() + std::chrono::seconds(5);
                    if (remote->running() && (Clock::now() >= stopDeadline || Clock::now() >= expiryDeadline)) { pending = true; throw std::runtime_error("停止或到期后尚未确认板端任务结束"); }
                    if (remote->running()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                finalState = jsonString(remote->last, "state", "failed");
                if (const auto* result = remote->last.find("result"); result && result->isObject()) finalDetail = jsonString(*result, "summary");
                runs.push_back(remote->last); break;
            }
            while (remote->running() && jsonString(remote->last, "stage") != "ready") {
                remote->poll(cancel_); observe(); std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!remote->running()) { runs.push_back(remote->last); finalState = jsonString(remote->last, "state", "failed"); break; }
            const bool reverse = request.network.direction == 1 || (request.network.direction == 2 && index == 1);
            const auto& net = request.network;
            std::vector<std::string> args{"-c", net.boardIp, "-B", net.sourceIp, "-p", std::to_string(net.port),
                "-t", std::to_string(net.seconds), "-P", std::to_string(net.parallel), "-i", "0.5", "--connect-timeout", "5000", "--json-stream", "--forceflush"};
            if (reverse) args.push_back("-R");
            if (net.protocol == "udp") { args.push_back("-u"); args.push_back("-b"); args.push_back(std::to_string(static_cast<std::int64_t>(net.udpMbps) * 1000000)); }
            IperfJsonStream parser; std::mutex parserMutex; std::string parseError;
            localCancel = false;
            localFuture = std::async(std::launch::async, [&] {
                return host_->run(args, net.seconds + 15, localCancel, [&](const char* bytes, std::size_t size) {
                    std::lock_guard<std::mutex> lock(parserMutex);
                    return parser.push(std::string(bytes, size), parseError);
                });
            });
            while (localFuture.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
                if (cancel_) localCancel = true;
                try { remote->poll(cancel_); observe(); }
                catch (...) { localCancel = true; localFuture.wait(); throw; }
                if (!remote->running() && jsonString(remote->last, "state") != "succeeded") localCancel = true;
                { std::lock_guard<std::mutex> parseLock(parserMutex); std::lock_guard<std::mutex> stateLock(mutex_);
                  state_.progress.objectItems()["client_metrics"] = parser.metrics();
                  state_.progress.objectItems()["client_samples"] = parser.samples();
                  state_.progress.objectItems()["direction"] = JsonValue::string(reverse ? "board_to_pc" : "pc_to_board"); }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            const auto local = localFuture.get();
            const bool parsed = parseError.empty() && parser.finish(parseError);
            const bool peerMatches = parser.peer() == net.boardIp && parser.local() == net.sourceIp;
            const bool localOk = local.started && !local.cancelled && !local.truncated && local.exitCode == 0 && parsed && peerMatches;
            const auto waitEnd = Clock::now() + std::chrono::seconds(5);
            while (remote->running() && Clock::now() < waitEnd) {
                remote->poll(cancel_ || !localOk); observe();
                if (remote->running()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (remote->running()) throw std::runtime_error("板端测速尚未确认结束");
            const bool succeeded = localOk && jsonString(remote->last, "state") == "succeeded";
            runs.push_back(JsonValue::object({{"direction", JsonValue::string(reverse ? "board_to_pc" : "pc_to_board")},
                {"state", JsonValue::string(succeeded ? "succeeded" : cancel_ ? "cancelled" : "failed")},
                {"metrics", parser.metrics()}, {"samples", parser.samples()}, {"remote", remote->last},
                {"raw_stdout", JsonValue::string(parser.raw().substr(0, 1024 * 1024))},
                {"raw_truncated", JsonValue::boolean(parser.raw().size() > 1024 * 1024)},
                {"stderr", JsonValue::string(local.error.substr(0, 65536))},
                {"parser_error", JsonValue::string(parseError)}, {"peer_verified", JsonValue::boolean(peerMatches)}}));
            if (!succeeded) { finalState = cancel_ ? "cancelled" : "failed";
                finalDetail = !parseError.empty() ? parseError : !peerMatches ? "测速端点与所选地址不匹配" : local.error; break; }
            finalState = "succeeded";
        }
    } catch (const std::exception& error) {
        localCancel = true;
        if (localFuture.valid()) localFuture.wait();
        finalDetail = error.what(); finalState = cancel_ ? "cancelled" : "failed";
        if (remote) {
            if (!pending) {
                try { pending = !remote->stopAndConfirm(); observe(); }
                catch (...) { pending = remote->mayHaveStarted; }
            }
            if (request.networkTest && remote->mayHaveStarted && runs.size() == static_cast<std::size_t>(currentDirection))
                runs.push_back(JsonValue::object({{"direction", JsonValue::string(request.network.direction == 1 || currentDirection == 1 ? "board_to_pc" : "pc_to_board")},
                    {"state", JsonValue::string(pending ? "unknown" : finalState)}, {"remote", remote->last}, {"error", JsonValue::string(finalDetail)}}));
        }
    }
    if (!request.networkTest && remote && remote->mayHaveStarted && runs.empty()) runs.push_back(remote->last);
    if (pending) complete = false;
    const auto executedDirections = runs.size();
    if (request.networkTest) {
        const auto expected = request.network.direction == 2 ? 2U : 1U;
        while (runs.size() < expected) {
            const bool reverse = request.network.direction == 1 || runs.size() == 1;
            runs.push_back(JsonValue::object({{"direction", JsonValue::string(reverse ? "board_to_pc" : "pc_to_board")},
                {"state", JsonValue::string("not_run")}, {"reason", JsonValue::string("previous step did not finish successfully")}}));
        }
        if (executedDirections < expected) complete = false;
    }
    if (!complete && finalState == "succeeded") finalState = "failed";
    std::lock_guard<std::mutex> lock(mutex_);
    state_.active = false; state_.cancelling = false; state_.remotePending = pending; executing_ = false;
    state_.state = pending ? "unknown" : finalState; state_.complete = complete;
    state_.detail = finalDetail;
    state_.message = pending ? "结束状态未知，请重新连接原设备并复核" : finalState == "succeeded" ? "工具任务已完成" :
        finalState == "cancelled" ? "工具任务已停止" : finalState == "timed_out" ? "工具任务超时" : "工具任务失败，请查看详情";
    state_.report.objectItems()["state"] = JsonValue::string(state_.state);
    state_.report.objectItems()["complete"] = JsonValue::boolean(complete);
    state_.report.objectItems()["remote_pending"] = JsonValue::boolean(pending);
    state_.report.objectItems()["finished_unix_ms"] = JsonValue::integer(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    state_.report.objectItems()["detail"] = JsonValue::string(finalDetail);
    state_.report.objectItems()["runs"] = JsonValue::array(std::move(runs));
    state_.report.objectItems()["events"] = JsonValue::array(std::move(events));
    state_.report.objectItems()["instance_id"] = JsonValue::string(state_.instance);
    if (request.networkTest) {
        state_.report.objectItems()["iperf_version"] = JsonValue::string("3.20");
        state_.report.objectItems()["local_interface_index"] = JsonValue::integer(request.network.localInterface);
        state_.report.objectItems()["requested_directions"] = JsonValue::integer(request.network.direction == 2 ? 2 : 1);
        state_.report.objectItems()["executed_directions"] = JsonValue::integer(executedDirections);
    }
    if (remote) { state_.instance = remote->instance; state_.operation = remote->operation; state_.job = remote->job; }
}
void ToolboxService::cancel(std::uint64_t expectedId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (expectedId && expectedId != state_.id) return;
    cancel_ = true; if (state_.active) { state_.cancelling = true; state_.message = "正在停止工具任务"; }
}
void ToolboxService::extendHold(int seconds) { if (seconds >= 1 && seconds <= 300) extend_ = seconds; }
ToolboxSnapshot ToolboxService::snapshot() const { std::lock_guard<std::mutex> lock(mutex_); return state_; }
bool ToolboxService::reconcile(const ToolboxSession& session, std::string& error)
{
    const auto previous = snapshot();
    if (!previous.remotePending) return true;
    if (session.serial != previous.serial) { error = "请选择原任务设备后复核"; return false; }
    try {
        auto connection = open(session);
        const auto info = toolboxRpc(*connection, "toolbox.info", JsonValue::object());
        const auto instance = jsonString(info, "instance_id");
        if (!validToolToken(instance)) throw std::runtime_error("板端实例标识无效");
        if (instance == previous.instance) {
            auto params = identity(owner_, previous.instance);
            params.objectItems()["operation_id"] = JsonValue::string(previous.operation);
            toolboxRpc(*connection, "toolbox.abort", params);
            const auto result = toolboxRpc(*connection, "job.progress", params);
            if (jsonString(result, "schema") != "toolbox.job/v1" || jsonString(result, "instance_id") != instance ||
                jsonString(result, "operation_id") != previous.operation || !result.find("active") || !result.find("active")->isBool())
                throw std::runtime_error("原任务复核响应格式或归属不匹配");
            if (jsonBoolean(result, "active")) throw std::runtime_error("原任务仍未结束，请稍后复核");
        }
        connection->close();
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_.id != previous.id) return false;
        state_.remotePending = false; state_.message = instance == previous.instance ? "已复核原任务结束" : "原板端服务已重启，旧任务资源已释放";
        if (previous.capability == "gpio.board.set") state_.message += "；GPIO 设置可能仍然生效，请重新读取当前状态";
        state_.state = "failed";
        state_.report.objectItems()["state"] = JsonValue::string(state_.state);
        state_.report.objectItems()["remote_pending"] = JsonValue::boolean(false);
        state_.report.objectItems()["reconciled"] = JsonValue::boolean(true);
        state_.report.objectItems()["reconciliation"] = JsonValue::string(state_.message);
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}
} // namespace testtools
