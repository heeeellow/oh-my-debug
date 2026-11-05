#include "iperf_json.h"
#include "toolbox_contract.h"
#include "json.hpp"
#include <cmath>
#include <limits>

namespace testtools {
namespace {
using Json = nlohmann::json;
JsonValue number(const Json& object, const char* key, double factor = 1.0)
{
    const auto found = object.find(key);
    if (found == object.end() || !found->is_number()) return JsonValue::null();
    const double value = found->get<double>() * factor;
    if (!std::isfinite(value) || value < 0 || value >= 9223372036854774784.0) return JsonValue::null();
    return JsonValue::integer(static_cast<std::int64_t>(std::floor(value + 0.5)));
}
const Json* object(const Json& root, const char* key)
{
    const auto value = root.find(key);
    return value != root.end() && value->is_object() ? &*value : nullptr;
}
}
bool IperfJsonStream::push(const std::string& bytes, std::string& error)
{
    if (raw_.size() + bytes.size() > 8U * 1024 * 1024) { error = "iperf output exceeds 8 MiB"; return false; }
    raw_ += bytes; pending_ += bytes;
    std::size_t newline;
    while ((newline = pending_.find('\n')) != std::string::npos) {
        const auto line = pending_.substr(0, newline);
        pending_.erase(0, newline + 1);
        if (!line.empty() && !record(line, error)) return false;
    }
    if (pending_.size() > 256U * 1024) { error = "iperf record exceeds 256 KiB"; return false; }
    return true;
}
bool IperfJsonStream::record(const std::string& line, std::string& error)
{
    if (line.size() > 256U * 1024) { error = "iperf record exceeds 256 KiB"; return false; }
    try {
        const auto root = Json::parse(line);
        if (!root.is_object()) { error = "iperf record is not an object"; return false; }
        if (root.contains("error")) { error = root["error"].is_string() ? root["error"].get<std::string>() : "iperf error"; return false; }
        const std::string event = root.value("event", std::string{});
        if (event == "error") { error = root.contains("data") && root["data"].is_string() ? root["data"].get<std::string>() : "iperf error event"; return false; }
        const auto* data = object(root, "data");
        if (!data) { error = "iperf streaming record has no data object"; return false; }
        if (event == "start") {
            const auto* start = object(*data, "start");
            if (!start) start = data;
            const auto connected = start->find("connected");
            if (connected != start->end() && connected->is_array() && !connected->empty()) {
                const auto& endpoint = (*connected)[0];
                peer_ = endpoint.value("remote_host", std::string{});
                local_ = endpoint.value("local_host", std::string{});
            }
        } else if (event == "interval" || event == "end") {
            const auto* end = object(*data, "end");
            if (!end) end = data;
            const auto* sum = object(*end, "sum");
            const auto* sent = object(*end, "sum_sent");
            const auto* received = object(*end, "sum_received");
            if (event == "interval") {
                if (!sum) sum = sent ? sent : received;
                if (sum) {
                    metrics_.objectItems()["current_bps"] = number(*sum, "bits_per_second");
                    metrics_.objectItems()["elapsed_ms"] = number(*sum, "end", 1000);
                    const auto* bps = metrics_.find("current_bps"); const auto* elapsed = metrics_.find("elapsed_ms");
                    if (bps && elapsed && bps->isInt64() && elapsed->isInt64()) {
                        if (!samples_.empty() && jsonInteger(samples_.back(), "elapsed_ms") == elapsed->int64Value())
                            samples_.back() = JsonValue::object({{"elapsed_ms", *elapsed}, {"bps", *bps}});
                        else {
                            if (samples_.size() >= 512) { error = "iperf interval sample limit exceeded"; return false; }
                            samples_.push_back(JsonValue::object({{"elapsed_ms", *elapsed}, {"bps", *bps}}));
                        }
                    }
                    intervalSeen_ = true;
                }
            } else {
                metrics_.objectItems()["sender_bps"] = sent ? number(*sent, "bits_per_second") : JsonValue::null();
                metrics_.objectItems()["receiver_bps"] = received ? number(*received, "bits_per_second") : JsonValue::null();
                if (sum) {
                    metrics_.objectItems()["aggregate_bps"] = number(*sum, "bits_per_second");
                    metrics_.objectItems()["jitter_us"] = number(*sum, "jitter_ms", 1000);
                    metrics_.objectItems()["lost_packets"] = number(*sum, "lost_packets");
                    metrics_.objectItems()["packets"] = number(*sum, "packets");
                    const auto lost = metrics_.find("lost_packets"), packets = metrics_.find("packets");
                    metrics_.objectItems()["loss_ppm"] = lost && packets && lost->isInt64() && packets->isInt64() &&
                        packets->int64Value() > 0 && lost->int64Value() <= packets->int64Value() ?
                        JsonValue::integer(static_cast<std::int64_t>(std::floor(
                            1000000.0 * lost->int64Value() / packets->int64Value() + 0.5))) : JsonValue::null();
                }
                metrics_.objectItems()["retransmits"] = sent ? number(*sent, "retransmits") : JsonValue::null();
                if (const auto* cpu = object(*end, "cpu_utilization_percent")) {
                    metrics_.objectItems()["local_cpu_permille"] = number(*cpu, "host_total", 10);
                    metrics_.objectItems()["remote_cpu_permille"] = number(*cpu, "remote_total", 10);
                }
                ended_ = sent || received || sum;
            }
        }
        return true;
    } catch (const std::exception& exception) { error = std::string("invalid iperf JSON: ") + exception.what(); return false; }
}
bool IperfJsonStream::finish(std::string& error)
{
    if (!pending_.empty()) { const auto tail = pending_; pending_.clear(); if (!record(tail, error)) return false; }
    if (!ended_) { error = "iperf ended without a terminal measurement"; return false; }
    return true;
}
} // namespace testtools
