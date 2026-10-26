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

    // Windows 子进程命令行参数转义拼装（直接启动受管子进程，不经过命令解释器 Shell）。
    static std::string buildCommandLine(const std::string &program,
                                        const std::vector<std::string> &arguments);
};

} // namespace serialdemo
