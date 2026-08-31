#pragma once

#include "core/protocol/agent_connection.h"

#include <memory>

namespace testtools {

class AgentConnectionService {
  public:
    static std::shared_ptr<IAgentConnection> createClient();
};

} // namespace testtools
