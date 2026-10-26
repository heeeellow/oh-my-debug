#include "application/serial/serial_port_claims.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace testtools {

SerialPortClaims::Claim::Claim(std::shared_ptr<State> state, std::string key)
    : state_(std::move(state)), key_(std::move(key))
{
}

SerialPortClaims::Claim::~Claim()
{
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->owners.erase(key_);
}

std::string SerialPortClaims::resourceKey(bool agent,
                                          const std::string& serial,
                                          const std::string& endpoint)
{
    // 协议 v1 每块目标板仅允许单个活动串口会话（无论选择哪个 tty 节点）。
    if (agent)
        return serial.empty() ? std::string{} : "agent:" + serial;
    auto name = endpoint;
    if (name.rfind("\\\\.\\", 0) == 0)
        name.erase(0, 4);
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    if (name.rfind("COM", 0) == 0 && name.size() > 3 &&
        name.find_first_not_of("0123456789", 3) == std::string::npos) {
        const auto first = name.find_first_not_of('0', 3);
        name = "COM" + (first == std::string::npos ? std::string("0") : name.substr(first));
    }
    return name.empty() ? std::string{} : "local:" + name;
}

std::shared_ptr<SerialPortClaims::Claim> SerialPortClaims::acquire(
    const std::string& key, const std::string& owner, std::string& error)
{
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (key.empty()) {
        error = "请选择有效的串口";
        return {};
    }
    const auto existing = state_->owners.find(key);
    if (existing != state_->owners.end()) {
        error = "串口已被“" + existing->second + "”占用，请先在该页面断开";
        return {};
    }
    auto claim = std::shared_ptr<Claim>(new Claim(state_, key));
    state_->owners.emplace(key, owner);
    error.clear();
    return claim;
}

} // namespace testtools
