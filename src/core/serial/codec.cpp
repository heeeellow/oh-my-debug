#include "core/serial/codec.h"

#include <cctype>

namespace testtools {

namespace {

int hexValue(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

} // namespace

bool parseHex(const std::string &text, ByteBuffer &output, std::string &error)
{
    std::string digits;
    digits.reserve(text.size());

    for (std::size_t index = 0; index < text.size(); ++index) {
        const char value = text[index];
        if (std::isspace(static_cast<unsigned char>(value)) || value == ',' || value == ':' || value == '-') {
            continue;
        }
        if (value == '0' && index + 1 < text.size() && (text[index + 1] == 'x' || text[index + 1] == 'X')) {
            ++index;
            continue;
        }
        if (hexValue(value) < 0) {
            error = "HEX 数据包含非法字符";
            return false;
        }
        digits.push_back(value);
    }

    if (digits.size() % 2 != 0) {
        error = "HEX 数据必须由完整字节组成";
        return false;
    }

    output.clear();
    output.reserve(digits.size() / 2);
    for (std::size_t index = 0; index < digits.size(); index += 2) {
        output.push_back(static_cast<std::uint8_t>((hexValue(digits[index]) << 4) | hexValue(digits[index + 1])));
    }
    error.clear();
    return true;
}

std::string toHex(const ByteBuffer &data)
{
    static constexpr char table[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(data.size() * 3);
    for (std::size_t index = 0; index < data.size(); ++index) {
        if (index != 0) {
            result.push_back(' ');
        }
        result.push_back(table[(data[index] >> 4) & 0x0f]);
        result.push_back(table[data[index] & 0x0f]);
    }
    return result;
}

void appendLineEnding(ByteBuffer &data, LineEnding ending)
{
    switch (ending) {
    case LineEnding::Lf:
        data.push_back('\n');
        break;
    case LineEnding::Cr:
        data.push_back('\r');
        break;
    case LineEnding::CrLf:
        data.push_back('\r');
        data.push_back('\n');
        break;
    case LineEnding::None:
        break;
    }
}

} // namespace testtools
