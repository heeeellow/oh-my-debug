#pragma once

#include "core/serial/serial_types.h"

#include <string>

namespace testtools {

// 外部交互式 ADB Shell 终端启动结果与用例服务。
struct AdbTerminalResult {
    bool started = false;
    std::string host;
    std::string error;
    int effectiveUid = -1;
};

class AdbTerminalService {
public:
  int shellUid(const serialdemo::AdbDevice& device) const;
  AdbTerminalResult open(const serialdemo::AdbDevice& device) const;
};

} // namespace testtools
