#pragma once

#include "core/serial/serial_types.h"

#include <chrono>
#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace serialdemo {

struct ManagedCommandResult {
    CommandResult command;
    bool cancelled = false;
    bool outputTruncated = false;
};

class ProcessRunner {
public:
    static CommandResult run(const std::string &program,
                             const std::vector<std::string> &arguments,
                             std::chrono::milliseconds timeout);
    static bool openTerminal(const std::string &program,
                             const std::vector<std::string> &arguments,
                             std::string &host, std::string &error);
    static ManagedCommandResult runManaged(const std::string& program,
        const std::vector<std::string>& arguments, std::chrono::milliseconds timeout,
        const std::atomic<bool>& cancel,
        const std::function<void(const std::string&)>& progress = {});

    // Windows argv quoting, shared by owned child processes (no command shell).
    static std::string buildCommandLine(const std::string &program,
                                        const std::vector<std::string> &arguments);
};

} // namespace serialdemo
