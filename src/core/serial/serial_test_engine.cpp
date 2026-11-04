#include "core/serial/serial_test_engine.h"

#include <algorithm>
#include <array>
#include <limits>

namespace testtools {

namespace {

constexpr std::array<std::uint8_t, 4> Magic{{'T', 'T', 'S', 'P'}};
constexpr std::uint8_t ProtocolVersion = 1;
constexpr std::size_t HeaderSize = 12;
constexpr std::size_t TrailerSize = 4;

void appendU16(ByteBuffer &data, std::uint16_t value)
{
    data.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    data.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void appendU32(ByteBuffer &data, std::uint32_t value)
{
    data.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    data.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    data.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    data.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

std::uint16_t readU16(const ByteBuffer &data, std::size_t offset)
{
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[offset]) << 8)
                                      | data[offset + 1]);
}

std::uint32_t readU32(const ByteBuffer &data, std::size_t offset)
{
    return (static_cast<std::uint32_t>(data[offset]) << 24)
           | (static_cast<std::uint32_t>(data[offset + 1]) << 16)
           | (static_cast<std::uint32_t>(data[offset + 2]) << 8)
           | static_cast<std::uint32_t>(data[offset + 3]);
}

std::uint32_t crc32(const std::uint8_t *data, std::size_t size)
{
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

std::uint64_t differingBits(std::uint8_t left, std::uint8_t right)
{
    std::uint8_t value = static_cast<std::uint8_t>(left ^ right);
    std::uint64_t count = 0;
    while (value != 0) {
        value = static_cast<std::uint8_t>(value & static_cast<std::uint8_t>(value - 1));
        ++count;
    }
    return count;
}

} // namespace

bool PeriodicSendPlan::start(const PeriodicSendConfig &config, Clock::time_point now)
{
    if (config.interval.count() <= 0) {
        return false;
    }
    config_ = config;
    attempts_ = 0;
    successes_ = 0;
    nextDue_ = now;
    running_ = true;
    return true;
}

void PeriodicSendPlan::stop()
{
    running_ = false;
}

bool PeriodicSendPlan::isRunning() const
{
    return running_;
}

bool PeriodicSendPlan::isDue(Clock::time_point now) const
{
    return running_ && now >= nextDue_;
}

void PeriodicSendPlan::recordAttempt(bool succeeded, Clock::time_point now)
{
    if (!running_) {
        return;
    }
    ++attempts_;
    if (succeeded) {
        ++successes_;
    }
    if (config_.targetCount != 0 && attempts_ >= config_.targetCount) {
        running_ = false;
        return;
    }

    // 跳过因界面或调度停顿而错过的定时器时间槽，避免恢复后突发连续写入。
    nextDue_ += config_.interval;
    if (nextDue_ <= now) {
        nextDue_ = now + config_.interval;
    }
}

std::uint64_t PeriodicSendPlan::attempts() const
{
    return attempts_;
}

std::uint64_t PeriodicSendPlan::successes() const
{
    return successes_;
}

const PeriodicSendConfig &PeriodicSendPlan::config() const
{
    return config_;
}

double IntegrityStats::bitErrorRate() const
{
    return comparedBits == 0 ? 0.0
                             : static_cast<double>(bitErrors) / static_cast<double>(comparedBits);
}

ByteBuffer makeIntegrityPayload(std::uint32_t sequence, std::size_t payloadSize)
{
    payloadSize = std::min(payloadSize, IntegrityStreamVerifier::MaximumPayloadSize);
    ByteBuffer payload;
    payload.reserve(payloadSize);
    std::uint32_t state = sequence ^ 0x9E3779B9u;
    for (std::size_t index = 0; index < payloadSize; ++index) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        payload.push_back(static_cast<std::uint8_t>((state + static_cast<std::uint32_t>(index)) & 0xFF));
    }
    return payload;
}

ByteBuffer encodeIntegrityFrame(std::uint32_t sequence, std::size_t payloadSize)
{
    payloadSize = std::min(payloadSize, IntegrityStreamVerifier::MaximumPayloadSize);
    ByteBuffer frame(Magic.begin(), Magic.end());
    frame.push_back(ProtocolVersion);
    frame.push_back(0);
    appendU16(frame, static_cast<std::uint16_t>(payloadSize));
    appendU32(frame, sequence);
    const auto payload = makeIntegrityPayload(sequence, payloadSize);
    frame.insert(frame.end(), payload.begin(), payload.end());
    appendU32(frame, crc32(frame.data() + Magic.size(), frame.size() - Magic.size()));
    return frame;
}

void IntegrityStreamVerifier::reset()
{
    buffer_.clear();
    stats_ = {};
    sequences_.clear();
}

