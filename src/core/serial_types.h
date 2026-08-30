#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace serialdemo {

enum class Parity {
    None,
    Odd,
    Even
};

enum class StopBits {
    One,
    Two
};

enum class FlowControl {
    None,
    Hardware,
    Software
};

struct SerialConfig {
    std::string endpoint;
    std::string adbSerial;
    bool allowAdbRoot = false;
    int baudRate = 115200;
    int dataBits = 8;
    Parity parity = Parity::None;
    StopBits stopBits = StopBits::One;
    FlowControl flowControl = FlowControl::None;
};

struct PortInfo {
    std::string name;
    std::string description;
    std::string hardwareId;
};

struct AgentSerialPort {
    std::string endpoint;
    std::string interfaceName;
    std::string occupiedBy;
    bool readable = false;
    bool writable = false;
};

struct AdbDevice {
    std::string serial;
    std::string state;
    std::string description;
};

struct CommandResult {
    int exitCode = -1;
    bool timedOut = false;
    std::string standardOutput;
    std::string standardError;
};

using ByteBuffer = std::vector<std::uint8_t>;

} // namespace serialdemo
