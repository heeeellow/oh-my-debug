#include "command_line.h"

#include <charconv>
#include <limits>

namespace testtools {

bool parseTcpPort(std::string_view text, std::uint16_t &port, bool allowZero)
{
    if (text.empty()) {
        return false;
    }
    unsigned int value = 0;
    const char *begin = text.data();
    const char *end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end
        || value > std::numeric_limits<std::uint16_t>::max()
        || (!allowZero && value == 0)) {
        return false;
    }
    port = static_cast<std::uint16_t>(value);
    return true;
}

} // namespace testtools
