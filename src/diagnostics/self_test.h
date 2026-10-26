#pragma once

#include <cstdint>
#include <string>

namespace serialdemo {

// 命令行自检与实机/板端冒烟测试入口集合。
int runSelfTest();
int runAgentSmokeTest(const std::string &host, std::uint16_t port);
int runAdbSerialSmokeTest(const std::string &serial, const std::string &endpoint);
int runAgentSerialSmokeTest(const std::string &serial, std::uint16_t agentControlPort,
                            const std::string &endpoint);
int runAgentDeploymentSmokeTest(const std::string &serial,
                                const std::string &applicationDirectory);
int runAgentReconnectSmokeTest(const std::string &serial);
int runAgentAtSmokeTest(const std::string &serial, const std::string &endpoint);

} // namespace serialdemo
