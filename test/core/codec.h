#pragma once

#include "serial_types.h"

#include <string>

namespace serialdemo {

bool parseHex(const std::string &text, ByteBuffer &output, std::string &error);
std::string toHex(const ByteBuffer &data);

} // namespace serialdemo
