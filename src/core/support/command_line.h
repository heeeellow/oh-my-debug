#pragma once

#include <cstdint>
#include <string_view>

namespace testtools {

// 解析并校验 TCP 端口号字符串（默认仅接受 1..65535，allowZero=true 时允许 0）。
bool parseTcpPort(std::string_view text, std::uint16_t &port, bool allowZero = false);

} // namespace testtools
