#include "application/agent/agent_connection_service.h"

#include "adapters/agent/agent_client.h"

namespace testtools {

std::shared_ptr<IAgentConnection> AgentConnectionService::createClient()
{
    return std::make_shared<AgentClient>();
}

} // namespace testtools
