#include "application/serial/serial_transport_service.h"

#include "adapters/agent/agent_serial_transport.h"
#include "adapters/serial/cserialport_transport.h"

#include <utility>

namespace testtools {

std::vector<serialdemo::PortInfo> SerialTransportService::localPorts()
{
    return serialdemo::CSerialPortTransport::availablePorts();
}

std::vector<serialdemo::AgentSerialPort> SerialTransportService::agentPorts(
    std::uint16_t controlPort, std::string &error)
{
    return serialdemo::AgentSerialTransport::availablePorts(controlPort, error);
}

std::unique_ptr<serialdemo::ISerialTransport> SerialTransportService::createLocal()
{
    return std::make_unique<serialdemo::CSerialPortTransport>();
}

std::unique_ptr<serialdemo::ISerialTransport> SerialTransportService::createAgent(
    std::string adbSerial, std::uint16_t controlPort)
{
    return std::make_unique<serialdemo::AgentSerialTransport>(std::move(adbSerial), controlPort);
}

} // namespace testtools
