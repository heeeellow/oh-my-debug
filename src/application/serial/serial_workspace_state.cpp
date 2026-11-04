#include "application/serial/serial_workspace_state.h"
#include "application/serial/serial_port_claims.h"

#include "core/serial/codec.h"

#include <algorithm>
#include <utility>

namespace testtools {

LocalPortChoice chooseLocalPort(const std::string& previous,
                               bool manual,
                               const std::vector<std::string>& before,
                               const std::vector<std::string>& available,
                               bool explicitRefresh)
{
    if (manual)
        return {previous, LocalPortChoiceState::Manual};
    const auto same = [](const std::string& a, const std::string& b) {
        return SerialPortClaims::resourceKey(false, {}, a) ==
               SerialPortClaims::resourceKey(false, {}, b);
    };
    const auto contains = [&](const std::vector<std::string>& ports, const std::string& p) {
        return std::any_of(ports.begin(), ports.end(), [&](const auto& value) { return same(value, p); });
    };
    for (const auto& endpoint : available)
        if (same(endpoint, previous))
            return {endpoint, LocalPortChoiceState::Selected};
    if (explicitRefresh) {
        std::vector<std::string> added;
        for (const auto& endpoint : available)
            if (!contains(before, endpoint) && !contains(added, endpoint))
                added.push_back(endpoint);
        if (added.size() == 1)
            return {added.front(), LocalPortChoiceState::Selected};
    }
    if (previous.empty() && available.size() == 1)
        return {available.front(), LocalPortChoiceState::Selected};
    return {{}, !previous.empty() ? LocalPortChoiceState::Missing
                                 : available.empty() ? LocalPortChoiceState::Empty
                                                     : LocalPortChoiceState::Choose};
}

namespace {

void appendEscapedByte(std::string &out, std::uint8_t byte)
{
    static constexpr char hex[] = "0123456789ABCDEF";
    out += "\\x";
    out.push_back(hex[(byte >> 4) & 0x0F]);
    out.push_back(hex[byte & 0x0F]);
}

std::size_t utf8ExpectedLength(std::uint8_t lead)
{
    if (lead < 0x80) {
        return 1;
    }
    if (lead >= 0xC2 && lead <= 0xDF) {
        return 2;
    }
    if (lead >= 0xE0 && lead <= 0xEF) {
        return 3;
    }
    if (lead >= 0xF0 && lead <= 0xF4) {
        return 4;
    }
    return 0;
}

bool validUtf8Prefix(const std::uint8_t *bytes, std::size_t available, std::size_t expected)
{
    if (available == 0 || expected < 2 || available > expected) {
        return false;
    }
    const std::uint8_t b0 = bytes[0];
    for (std::size_t i = 1; i < available; ++i) {
        const std::uint8_t b = bytes[i];
        if ((b & 0xC0) != 0x80) {
            return false;
        }
        if (i == 1) {
            if (b0 == 0xE0 && b < 0xA0) {
                return false;
            }
            if (b0 == 0xED && b > 0x9F) {
                return false;
            }
            if (b0 == 0xF0 && b < 0x90) {
                return false;
            }
            if (b0 == 0xF4 && b > 0x8F) {
                return false;
            }
        }
    }
    return true;
}

std::string formatUtf8Text(const serialdemo::ByteBuffer &bytes,
                           std::size_t length,
                           bool escapeLineBreaks)
{
    std::string out;
    out.reserve(length);
    std::size_t index = 0;
    while (index < length) {
        const std::uint8_t b = bytes[index];
        if (b < 0x80) {
            if (b == '\r') {
                if (escapeLineBreaks) {
                    out += "\\r";
                } else if (index + 1 < length) {
                    out += "\\r";
                }
            } else if (b == '\n') {
                if (escapeLineBreaks) {
                    out += "\\n";
                } else {
                    out.push_back('\n');
                }
            } else if (b == '\t' || (b >= 0x20 && b != 0x7f)) {
                out.push_back(static_cast<char>(b));
            } else {
                appendEscapedByte(out, b);
            }
            ++index;
            continue;
        }

        const std::size_t expected = utf8ExpectedLength(b);
        const std::size_t remaining = length - index;
        if (expected >= 2) {
            if (remaining >= expected && validUtf8Prefix(&bytes[index], expected, expected)) {
                out.append(reinterpret_cast<const char *>(&bytes[index]), expected);
                index += expected;
                continue;
            }
        }
        appendEscapedByte(out, b);
        ++index;
    }
    return out;
}

} // namespace

SerialWorkspaceMode SerialWorkspaceState::mode() const
{
    return mode_;
}

SerialModeSwitchDecision SerialWorkspaceState::evaluateModeSwitch(
    SerialWorkspaceMode requested, const SerialWorkspaceActivity &activity) const
{
    if (requested == mode_) {
        return {};
    }
    const bool scheduledSendRunning = std::any_of(
        activity.scheduledSendRunning.begin(), activity.scheduledSendRunning.end(),
        [](bool running) { return running; });
    if (scheduledSendRunning || activity.bidirectionalTestRunning
        || activity.reliabilityTestRunning) {
        return {false, SerialModeSwitchBlock::TaskRunning};
    }
    // 单串口与终端视图均会隐藏通道 A；绝不允许在通道 A 仍保持连接时将其隐藏。
    if (requested != SerialWorkspaceMode::Dual && activity.channelActive[0]) {
        return {false, SerialModeSwitchBlock::HiddenChannelActive};
    }
    return {};
}

bool SerialWorkspaceState::applyMode(SerialWorkspaceMode requested,
                                     const SerialWorkspaceActivity &activity)
{
    if (!evaluateModeSwitch(requested, activity).allowed) {
        return false;
    }
    mode_ = requested;
    return true;
}

SerialEndpointConflict SerialWorkspaceState::endpointConflict(
    const SerialEndpointSelection &first, const SerialEndpointSelection &second)
{
    if (!first.active || !second.active) {
        return SerialEndpointConflict::None;
    }
    if (first.kind == SerialEndpointKind::Agent
        && second.kind == SerialEndpointKind::Agent) {
        return SerialEndpointConflict::AgentSingleSession;
    }
    if (first.kind == second.kind && first.endpoint == second.endpoint
        && (first.kind == SerialEndpointKind::Local
            || first.deviceSerial == second.deviceSerial)) {
        return SerialEndpointConflict::SameResource;
    }
    return SerialEndpointConflict::None;
}

SerialLogBuffer::SerialLogBuffer(std::size_t maxEntries,
                                 std::size_t maxBytes)
    : maxEntries_(std::max<std::size_t>(1, maxEntries)),
      maxBytes_(std::max<std::size_t>(256, maxBytes))
{
}

SerialLogAppendResult SerialLogBuffer::appendTx(const serialdemo::ByteBuffer &data,
                                                std::int64_t timestampMs,
                                                std::string timestampText)
{
    if (data.empty()) {
        return {};
    }
    SerialLogAppendResult result;
    SerialLogEntry entry;
    entry.direction = SerialLogDirection::Tx;
    entry.timestampMs = timestampMs;
    entry.timestampText = std::move(timestampText);
    entry.bytes = data;
    totalBytes_ += entry.bytes.size();
    entries_.push_back(std::move(entry));
    result.addedEntries = 1;
    result.trimmedOldest = trimToLimits();
    return result;
}

SerialLogAppendResult SerialLogBuffer::appendRx(const serialdemo::ByteBuffer &data,
                                                std::int64_t timestampMs,
                                                std::string timestampText)
{
    if (data.empty()) {
        return {};
    }
    SerialLogAppendResult result;
    std::size_t offset = 0;
    while (offset < data.size()) {
        std::size_t lineEnd = offset;
        while (lineEnd < data.size() && data[lineEnd] != '\n') {
            ++lineEnd;
        }
        const bool endsWithLf = (lineEnd < data.size() && data[lineEnd] == '\n');
        if (endsWithLf) {
            ++lineEnd;
        }
        // 保持各次接收回调记录独立，不用到达时间间隔暗中拼接协议帧。
        SerialLogEntry entry;
        entry.direction = SerialLogDirection::Rx;
        entry.timestampMs = timestampMs;
        entry.timestampText = timestampText;
        entry.bytes.assign(data.begin() + static_cast<std::ptrdiff_t>(offset),
                           data.begin() + static_cast<std::ptrdiff_t>(lineEnd));
        totalBytes_ += entry.bytes.size();
        entries_.push_back(std::move(entry));
        ++result.addedEntries;
        offset = lineEnd;
    }
    result.trimmedOldest = trimToLimits();
    return result;
}

SerialLogAppendResult SerialLogBuffer::appendNotice(const std::string &text,
                                                    std::int64_t timestampMs,
                                                    std::string timestampText)
{
    if (text.empty()) {
        return {};
    }
    SerialLogAppendResult result;
    SerialLogEntry entry;
    entry.direction = SerialLogDirection::Notice;
    entry.timestampMs = timestampMs;
    entry.timestampText = std::move(timestampText);
    entry.bytes.assign(text.begin(), text.end());
    totalBytes_ += entry.bytes.size();
    entries_.push_back(std::move(entry));
    result.addedEntries = 1;
    result.trimmedOldest = trimToLimits();
    return result;
}

void SerialLogBuffer::clear()
{
    entries_.clear();
    totalBytes_ = 0;
    truncated_ = false;
}

bool SerialLogBuffer::empty() const noexcept
{
    return entries_.empty();
}

std::size_t SerialLogBuffer::size() const noexcept
{
    return entries_.size();
}

std::size_t SerialLogBuffer::totalBytes() const noexcept
{
    return totalBytes_;
}

const std::deque<SerialLogEntry> &SerialLogBuffer::entries() const noexcept
{
    return entries_;
}

std::string SerialLogBuffer::formatEntry(const SerialLogEntry &entry,
                                         bool hexDisplay,
                                         bool showTimestamp)
{
    if (entry.direction == SerialLogDirection::Notice) {
        std::string notice(reinterpret_cast<const char *>(entry.bytes.data()),
                           entry.bytes.size());
        if (notice.empty() || notice.back() != '\n') {
            notice.push_back('\n');
        }
        return notice;
    }

    std::string line;
    if (showTimestamp && !entry.timestampText.empty()) {
        line += '[';
        line += entry.timestampText;
        line += "] ";
    }
    line += (entry.direction == SerialLogDirection::Tx) ? "TX  " : "RX  ";

    if (hexDisplay) {
        line += serialdemo::toHex(entry.bytes);
    } else if (entry.direction == SerialLogDirection::Tx) {
        line += formatUtf8Text(entry.bytes, entry.bytes.size(), true);
    } else {
        std::size_t contentLength = entry.bytes.size();
        if (contentLength > 0 && entry.bytes[contentLength - 1] == '\n') {
            --contentLength;
            if (contentLength > 0 && entry.bytes[contentLength - 1] == '\r') {
                --contentLength;
            }
        } else if (contentLength > 0 &&
                   entry.bytes[contentLength - 1] == '\r') {
            --contentLength;
        }
        // 对不完整的 UTF-8 序列立即进行转义显示，不隐藏字节等待后续分片。
        line += formatUtf8Text(entry.bytes, contentLength, false);
    }
    line.push_back('\n');
    return line;
}

bool SerialLogBuffer::truncated() const noexcept
{
    return truncated_;
}

std::string SerialLogBuffer::formatAll(bool hexDisplay, bool showTimestamp,
                                      const std::string &filter) const
{
    std::string output;
    for (const auto &entry : entries_) {
        const auto text = formatEntry(entry, hexDisplay, showTimestamp);
        // 过滤仅筛选渲染输出记录，绝不改写底层保留的原始字节。
        if (filter.empty() || text.find(filter) != std::string::npos) {
            output += text;
        }
    }
    return output;
}

bool SerialLogBuffer::trimToLimits()
{
    bool trimmed = false;
    while (entries_.size() > maxEntries_ ||
           (totalBytes_ > maxBytes_ && entries_.size() > 1)) {
        totalBytes_ -= entries_.front().bytes.size();
        entries_.pop_front();
        trimmed = true;
    }
    truncated_ = truncated_ || trimmed;
    return trimmed;
}

} // namespace testtools
