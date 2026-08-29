#pragma once

#include "serial_types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace testtools {

enum class AgentFrameKind : std::uint8_t {
    Json = 1,
    Binary = 2
};

struct AgentFrame {
    AgentFrameKind kind = AgentFrameKind::Json;
    serialdemo::ByteBuffer payload;
};

serialdemo::ByteBuffer encodeAgentFrame(const AgentFrame &frame);

class AgentFrameDecoder {
public:
    static constexpr std::size_t MaximumPayloadSize = 1024 * 1024;

    bool push(const serialdemo::ByteBuffer &data, std::vector<AgentFrame> &frames,
              std::string &error);
    void reset();

private:
    serialdemo::ByteBuffer buffer_;
};

AgentFrame jsonAgentFrame(const std::string &json);
std::string agentFrameText(const AgentFrame &frame);

} // namespace testtools
