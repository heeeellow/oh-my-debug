#pragma once

#include "core/serial/serial_types.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <unordered_set>

namespace serialdemo {

struct PeriodicSendConfig {
    std::chrono::milliseconds interval{1000};
    std::uint64_t targetCount = 0; // 0 means continuous.
};

class PeriodicSendPlan {
public:
    using Clock = std::chrono::steady_clock;

    bool start(const PeriodicSendConfig &config, Clock::time_point now = Clock::now());
    void stop();
    bool isRunning() const;
    bool isDue(Clock::time_point now = Clock::now()) const;
    void recordAttempt(bool succeeded, Clock::time_point now = Clock::now());

    std::uint64_t attempts() const;
    std::uint64_t successes() const;
    const PeriodicSendConfig &config() const;

private:
    PeriodicSendConfig config_;
    Clock::time_point nextDue_{};
    std::uint64_t attempts_ = 0;
    std::uint64_t successes_ = 0;
    bool running_ = false;
};

struct IntegrityStats {
    std::uint64_t receivedBytes = 0;
    std::uint64_t discardedBytes = 0;
    std::uint64_t frames = 0;
    std::uint64_t uniqueFrames = 0;
    std::uint64_t validFrames = 0;
    std::uint64_t corruptedFrames = 0;
    std::uint64_t duplicateFrames = 0;
    std::uint64_t framingErrors = 0;
    std::uint64_t comparedBits = 0;
    std::uint64_t bitErrors = 0;

    double bitErrorRate() const;
};

ByteBuffer makeIntegrityPayload(std::uint32_t sequence, std::size_t payloadSize);
ByteBuffer encodeIntegrityFrame(std::uint32_t sequence, std::size_t payloadSize);

class IntegrityStreamVerifier {
public:
    static constexpr std::size_t MaximumPayloadSize = 4096;

    void reset();
    void push(const ByteBuffer &data);
    const IntegrityStats &stats() const;

private:
    void discardPrefix(std::size_t count, bool framingError);
    void consumeFrame(std::size_t payloadSize);

    ByteBuffer buffer_;
    IntegrityStats stats_;
    std::unordered_set<std::uint32_t> sequences_;
};

} // namespace serialdemo
