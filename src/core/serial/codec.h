#pragma once

#include "core/serial/serial_types.h"

#include <string>

namespace serialdemo {

// 串口文本发送行尾类型。
enum class LineEnding {
    None,
    Lf,
    Cr,
    CrLf
};

// 十六进制文本与字节流互转及行尾追加工具函数。
bool parseHex(const std::string &text, ByteBuffer &output, std::string &error);
std::string toHex(const ByteBuffer &data);
void appendLineEnding(ByteBuffer &data, LineEnding ending);

} // namespace serialdemo
