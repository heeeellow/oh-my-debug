#include "self_test.h"

#include "adb_transport.h"
#include "codec.h"
#include "process_runner.h"

#include <chrono>
#include <algorithm>
#include <iostream>

namespace serialdemo {

namespace {

std::string trim(std::string value)
{
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ')) {
        value.pop_back();
    }
    return value;
}

} // namespace

int runSelfTest()
{
    ByteBuffer bytes;
    std::string error;
    if (!parseHex("55 AA 01 0d 0A", bytes, error) || toHex(bytes) != "55 AA 01 0D 0A") {
        std::cerr << "FAIL codec: " << error << '\n';
        return 10;
    }

    AdbTransport adb;
    auto devices = adb.availableDevices(error);
    if (!error.empty() || devices.empty()) {
        std::cerr << "FAIL adb devices: " << error << '\n';
        return 20;
    }
    const auto online = std::find_if(devices.begin(), devices.end(), [](const AdbDevice &device) {
        return device.state == "device";
    });
    if (online == devices.end()) {
        std::cerr << "FAIL adb device state=" << devices.front().state << '\n';
        return 22;
    }
    const auto serial = online->serial;
    const auto ports = adb.availableRemotePorts(serial, error);
    if (!error.empty()) {
        std::cerr << "FAIL adb ports: " << error << '\n';
        return 21;
    }

    const auto forward = ProcessRunner::run("adb",
                                             {"-s", serial, "forward", "tcp:0", "dev:/dev/null"},
                                             std::chrono::seconds(5));
    if (forward.timedOut || forward.exitCode != 0) {
        std::cerr << "FAIL adb dev forward: " << forward.standardError << '\n';
        return 30;
    }

    int localPort = 0;
    try {
        localPort = std::stoi(trim(forward.standardOutput));
    } catch (...) {
        std::cerr << "FAIL invalid adb forward port\n";
        return 31;
    }

    const auto cleanup = ProcessRunner::run("adb",
                                             {"-s", serial, "forward", "--remove", "tcp:" + std::to_string(localPort)},
                                             std::chrono::seconds(5));
    if (cleanup.timedOut || cleanup.exitCode != 0) {
        std::cerr << "FAIL cleanup adb forward\n";
        return 32;
    }

    std::cout << "PASS codec; adb_device=" << serial << "; remote_ports=" << ports.size()
              << "; safe_dev_forward=tcp:" << localPort << '\n';
    return 0;
}

} // namespace serialdemo
