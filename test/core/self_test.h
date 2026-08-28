#pragma once

#include <cstdint>
#include <string>

namespace serialdemo {

int runSelfTest();
int runAgentSmokeTest(const std::string &host, std::uint16_t port);
int runAdbSerialSmokeTest(const std::string &serial, const std::string &endpoint);

} // namespace serialdemo
