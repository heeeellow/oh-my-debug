#pragma once

#include "core/serial/serial_types.h"

#include <chrono>
#include <string>
#include <vector>

namespace serialdemo {

class ProcessRunner {
public:
    static CommandResult run(const std::string &program,
                             const std::vector<std::string> &arguments,
                             std::chrono::milliseconds timeout);

private:
    static std::string buildCommandLine(const std::string &program,
                                        const std::vector<std::string> &arguments);
};

} // namespace serialdemo
