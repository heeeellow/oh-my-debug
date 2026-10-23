#include "adapters/adb/process_runner.h"
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <io.h>
#include <iostream>
#include <thread>
#include <windows.h>

std::string utf8(const wchar_t* value)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    std::string out(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, value, -1, out.data(), n, nullptr, nullptr);
    out.pop_back();
    return out;
}
std::filesystem::path host(std::string value)
{
    if (value.size() > 3 && value[0] == '/' && value[2] == '/')
        value = std::string(1, value[1]) + ":" + value.substr(2);
    return std::filesystem::u8path(value);
}
int wmain(int argc, wchar_t** argv)
{
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
        args.push_back(utf8(argv[i]));
    if (args.size() == 1 && args[0] == "leaf") {
        std::this_thread::sleep_for(std::chrono::seconds(15));
        return 0;
    }
    if (args.size() == 2 && args[0] == "spawn-leaf") {
        std::wstring command = L"\"" + std::wstring(argv[0]) + L"\" leaf";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(argv[0],
                            command.data(),
                            nullptr,
                            nullptr,
                            FALSE,
                            CREATE_NO_WINDOW,
                            nullptr,
                            nullptr,
                            &startup,
                            &process))
            return 3;
        {
            std::ofstream pidFile(std::filesystem::u8path(args[1]));
            pidFile << process.dwProcessId;
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        std::this_thread::sleep_for(std::chrono::seconds(5));
        return 0;
    }
    if (args.size() == 1 && args[0] == "sleep") {
        for (int i = 0; i < 100; ++i) {
            std::cout << "working\n" << std::flush;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return 0;
    }
    if (args.size() == 1 && args[0] == "flood") {
        std::cout << std::string(5 * 1024 * 1024, 'x');
        return 0;
    }
    if (args.size() < 3 || args[0] != "-s" || args[1] != "device-A") {
        std::cerr << "invalid fixed device\n";
        return 2;
    }
    if (args[2] == "shell" && args.size() == 5 && args[3] == "-T") {
        if (args[4].find("noaccess") != std::string::npos) {
            std::cerr << "Permission denied\n";
            return 1;
        }
        if (args[4].find("cd ") == 0 && args[4].find("invalid-name") != std::string::npos) {
            for (const auto& value :
                 std::vector<std::string>{"bad:name", "f", "1", "0", "o", "", ""})
                std::cout.write(value.c_str(), value.size() + 1);
            return 0;
        }
        if (args[4].find("cd ") == 0 && args[4].find("case-clash") != std::string::npos) {
            for (const auto& name : std::vector<std::string>{"a.txt", "A.txt"})
                for (const auto& field : std::vector<std::string>{name, "f", "1", "0", "o", "", ""})
                    std::cout.write(field.c_str(), field.size() + 1);
            return 0;
        }
        auto result = serialdemo::ProcessRunner::run(
            "C:/Program Files/Git/bin/bash.exe", {"-c", args[4]}, std::chrono::seconds(15));
        std::cout.write(result.standardOutput.data(), result.standardOutput.size());
        std::cerr.write(result.standardError.data(), result.standardError.size());
        return result.exitCode;
    }
    if ((args[2] == "pull" || args[2] == "push") && args.size() == 5) {
        if (args[3].find("denied.bin") != std::string::npos) {
            std::cerr << "fixture transfer denied\n";
            return 1;
        }
        if (args[3].find("slow.bin") != std::string::npos)
            for (int i = 0; i < 100; ++i) {
                std::cout << "[ 1% ] slow.bin\n" << std::flush;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        try {
            auto source = host(args[3]), target = host(args[4]);
            if (args[2] == "push" && std::filesystem::is_directory(target))
                target /= source.filename();
            std::filesystem::copy(source,
                                  target,
                                  std::filesystem::copy_options::recursive |
                                      std::filesystem::copy_options::overwrite_existing);
            std::cout << "1 file copied\n";
            return 0;
        } catch (const std::exception& e) {
            std::cerr << e.what() << '\n';
            return 1;
        }
    }
    std::cerr << "unsupported fixture command\n";
    return 2;
}
