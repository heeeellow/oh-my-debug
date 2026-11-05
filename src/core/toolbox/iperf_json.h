#pragma once
#include "core/protocol/json_value.h"
#include <string>
#include <vector>

namespace testtools {
// iperf 的标准 JSON 小数仅在此适配器转换；控制协议仍使用整数基础单位。
class IperfJsonStream {
  public:
    bool push(const std::string& bytes, std::string& error);
    bool finish(std::string& error);
    JsonValue metrics() const { return metrics_; }
    JsonValue samples() const { return JsonValue::array(samples_); }
    const std::string& raw() const { return raw_; }
    bool ended() const { return ended_; }
    const std::string& peer() const { return peer_; }
    const std::string& local() const { return local_; }
  private:
    bool record(const std::string& line, std::string& error);
    std::string pending_, raw_, peer_, local_;
    JsonValue metrics_ = JsonValue::object();
    JsonValue::Array samples_;
    bool ended_ = false, intervalSeen_ = false;
};
} // namespace testtools
