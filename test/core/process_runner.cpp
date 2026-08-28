#include "process_runner.h"

#include "process.hpp"

#include <future>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#endif

namespace serialdemo {

namespace {

std::string quoteArgument(const std::string &argument)
{
    if (argument.empty()) {
        return "\"\"";
    }

    const bool needsQuotes = argument.find_first_of(" \t\n\v\"") != std::string::npos;
    if (!needsQuotes) {
        return argument;
    }

    std::string result = "\"";
    std::size_t backslashes = 0;
    for (const char value : argument) {
        if (value == '\\') {
            ++backslashes;
            continue;
        }
        if (value == '\"') {
            result.append(backslashes * 2 + 1, '\\');
            result.push_back('\"');
            backslashes = 0;
            continue;
        }
        result.append(backslashes, '\\');
        backslashes = 0;
        result.push_back(value);
    }
    result.append(backslashes * 2, '\\');
    result.push_back('\"');
    return result;
}

#if defined(_WIN32) && defined(UNICODE)
std::wstring toNativeString(const std::string &text)
{
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}
#else
std::string toNativeString(const std::string &text)
{
    return text;
}
#endif

} // namespace

std::string ProcessRunner::buildCommandLine(const std::string &program,
                                            const std::vector<std::string> &arguments)
{
    std::string command = quoteArgument(program);
    for (const auto &argument : arguments) {
        command.push_back(' ');
        command += quoteArgument(argument);
    }
    return command;
}

CommandResult ProcessRunner::run(const std::string &program,
                                 const std::vector<std::string> &arguments,
                                 std::chrono::milliseconds timeout)
{
    CommandResult result;
    std::mutex outputMutex;
    const auto appendOutput = [&outputMutex](std::string &target, const char *bytes, std::size_t size) {
        std::lock_guard<std::mutex> lock(outputMutex);
        target.append(bytes, size);
    };

    {
        TinyProcessLib::Process process(
            toNativeString(buildCommandLine(program, arguments)),
            {},
            [&result, &appendOutput](const char *bytes, std::size_t size) {
                appendOutput(result.standardOutput, bytes, size);
            },
            [&result, &appendOutput](const char *bytes, std::size_t size) {
                appendOutput(result.standardError, bytes, size);
            });

        if (process.get_id() == 0) {
            result.standardError = "无法启动进程: " + program;
            return result;
        }

        auto waiter = std::async(std::launch::async, [&process]() { return process.get_exit_status(); });
        if (waiter.wait_for(timeout) != std::future_status::ready) {
            result.timedOut = true;
            process.kill(true);
            waiter.wait();
        }
        result.exitCode = waiter.get();
    }
    return result;
}

} // namespace serialdemo
