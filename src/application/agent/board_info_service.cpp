#include "application/agent/board_info_service.h"
#include <algorithm>
#include <iomanip>
#include <set>
#include <sstream>
#include <thread>

namespace testtools {
namespace {
using Clock = std::chrono::steady_clock;
bool terminal(BoardTestJobState state)
{
    return state != BoardTestJobState::Running && state != BoardTestJobState::Cancelling;
}
bool has(const std::vector<std::string>& values, const std::string& value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}
std::int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
std::string number(const JsonValue& value, const char* key, bool positive = false)
{
    const auto* data = value.find(key);
    return data && data->isInt64() && data->int64Value() >= (positive ? 1 : 0)
               ? std::to_string(data->int64Value())
               : "未知";
}
std::string text(const JsonValue& value, const char* key)
{
    const auto* data = value.find(key);
    return data && data->isString() && !data->stringValue().empty() ? data->stringValue() : "未知";
}
std::string capacity(const JsonValue& value, const char* key, std::uint64_t scale, const char* unit)
{
    const auto raw = number(value, key, true);
    if (raw == "未知")
        return raw;
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << std::stold(raw) * scale / (1024.0L * 1024 * 1024)
        << " GiB（" << raw << ' ' << unit << "）";
    return out.str();
}
} // namespace
const std::vector<std::string>& BoardInfoService::capabilities()
{
    static const std::vector<std::string> ids{
        "memory.capacity", "storage.device.info", "storage.device.health"};
    return ids;
}
bool decodeBoardInfo(const std::string& capability,
                     const BoardTestResult& result,
                     std::vector<BoardInfoField>& fields,
                     std::string& error)
{
    fields.clear();
    error.clear();
    if (result.observationSchema != capability + "/v1" || !result.observations.isObject()) {
        error = "观察值格式或版本不受支持";
        return false;
    }
    const auto& data = result.observations;
    if (capability == "memory.capacity") {
        fields.push_back({{},
                          "系统可见内存",
                          capacity(data, "memtotal_kib", 1024, "KiB"),
                          "/proc/meminfo 的 MemTotal，不代表 DDR 标称容量"});
        return true;
    }
    if (capability != "storage.device.info" && capability != "storage.device.health") {
        error = "不支持的只读信息项";
        return false;
    }
    const auto* devices = data.find("devices");
    if (!devices || !devices->isArray() || devices->arrayItems().size() > 128) {
        error = "存储设备列表缺失、无效或超过 128 项";
        return false;
    }
    if (devices->arrayItems().empty()) {
        fields.push_back({{}, "存储设备", "未知", "设备未报告存储候选对象"});
        return true;
    }
    std::size_t index = 0;
    for (const auto& device : devices->arrayItems()) {
        if (!device.isObject()) {
            error = "存储设备项格式无效";
            return false;
        }
        const auto key = text(device, "device_key");
        const auto group = key == "未知" ? "未标识设备 " + std::to_string(++index) : key;
        if (capability == "storage.device.info") {
            fields.push_back(
                {group, "设备类型", text(device, "type"), "设备报告的类型；所有候选对象均保留"});
            fields.push_back({group,
                              "设备容量",
                              capacity(device, "capacity_bytes", 1, "字节"),
                              "/sys 报告的设备容量"});
            for (const auto& field : {std::make_pair("kernel_name", "内核名称"),
                                      {"manufacturer", "厂商"},
                                      {"model", "型号"},
                                      {"serial", "序列号"},
                                      {"firmware", "固件"}})
                fields.push_back({group, field.second, text(device, field.first), "设备原始字段"});
        } else {
            for (const auto& field : {std::make_pair("pre_eol", "Pre-EOL"),
                                      {"life_time_a", "寿命字段 A"},
                                      {"life_time_b", "寿命字段 B"}})
                fields.push_back({group,
                                  field.second,
                                  number(device, field.first),
                                  "设备分档原值；不换算剩余寿命百分比"});
            for (const auto& field : {std::make_pair("read_bytes", "累计读取"),
                                      {"write_bytes", "累计写入"},
                                      {"discard_bytes", "累计丢弃"}}) {
                const auto raw = number(device, field.first);
                fields.push_back({group,
                                  field.second,
                                  raw == "未知" ? raw : raw + " 字节",
                                  "设备可读的 I/O 计数，不是当前速率"});
            }
        }
    }
    return true;
}
bool BoardInfoService::prepare(BoardInfoRequest request, bool maintenanceBusy, std::string& error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    error.clear();
    if (state_.active)
        error = "已有硬件信息读取任务，请等待结束";
    else if (maintenanceBusy)
        error = "该设备正在维护，请等待维护结束";
    else if (request.session.serial.empty() || !request.session.connection ||
             !request.session.generation || !request.session.alive ||
             !request.session.alive->load())
        error = "请先连接当前设备的板端服务";
    else if (request.capabilities.empty() || request.capabilities.size() > 3 ||
             request.timeout.count() <= 0 || request.timeout > std::chrono::seconds(60) ||
             request.pollInterval.count() <= 0 || request.pollInterval > std::chrono::seconds(1) ||
             request.cancelGrace.count() <= 0 || request.cancelGrace > std::chrono::seconds(10))
        error = "读取参数无效";
    std::set<std::string> distinct;
    for (const auto& id : request.capabilities)
        if (!has(capabilities(), id) || !distinct.insert(id).second)
            error = "只允许三个已定义的信息项，且不能重复";
    const bool any =
        std::any_of(request.capabilities.begin(),
                    request.capabilities.end(),
                    [&](const std::string& id) { return has(request.session.capabilities, id); });
    if (!any && error.empty())
        error = "当前服务未声明所选只读能力";
    if (!error.empty())
        return false;
    auto records =
        state_.serial == request.session.serial && state_.generation == request.session.generation
            ? state_.records
            : std::vector<BoardInfoRecord>{};
    if (records.empty())
        for (const auto& id : capabilities())
            records.push_back({id});
    for (auto& record : records)
        if (has(request.capabilities, record.capability)) {
            const auto id = record.capability;
            record = {};
            record.capability = id;
            record.state = has(request.session.capabilities, id) ? BoardInfoState::NotRead
                                                                 : BoardInfoState::Unavailable;
        }
    const auto id = state_.id + 1;
    const auto pending = state_.remotePending && state_.serial == request.session.serial;
    state_ = {};
    state_.id = id;
    state_.active = true;
    state_.serial = request.session.serial;
    state_.generation = request.session.generation;
    state_.records = std::move(records);
    state_.remotePending = pending;
    state_.message = "准备读取";
    request_ = std::move(request);
    executing_ = false;
    cancel_ = false;
    return true;
}
void BoardInfoService::executePrepared()
{
    BoardInfoRequest request;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!state_.active || executing_)
            return;
        executing_ = true;
        request = request_;
    }
    struct OwnedConnection {
        std::shared_ptr<IAgentConnection> connection;
        ~OwnedConnection()
        {
            if (connection)
                try {
                    connection->close();
                } catch (...) {
                }
        }
    } owned;
    std::string openError;
    auto connection = request.session.connection;
    if (request.session.openConnection && !cancel_ && request.session.alive->load()) {
        try {
            owned.connection = request.session.openConnection(openError);
        } catch (const std::exception& e) {
            openError = e.what();
        } catch (...) {
            openError = "无法创建只读服务连接";
        }
        connection = owned.connection;
        if (!connection && openError.empty())
            openError = "无法建立只读服务连接";
    }
    BoardTestJobClient client(connection);
    bool abort = false;
    const auto save = [&](const BoardInfoRecord& record) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& row : state_.records)
            if (row.capability == record.capability) {
                row = record;
                break;
            }
    };
    const auto pending = [&](bool value) {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.remotePending = value;
    };
    for (const auto& capability : request.capabilities) {
        BoardInfoRecord record;
        record.capability = capability;
        if (!has(request.session.capabilities, capability)) {
            record.state = BoardInfoState::Unavailable;
            record.summary = "当前服务未声明此能力";
            save(record);
            continue;
        }
        if (abort || cancel_ || !request.session.alive->load()) {
            record.state = BoardInfoState::Cancelled;
            record.summary = "未执行，读取已停止或会话已变化";
            save(record);
            continue;
        }
        record.state = BoardInfoState::Reading;
        save(record);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_.current = capability;
            state_.message = "正在读取";
        }
        const auto deadline = Clock::now() + request.timeout;
        std::string jobId;
        try {
            auto call = client.start(capability);
            if (!call.ok) {
                record.state = BoardInfoState::Failed;
                record.error =
                    call.error.code + ": " + (openError.empty() ? call.error.message : openError);
                if (!openError.empty())
                    abort = true;
                if (call.error.code == "transport_error" || call.error.code == "invalid_response" ||
                    call.error.code == "job_busy") {
                    abort = true;
                    pending(true);
                }
            } else {
                bool cancelling = false, expired = false;
                auto cancelDeadline = Clock::time_point::max();
                jobId = call.snapshot.jobId;
                const auto valid = [&](const BoardTestJobCall& value) {
                    return value.ok && value.snapshot.jobId == jobId &&
                           value.snapshot.capabilityId == capability &&
                           value.snapshot.capabilityVersion == 1;
                };
                if (!valid(call)) {
                    record.state = BoardInfoState::Failed;
                    record.error = "任务响应与请求的信息项或版本不一致";
                    pending(true);
                    abort = true;
                } else {
                    pending(false);
                    while (true) {
                        if (!valid(call)) {
                            record.state = BoardInfoState::Failed;
                            record.error = call.ok ? "任务响应的设备任务 ID 或能力不匹配"
                                                   : call.error.code + ": " + call.error.message;
                            pending(true);
                            abort = true;
                            break;
                        }
                        record.job = call.snapshot;
                        save(record);
                        expired = expired || Clock::now() >= deadline;
                        if (terminal(call.snapshot.state)) {
                            record.state = expired ? BoardInfoState::TimedOut
                                           : call.snapshot.state == BoardTestJobState::Succeeded
                                               ? BoardInfoState::Completed
                                           : call.snapshot.state == BoardTestJobState::Cancelled
                                               ? BoardInfoState::Cancelled
                                           : call.snapshot.state == BoardTestJobState::TimedOut
                                               ? BoardInfoState::TimedOut
                                               : BoardInfoState::Failed;
                            pending(false);
                            if (call.snapshot.result) {
                                record.summary = call.snapshot.result->summary;
                                if (record.state != BoardInfoState::Completed &&
                                    record.error.empty())
                                    record.error = record.summary;
                                if (record.state == BoardInfoState::Completed &&
                                    !decodeBoardInfo(capability,
                                                     *call.snapshot.result,
                                                     record.fields,
                                                     record.error))
                                    record.state = BoardInfoState::Failed;
                            }
                            if (expired) {
                                record.error = "主机等待超时";
                                abort = true;
                            }
                            break;
                        }
                        if (!cancelling && (expired || cancel_ || !request.session.alive->load())) {
                            cancelling = true;
                            cancelDeadline = Clock::now() + request.cancelGrace;
                            const auto stopped = client.cancel(jobId);
                            if (stopped.ok) {
                                call = stopped;
                                continue;
                            }
                            if (stopped.error.code != "job_not_active") {
                                record.state =
                                    expired ? BoardInfoState::TimedOut : BoardInfoState::Cancelled;
                                record.error = "停止请求未确认：" + stopped.error.message;
                                pending(true);
                                abort = true;
                                break;
                            }
                        }
                        if (cancelling && Clock::now() >= cancelDeadline) {
                            record.state =
                                expired ? BoardInfoState::TimedOut : BoardInfoState::Cancelled;
                            record.error = "停止等待已结束，远端任务终态尚未确认";
                            pending(true);
                            abort = true;
                            break;
                        }
                        std::this_thread::sleep_for(request.pollInterval);
                        call = client.status(jobId);
                    }
                }
            }
        } catch (const std::exception& e) {
            record.state = BoardInfoState::Failed;
            record.error = e.what();
            pending(true);
            abort = true;
        } catch (...) {
            record.state = BoardInfoState::Failed;
            record.error = "读取任务出现未知异常";
            pending(true);
            abort = true;
        }
        if (!request.session.alive->load()) {
            record.state = BoardInfoState::Cancelled;
            record.error = "设备或连接会话已变化，旧结果不用于当前设备";
            abort = true;
        }
        record.finishedAtMs = nowMs();
        save(record);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    state_.active = false;
    state_.cancelling = false;
    state_.current.clear();
    state_.message = state_.remotePending ? "读取已停止，远端结束状态未确认"
                     : cancel_            ? "读取已停止"
                                          : "信息读取已结束";
    executing_ = false;
}
void BoardInfoService::cancel(std::uint64_t expectedId) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!state_.active || (expectedId && expectedId != state_.id))
        return;
    cancel_ = true;
    state_.cancelling = true;
    state_.message = "正在停止读取…";
}
BoardInfoSnapshot BoardInfoService::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}
} // namespace testtools
