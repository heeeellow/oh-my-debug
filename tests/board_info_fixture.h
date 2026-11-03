#pragma once
#include "application/agent/board_info_service.h"
#include <mutex>

namespace boardfixture {
using J = testtools::JsonValue;
enum class Mode {
    Normal,
    Hold,
    FailMemory,
    FailStorage,
    BadSchema,
    Lost,
    WrongJob,
    WrongCapability,
    IgnoreCancel,
    CancelRace
};
class Connection final : public testtools::IAgentConnection {
    mutable std::mutex mutex_;
    std::vector<std::string> starts_;
    std::string capability_, job_;
    bool active_ = false, cancelled_ = false, cancelCalled_ = false;
    int nextJob_ = 0;

  public:
    std::atomic<Mode> mode{Mode::Normal};
    std::atomic<bool> closed{false}, missing{false};
    std::atomic<int> calls{0}, cancelCalls{0};
    std::atomic<std::int64_t> memoryKiB{3987456};
    std::atomic<std::int64_t> storageBytes{31268536320LL};
    bool connect(const std::string&, std::uint16_t, std::string&) override
    {
        return true;
    }
    void close() override
    {
        closed = true;
    }
    std::vector<std::string> starts() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return starts_;
    }
    static J data(const std::string& capability, bool missing)
    {
        if (capability == "memory.capacity")
            return J::object({{"memtotal_kib", missing ? J::null() : J::integer(3987456)}});
        if (capability == "storage.device.info")
            return J::object(
                {{"devices",
                  J::array({J::object({{"device_key", J::string("emmc:mmcblk0")},
                                       {"type", J::string("emmc")},
                                       {"kernel_name", J::string("mmcblk0")},
                                       {"capacity_bytes", J::integer(31268536320LL)},
                                       {"model", J::string("fixture eMMC")},
                                       {"manufacturer", J::string("0x15")},
                                       {"serial", J::string("fixture-serial")},
                                       {"firmware", J::null()}}),
                            J::object({{"device_key", J::string("ufs:sda")},
                                       {"type", J::string("ufs")},
                                       {"kernel_name", J::string("sda")},
                                       {"capacity_bytes", J::integer(62537072640LL)}})})}});
        return J::object({{"devices",
                           J::array({J::object({{"device_key", J::string("emmc:mmcblk0")},
                                                {"type", J::string("emmc")},
                                                {"pre_eol", J::integer(0)},
                                                {"life_time_a", J::integer(1)},
                                                {"life_time_b", J::null()},
                                                {"read_bytes", J::integer(0)},
                                                {"write_bytes", J::integer(1024)},
                                                {"discard_bytes", J::null()}}),
                                     J::object({{"device_key", J::string("ufs:sda")},
                                                {"type", J::string("ufs")},
                                                {"pre_eol", J::integer(2)},
                                                {"life_time_a", J::integer(3)},
                                                {"life_time_b", J::integer(4)},
                                                {"read_bytes", J::integer(4096)}})})}});
    }
    bool request(const std::string& wire, std::string& response, std::string& error) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++calls;
        if (closed) {
            error = "fixture closed";
            return false;
        }
        J request;
        testtools::JsonError parse;
        if (!testtools::parseJson(wire, request, parse)) {
            error = "invalid request";
            return false;
        }
        const auto id = request.find("request_id")->stringValue();
        const auto method = request.find("method")->stringValue();
        const auto& params = *request.find("params");
        auto envelope = J::object(
            {{"protocol", J::integer(1)}, {"request_id", J::string(id)}, {"ok", J::boolean(true)}});
        auto fail = [&](const std::string& code) {
            envelope.objectItems()["ok"] = J::boolean(false);
            envelope.objectItems()["error"] =
                J::object({{"code", J::string(code)}, {"message", J::string("fixture " + code)}});
            response = testtools::serializeJson(envelope);
            return true;
        };
        if (method == "job.start") {
            if (active_)
                return fail("job_busy");
            capability_ = params.find("capability_id")->stringValue();
            job_ = "job-" + std::to_string(++nextJob_);
            starts_.push_back(capability_);
            active_ = true;
            cancelled_ = false;
            cancelCalled_ = false;
        } else if (method == "job.cancel") {
            ++cancelCalls;
            cancelCalled_ = true;
            if (params.find("job_id")->stringValue() != job_)
                return fail("job_not_found");
            if (mode == Mode::CancelRace) {
                active_ = false;
                return fail("job_not_active");
            }
            if (mode != Mode::IgnoreCancel)
                cancelled_ = true;
        } else if (method == "job.status") {
            if (params.find("job_id")->stringValue() != job_)
                return fail("job_not_found");
            if (mode == Mode::Lost) {
                error = "fixture transport lost";
                return false;
            }
            if (cancelled_ ||
                (mode != Mode::Hold && mode != Mode::IgnoreCancel && mode != Mode::CancelRace))
                active_ = false;
        } else
            return fail("method_not_found");
        std::string state =
            active_      ? (cancelled_ ? "cancelling" : "running")
            : cancelled_ ? "cancelled"
            : (mode == Mode::FailMemory && capability_ == "memory.capacity") ||
                    (mode == Mode::FailStorage && capability_ == "storage.device.info")
                ? "failed"
                : "succeeded";
        J result = J::null();
        auto observations = data(capability_, missing);
        if (capability_ == "memory.capacity" && !missing)
            observations.objectItems()["memtotal_kib"] = J::integer(memoryKiB.load());
        if (capability_ == "storage.device.info")
            observations.objectItems()["devices"].arrayItems()[0].objectItems()["capacity_bytes"] =
                J::integer(storageBytes.load());
        if (!active_)
            result = J::object(
                {{"completion",
                  J::string(state == "succeeded"   ? "completed"
                            : state == "cancelled" ? "cancelled"
                                                   : "error")},
                 {"exit_code", J::integer(state == "succeeded" ? 0 : 1)},
                 {"provider",
                  J::object({{"kind", J::string("agent_native")},
                             {"id", J::string("fixture")},
                             {"version", J::integer(1)}})},
                 {"observations",
                  J::object(
                      {{"schema",
                        J::string(mode == Mode::BadSchema ? "other/v1" : capability_ + "/v1")},
                       {"data", std::move(observations)}})},
                 {"provider_claim", J::null()},
                 {"summary", J::string("fixture read-only observation")},
                 {"log", J::string("")}});
        envelope.objectItems()["result"] = J::object(
            {{"job_id",
              J::string(mode == Mode::WrongJob && method == "job.status" ? "job-999" : job_)},
             {"capability_id",
              J::string(mode == Mode::WrongCapability ? "other.capability" : capability_)},
             {"capability_version", J::integer(1)},
             {"state", J::string(state)},
             {"result", result}});
        response = testtools::serializeJson(envelope);
        return true;
    }
};
inline testtools::BoardInfoRequest request(const std::shared_ptr<Connection>& connection,
                                           std::uint64_t generation = 1)
{
    testtools::BoardInfoRequest value;
    value.session = {"device-A",
                     generation,
                     connection,
                     testtools::BoardInfoService::capabilities(),
                     std::make_shared<std::atomic<bool>>(true)};
    value.capabilities = testtools::BoardInfoService::capabilities();
    value.pollInterval = std::chrono::milliseconds(1);
    value.timeout = std::chrono::milliseconds(200);
    value.cancelGrace = std::chrono::milliseconds(30);
    return value;
}
} // namespace boardfixture
