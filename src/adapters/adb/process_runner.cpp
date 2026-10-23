#include "adapters/adb/process_runner.h"

#include "process.hpp"

#include <algorithm>
#include <cwchar>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#endif

namespace serialdemo {

namespace {

std::string quoteArgument(const std::string &argument, bool forceQuotes = false)
{
    if (argument.empty()) {
        return "\"\"";
    }

    const bool needsQuotes = forceQuotes || argument.find_first_of(" \t\n\v\"") != std::string::npos;
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

#ifdef _WIN32
std::wstring searchProgram(const std::wstring &name)
{
    wchar_t path[32768] = {};
    const DWORD size = SearchPathW(nullptr, name.c_str(), L".exe",
                                  static_cast<DWORD>(std::size(path)), path, nullptr);
    return size > 0 && size < std::size(path) ? std::wstring(path, size) : std::wstring{};
}

bool createTerminalProcess(const std::wstring &program, std::wstring command,
                           DWORD flags, void *environment, bool checkLauncher,
                           DWORD &error)
{
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE,
                        flags, environment, nullptr, &startup, &process)) {
        error = GetLastError();
        return false;
    }
    CloseHandle(process.hThread);
    bool started = true;
    // wt may exit after handing off a window. Only an immediate nonzero exit is a launch failure.
    if (checkLauncher && WaitForSingleObject(process.hProcess, 500) == WAIT_OBJECT_0) {
        DWORD exitCode = 0;
        if (GetExitCodeProcess(process.hProcess, &exitCode) && exitCode != 0) {
            error = exitCode;
            started = false;
        }
    }
    // External windows belong to the user, not to ProcessRunner's blocking command lifetime.
    CloseHandle(process.hProcess);
    return started;
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

ManagedCommandResult ProcessRunner::runManaged(const std::string& program,
    const std::vector<std::string>& arguments, std::chrono::milliseconds timeout,
    const std::atomic<bool>& cancel, const std::function<void(const std::string&)>& progress)
{
    ManagedCommandResult result;
    if (cancel) { result.cancelled = true; return result; }
    std::mutex mutex;
    std::atomic<bool> truncated{false};
    const auto append = [&](std::string& output, const char* data, std::size_t size) {
        { std::lock_guard<std::mutex> lock(mutex);
          constexpr std::size_t limit = 4 * 1024 * 1024;
          const auto available = limit - output.size();
          output.append(data, std::min(size, available));
          if (size > available) truncated = true;
        }
        if (progress) { try { progress(std::string(data, std::min<std::size_t>(size, 512))); } catch (...) {} }
    };
    {
        TinyProcessLib::Process process(toNativeString(buildCommandLine(program, arguments)), {},
            [&](const char* data, std::size_t size) { append(result.command.standardOutput, data, size); },
            [&](const char* data, std::size_t size) { append(result.command.standardError, data, size); });
        if (!process.get_id()) { result.command.standardError = "无法启动进程：" + program; return result; }
#ifdef _WIN32
        // Keep a native handle while waiting so an exited client's PID cannot be reused.
        const std::unique_ptr<void, decltype(&CloseHandle)> client(
            OpenProcess(PROCESS_TERMINATE, FALSE, process.get_id()), &CloseHandle);
#endif
        auto waiter = std::async(std::launch::async, [&] { return process.get_exit_status(); });
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (waiter.wait_for(std::chrono::milliseconds(20)) != std::future_status::ready) {
            if (cancel || truncated || std::chrono::steady_clock::now() >= deadline) {
                result.cancelled = cancel.load();
                result.command.timedOut = !result.cancelled && !truncated;
                // TinyProcessLib::kill also terminates direct children on Windows.
                // An ADB client may have started the shared server, which we do not own.
#ifdef _WIN32
                if (client) TerminateProcess(client.get(), 2);
#else
                process.kill(true);
#endif
                waiter.wait(); break;
            }
        }
        result.command.exitCode = waiter.get();
    }
    result.outputTruncated = truncated.load();
    return result;
}

bool ProcessRunner::openTerminal(const std::string &program,
                                  const std::vector<std::string> &arguments,
                                  std::string &host, std::string &error)
{
    host.clear();
    error.clear();
#if defined(_WIN32) && defined(UNICODE)
    const auto unsafe = [](const std::string &value) {
        return value.find_first_of("\"\r\n") != std::string::npos ||
               value.find('\0') != std::string::npos;
    };
    if (unsafe(program) || std::any_of(arguments.begin(), arguments.end(), unsafe)) {
        error = "终端路径或参数包含不支持的引号或控制字符";
        return false;
    }
    const auto executable = searchProgram(toNativeString(program));
    if (executable.empty()) {
        error = "找不到 ADB 程序，请检查随包 platform-tools 或 ADB_PATH";
        return false;
    }
    const auto wt = searchProgram(L"wt.exe");
    DWORD launchError = 0;
    // A semicolon is a wt command separator; let cmd handle such literal arguments instead.
    const bool wtSafe = executable.find(L';') == std::wstring::npos &&
        std::none_of(arguments.begin(), arguments.end(), [](const auto &value) {
            return value.find(';') != std::string::npos;
        });
    if (!wt.empty() && wtSafe) {
        std::wstring command = L"\"" + wt + L"\" -w new new-tab \"" + executable + L"\"";
        for (const auto &argument : arguments) {
            command += L" " + toNativeString(quoteArgument(argument, true));
        }
        if (createTerminalProcess(wt, std::move(command), 0, nullptr, true, launchError)) {
            host = "Windows Terminal";
            return true;
        }
    }

    wchar_t systemDirectory[MAX_PATH] = {};
    const UINT systemLength = GetSystemDirectoryW(systemDirectory, MAX_PATH);
    if (systemLength == 0 || systemLength >= MAX_PATH) {
        error = "无法定位 Windows 命令提示符";
        return false;
    }
    const std::wstring cmd = std::wstring(systemDirectory) + L"\\cmd.exe";
    std::wstring shellCommand = L"\"" + executable + L"\"";
    for (const auto &argument : arguments) {
        shellCommand += L" " + toNativeString(quoteArgument(argument, true));
    }
    // Expand the command once from a private child environment. Percent signs in paths stay
    // literal, and /v:off keeps exclamation marks literal without changing the parent environment.
    const std::wstring variable = L"TESTTOOLS_TERMINAL_COMMAND=";
    std::vector<std::wstring> variables;
    wchar_t *inherited = GetEnvironmentStringsW();
    if (!inherited) {
        error = "无法读取终端启动环境";
        return false;
    }
    for (const wchar_t *entry = inherited; *entry; entry += std::wcslen(entry) + 1) {
        if (_wcsnicmp(entry, variable.c_str(), variable.size()) != 0) {
            variables.emplace_back(entry);
        }
    }
    FreeEnvironmentStringsW(inherited);
    variables.push_back(variable + shellCommand);
    std::sort(variables.begin(), variables.end(), [](const auto &first, const auto &second) {
        return _wcsicmp(first.c_str(), second.c_str()) < 0;
    });
    std::vector<wchar_t> environment;
    for (const auto &value : variables) {
        environment.insert(environment.end(), value.begin(), value.end());
        environment.push_back(L'\0');
    }
    environment.push_back(L'\0');
    std::wstring command = L"\"" + cmd + L"\" /d /v:off /s /k \"%TESTTOOLS_TERMINAL_COMMAND%\"";
    if (createTerminalProcess(cmd, std::move(command),
                              CREATE_NEW_CONSOLE | CREATE_UNICODE_ENVIRONMENT,
                              environment.data(), false, launchError)) {
        host = "命令提示符";
        return true;
    }
    error = "无法打开 ADB 终端（Windows 错误 " + std::to_string(launchError) + "）";
    return false;
#else
    error = "ADB 系统终端目前仅支持 Windows";
    return false;
#endif
}

} // namespace serialdemo
