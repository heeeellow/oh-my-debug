#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <io.h>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

std::string utf8(const wchar_t* value)
{
    int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    std::string out(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, value, -1, out.data(), size, nullptr, nullptr);
    out.pop_back();
    return out;
}
int wmain(int argc, wchar_t** argv)
{
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    const auto* dir = _wgetenv(L"TESTTOOLS_MAINTENANCE_FIXTURE");
    if (!dir)
        return 90;
    std::filesystem::path root(dir);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
        args.push_back(utf8(argv[i]));
    std::string scene;
    {
        std::ifstream in(root / "scene.txt");
        in >> scene;
    }
    {
        std::ofstream trace(root / "calls.txt", std::ios::app);
        for (const auto& arg : args)
            trace << arg << '\t';
        trace << '\n';
    }
    if (args.size() < 3 || args[0] != "-s" || args[1] != "device-A") {
        std::cerr << "wrong target";
        return 91;
    }
    const auto& command = args[2];
    if (command == "root") {
        std::cout << (scene == "root-denied" ? "adbd cannot run as root in production builds\n"
                                             : "restarting adbd as root\n");
        return 0;
    }
    if (command == "remount") {
        if (scene == "remount-success-reboot") {
            std::cout << "Now reboot your device for settings to take effect\nremount succeeded\n";
            return 0;
        }
        if (scene == "remount-prepare-reboot") {
            std::cout << "Overlayfs enabled.\nReboot the device for new settings to take effect\n";
            return 0;
        }
        if (scene == "remount-mixed") {
            std::cout << "remount succeeded\n/vendor: Permission denied\n";
            return 0;
        }
        std::cout << (scene == "remount-fail"      ? "remount failed: permission denied\n"
                      : scene == "remount-unknown" ? "Please inspect device\n"
                                                   : "remount succeeded\n");
        return 0;
    }
    if (command == "reboot") {
        std::ofstream(root / "polls.txt") << 0;
        return 0;
    }
    if (command == "get-state") {
        if (scene == "reboot-state-error") {
            std::cerr << "ADB server could not start\n";
            return 2;
        }
        int count = 0;
        {
            std::ifstream in(root / "polls.txt");
            in >> count;
        }
        std::ofstream(root / "polls.txt") << count + 1;
        if (scene == "reboot-never-offline") {
            std::cout << "device\n";
            return 0;
        }
        if (count == 0 || scene == "reboot-never-online") {
            std::cerr << "error: device not found\n";
            return 1;
        }
        std::cout << "device\n";
        return 0;
    }
    if (command == "shell" && args.size() == 5 && args[3] == "-T") {
        if (args[4].find("echo OMD_LOADER_V1") == 0) {
            if (scene == "loader-denied") { std::cerr << "Permission denied"; return 1; }
            std::cout << "OMD_LOADER_V1\nPLATFORM\nrockchip,rk3568\nENDPLATFORM\n";
            if (scene != "loader-unknown") std::cout << "NODE\nsyscon-reboot-mode\nMAGIC\n 52 42 c3 01\nENDNODE\n";
            std::cout << "END\n"; return 0;
        }
        if (args[4] == "getprop ro.build.version.sdk") {
            if (scene != "linux")
                std::cout << "31\n";
            return 0;
        }
        if (args[4] == "id -u") {
            std::cout << (scene == "uid-invalid"  ? "uid=0(root)\n"
                          : scene == "uid-denied" ? "2000\n"
                                                  : "0\n");
            return 0;
        }
        if (args[4] != "dmesg")
            return 92;
    } else if (command != "logcat" || args.size() != 4 || args[3] != "-d")
        return 93;
    if (scene == "big-log") {
        const std::string chunk(4096, 'x');
        for (int i = 0; i < 1536; ++i)
            std::cout.write(chunk.data(), chunk.size());
        const char tail[] = {'\0', '\r', '\n', char(0xe6), char(0x97), char(0xa5), '!'};
        std::cout.write(tail, sizeof(tail));
        return 0;
    }
    std::cout << "prefix\n" << std::flush;
    if (scene == "log-denied") {
        std::cerr << "dmesg: Permission denied\n";
        return 1;
    }
    if (scene == "slow-log")
        std::this_thread::sleep_for(std::chrono::seconds(20));
    return 0;
}