void IntegrityStreamVerifier::push(const ByteBuffer &data)
{
    if (data.empty()) {
        return;
    }
    stats_.receivedBytes += data.size();
    buffer_.insert(buffer_.end(), data.begin(), data.end());

    while (true) {
        const auto magic = std::search(buffer_.begin(), buffer_.end(), Magic.begin(), Magic.end());
        if (magic != buffer_.begin()) {
            if (magic == buffer_.end()) {
                const std::size_t keep = std::min(buffer_.size(), Magic.size() - 1);
                discardPrefix(buffer_.size() - keep, buffer_.size() > keep);
                return;
            }
            discardPrefix(static_cast<std::size_t>(std::distance(buffer_.begin(), magic)), true);
        }
        if (buffer_.size() < HeaderSize) {
            return;
        }
        const std::size_t payloadSize = readU16(buffer_, 6);
        if (buffer_[4] != ProtocolVersion || payloadSize > MaximumPayloadSize) {
            discardPrefix(1, true);
            continue;
        }
        const std::size_t frameSize = HeaderSize + payloadSize + TrailerSize;
        if (buffer_.size() < frameSize) {
            return;
        }
        consumeFrame(payloadSize);
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(frameSize));
    }
}

const IntegrityStats &IntegrityStreamVerifier::stats() const
{
    return stats_;
}

void IntegrityStreamVerifier::discardPrefix(std::size_t count, bool framingError)
{
    if (count == 0) {
        return;
    }
    stats_.discardedBytes += count;
    if (framingError) {
        ++stats_.framingErrors;
    }
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(count));
}

void IntegrityStreamVerifier::consumeFrame(std::size_t payloadSize)
{
    const std::size_t frameSize = HeaderSize + payloadSize + TrailerSize;
    const std::uint32_t sequence = readU32(buffer_, 8);
    const std::uint32_t expectedCrc = readU32(buffer_, frameSize - TrailerSize);
    const std::uint32_t actualCrc = crc32(buffer_.data() + Magic.size(),
                                          frameSize - Magic.size() - TrailerSize);
    const auto expectedPayload = makeIntegrityPayload(sequence, payloadSize);

    std::uint64_t bitErrors = 0;
    for (std::size_t index = 0; index < payloadSize; ++index) {
        bitErrors += differingBits(buffer_[HeaderSize + index], expectedPayload[index]);
    }

    ++stats_.frames;
    stats_.comparedBits += payloadSize * 8;
    stats_.bitErrors += bitErrors;
    if (sequences_.insert(sequence).second) {
        ++stats_.uniqueFrames;
    } else {
        ++stats_.duplicateFrames;
    }
    if (expectedCrc == actualCrc && bitErrors == 0) {
        ++stats_.validFrames;
    } else {
        ++stats_.corruptedFrames;
    }
}

int minimumStressIntervalMs(const SerialConfig &config,
                            std::size_t payloadSize,
                            int requestedIntervalMs)
{
    const std::size_t boundedPayload =
        std::min(payloadSize, IntegrityStreamVerifier::MaximumPayloadSize);
    const std::size_t frameBytes = HeaderSize + boundedPayload + TrailerSize;
    const int stopBits = config.stopBits == StopBits::Two ? 2 : 1;
    const int parityBits = config.parity == Parity::None ? 0 : 1;
    const int bitsPerByte = 1 + std::max(5, config.dataBits) + parityBits + stopBits;
    const int baudRate = std::max(300, config.baudRate);
    // 按物理线速向上取整计算单帧传输毫秒数，防止发送间隔小于串口实际传输耗时导致缓冲堆积。
    const auto wireMillis = static_cast<int>(
        (frameBytes * static_cast<std::size_t>(bitsPerByte) * 1000 +
         static_cast<std::size_t>(baudRate) - 1) /
        static_cast<std::size_t>(baudRate));
    return std::max(requestedIntervalMs, wireMillis);
}

bool appendAndMatchFrame(std::string &buffer,
                         const ByteBuffer &data,
                         std::string &expectedFrame,
                         std::size_t maxBufferBytes)
{
    if (expectedFrame.empty()) {
        return false;
    }
    if (!data.empty()) {
        buffer.append(reinterpret_cast<const char *>(data.data()), data.size());
    }
    if (buffer.find(expectedFrame) != std::string::npos) {
        expectedFrame.clear();
        return true;
    }
    if (maxBufferBytes > 0 && buffer.size() > maxBufferBytes) {
        const std::size_t keepBytes =
            std::min(maxBufferBytes, std::max(maxBufferBytes / 2, expectedFrame.size()));
        buffer.erase(0, buffer.size() - keepBytes);
    }
    return false;
}

StressDirectionReport evaluateStressDirection(const PeriodicSendPlan &sendPlan,
                                              const IntegrityStreamVerifier &verifier)
{
    const auto &stats = verifier.stats();
    StressDirectionReport report;
    report.sentFrames = sendPlan.successes();
    report.uniqueFrames = stats.uniqueFrames;
    report.lostFrames =
        report.sentFrames > stats.uniqueFrames ? report.sentFrames - stats.uniqueFrames : 0;
    report.corruptedFrames = stats.corruptedFrames;
    report.framingErrors = stats.framingErrors;
    report.bitErrors = stats.bitErrors;
    report.bitErrorRate = stats.bitErrorRate();
    report.passed = report.sentFrames > 0 && report.lostFrames == 0 &&
                    report.corruptedFrames == 0 && report.framingErrors == 0 &&
                    report.bitErrors == 0;
    return report;
}

} // namespace testtools
