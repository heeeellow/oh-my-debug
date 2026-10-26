#pragma once

#include "core/serial/serial_types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace testtools {

// Agent 控制通道帧类型：1 为 UTF-8 JSON 控制报文，2 为二进制载荷帧。
enum class AgentFrameKind : std::uint8_t {
    Json = 1,
    Binary = 2
};

struct AgentFrame {
    AgentFrameKind kind = AgentFrameKind::Json;
    serialdemo::ByteBuffer payload;
};

// 按 [类型:1字节][大端长度:4字节][载荷] 格式编码帧。
serialdemo::ByteBuffer encodeAgentFrame(const AgentFrame &frame);

// 流式帧解码器：支持 TCP 粘包/拆包重组及单帧最大长度校验。
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
