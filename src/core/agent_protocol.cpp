#include "agent_protocol.h"

#include <algorithm>

namespace testtools {

namespace {

std::uint32_t readU32(const serialdemo::ByteBuffer &data)
{
    return (static_cast<std::uint32_t>(data[0]) << 24)
           | (static_cast<std::uint32_t>(data[1]) << 16)
           | (static_cast<std::uint32_t>(data[2]) << 8)
           | static_cast<std::uint32_t>(data[3]);
}

} // namespace

serialdemo::ByteBuffer encodeAgentFrame(const AgentFrame &frame)
{
    if (frame.payload.size() > AgentFrameDecoder::MaximumPayloadSize) {
        return {};
    }
    const std::uint32_t length = static_cast<std::uint32_t>(frame.payload.size() + 1);
    serialdemo::ByteBuffer encoded;
    encoded.reserve(static_cast<std::size_t>(length) + 4);
    encoded.push_back(static_cast<std::uint8_t>((length >> 24) & 0xFF));
    encoded.push_back(static_cast<std::uint8_t>((length >> 16) & 0xFF));
    encoded.push_back(static_cast<std::uint8_t>((length >> 8) & 0xFF));
    encoded.push_back(static_cast<std::uint8_t>(length & 0xFF));
    encoded.push_back(static_cast<std::uint8_t>(frame.kind));
    encoded.insert(encoded.end(), frame.payload.begin(), frame.payload.end());
    return encoded;
}

bool AgentFrameDecoder::push(const serialdemo::ByteBuffer &data, std::vector<AgentFrame> &frames,
                             std::string &error)
{
    frames.clear();
    buffer_.insert(buffer_.end(), data.begin(), data.end());
    while (buffer_.size() >= 4) {
        const std::uint32_t length = readU32(buffer_);
        if (length < 1 || length > MaximumPayloadSize + 1) {
            error = "Agent frame length is outside the allowed range";
            reset();
            return false;
        }
        if (buffer_.size() < static_cast<std::size_t>(length) + 4) {
            error.clear();
            return true;
        }
        const auto kind = static_cast<AgentFrameKind>(buffer_[4]);
        if (kind != AgentFrameKind::Json && kind != AgentFrameKind::Binary) {
            error = "Agent frame kind is not supported";
            reset();
            return false;
        }
        AgentFrame frame;
        frame.kind = kind;
        frame.payload.assign(buffer_.begin() + 5,
                             buffer_.begin() + 4 + static_cast<std::ptrdiff_t>(length));
        frames.push_back(std::move(frame));
        buffer_.erase(buffer_.begin(),
                      buffer_.begin() + 4 + static_cast<std::ptrdiff_t>(length));
    }
    error.clear();
    return true;
}

void AgentFrameDecoder::reset()
{
    buffer_.clear();
}

AgentFrame jsonAgentFrame(const std::string &json)
{
    AgentFrame frame;
    frame.kind = AgentFrameKind::Json;
    frame.payload.assign(json.begin(), json.end());
    return frame;
}

std::string agentFrameText(const AgentFrame &frame)
{
    return {frame.payload.begin(), frame.payload.end()};
}

} // namespace testtools
