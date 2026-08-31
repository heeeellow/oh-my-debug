#pragma once

#include <cstdint>
#include <string_view>

namespace testtools {

bool parseTcpPort(std::string_view text, std::uint16_t &port, bool allowZero = false);

} // namespace testtools
