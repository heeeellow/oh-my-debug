#pragma once

#include <cstdint>
#include <string>

namespace serialdemo {

int runSelfTest();
int runAgentSmokeTest(const std::string &host, std::uint16_t port);
int runAdbSerialSmokeTest(const std::string &serial, const std::string &endpoint);
int runAgentSerialSmokeTest(const std::string &serial, std::uint16_t agentControlPort,
                            const std::string &endpoint);
int runAgentDeploymentSmokeTest(const std::string &serial,
                                const std::string &applicationDirectory);
int runAgentReconnectSmokeTest(const std::string &serial);

} // namespace serialdemo
