#include "application/serial/serial_transport_service.h"

#include "adapters/agent/agent_serial_transport.h"
#include "adapters/serial/cserialport_transport.h"

#include <utility>

namespace testtools {

std::vector<testtools::PortInfo> SerialTransportService::localPorts()
{
    return testtools::CSerialPortTransport::availablePorts();
}

std::vector<testtools::AgentSerialPort> SerialTransportService::agentPorts(
    std::uint16_t controlPort, std::string &error)
{
    return testtools::AgentSerialTransport::availablePorts(controlPort, error);
}

std::unique_ptr<testtools::ISerialTransport> SerialTransportService::createLocal()
{
    return std::make_unique<testtools::CSerialPortTransport>();
}

std::unique_ptr<testtools::ISerialTransport> SerialTransportService::createAgent(
    std::string adbSerial, std::uint16_t controlPort)
{
    return std::make_unique<testtools::AgentSerialTransport>(std::move(adbSerial), controlPort);
}

} // namespace testtools
