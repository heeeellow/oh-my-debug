#pragma once
#include "core/protocol/agent_connection.h"
#include <string>

namespace testtools {
enum class AgentServiceState { Disconnected, Connecting, Ready, Failed };
// Fixed, read-only protocol checks. Never uses the active serial control connection.
bool validateAgentService(const std::string& hello,
                          const std::string& capabilities,
                          const std::string& requiredVersion,
                          std::string& error);
bool probeAgentService(IAgentConnection& connection,
                       const std::string& requiredVersion,
                       std::string& error);
} // namespace testtools
