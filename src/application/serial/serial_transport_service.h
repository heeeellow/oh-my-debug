#pragma once

#include "core/serial/serial_transport.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace testtools {

// 串口端点枚举与底层传输实例工厂服务。
class SerialTransportService {
public:
    static std::vector<serialdemo::PortInfo> localPorts();
    static std::vector<serialdemo::AgentSerialPort> agentPorts(
        std::uint16_t controlPort, std::string &error);
    static std::unique_ptr<serialdemo::ISerialTransport> createLocal();
    static std::unique_ptr<serialdemo::ISerialTransport> createAgent(
        std::string adbSerial, std::uint16_t controlPort);
};

} // namespace testtools
