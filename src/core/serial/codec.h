#pragma once

#include "core/serial/serial_types.h"

#include <string>

namespace serialdemo {

enum class LineEnding {
    None,
    Lf,
    Cr,
    CrLf
};

bool parseHex(const std::string &text, ByteBuffer &output, std::string &error);
std::string toHex(const ByteBuffer &data);
void appendLineEnding(ByteBuffer &data, LineEnding ending);

} // namespace serialdemo
