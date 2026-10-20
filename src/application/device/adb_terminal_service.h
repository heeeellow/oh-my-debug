#pragma once

#include "core/serial/serial_types.h"

#include <string>

namespace testtools {

struct AdbTerminalResult {
    bool started = false;
    std::string host;
    std::string error;
};

class AdbTerminalService {
public:
    AdbTerminalResult open(const serialdemo::AdbDevice &device) const;
};

} // namespace testtools
