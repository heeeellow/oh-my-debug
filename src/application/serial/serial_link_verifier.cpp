#include "serial_link_verifier.h"
#include "core/serial/codec.h"

namespace testtools {

std::array<testtools::ByteBuffer, 2>
SerialLinkVerifier::startProbe(const std::string& token,
                               const std::array<testtools::LineEnding, 2>& endings)
{
    stopProbe();
    probe_.running = true;
    probe_.aToBPassed = probe_.bToAPassed = false;
    const std::array<std::string, 2> frames{"SERIAL_POC_A_TO_B_" + token,
                                            "SERIAL_POC_B_TO_A_" + token};
    expectedFrames_[1] = frames[0];
    expectedFrames_[0] = frames[1];
    std::array<testtools::ByteBuffer, 2> bytes;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i].assign(frames[i].begin(), frames[i].end());
        testtools::appendLineEnding(bytes[i], endings[i]);
    }
    return bytes;
}

void SerialLinkVerifier::stopProbe()
{
    probe_.running = false;
    ++probe_.generation;
    expectedFrames_ = {};
    detectionBuffers_ = {};
}

bool SerialLinkVerifier::expireProbe(std::uint64_t generation)
{
    if (!probe_.running || probe_.generation != generation)
        return false;
    probe_.running = false;
    expectedFrames_ = {};
    detectionBuffers_ = {};
    return true;
}

bool SerialLinkVerifier::receive(int receiver, const testtools::ByteBuffer& bytes)
{
    if (receiver < 0 || receiver >= 2)
        return false;
    if (stressRunning_) {
        for (auto& direction : directions_)
            if (direction.active && direction.receiver == receiver)
                direction.verifier.push(bytes);
        return false;
    }
    if (!probe_.running || expectedFrames_[receiver].empty() ||
        !testtools::appendAndMatchFrame(
            detectionBuffers_[receiver], bytes, expectedFrames_[receiver]))
        return false;
    if (receiver == 0)
        probe_.bToAPassed = true;
    else
        probe_.aToBPassed = true;
    if (probe_.aToBPassed && probe_.bToAPassed)
        probe_.running = false;
    return true;
}

int SerialLinkVerifier::stressInterval(int direction,
                                       std::size_t payloadSize,
                                       int requestedIntervalMs,
                                       const std::array<testtools::SerialConfig, 2>& configs)
{
    if (direction < 0 || direction > 2 || payloadSize == 0 ||
        payloadSize > testtools::IntegrityStreamVerifier::MaximumPayloadSize ||
        requestedIntervalMs < 10 || requestedIntervalMs > 60000)
        return 0;
    int interval = requestedIntervalMs;
    if (direction == 0 || direction == 2)
        interval = testtools::minimumStressIntervalMs(configs[0], payloadSize, interval);
    if (direction == 1 || direction == 2)
        interval = testtools::minimumStressIntervalMs(configs[1], payloadSize, interval);
    return interval;
}

int SerialLinkVerifier::startStress(int direction,
                                    std::uint64_t frameCount,
                                    std::size_t payloadSize,
                                    int requestedIntervalMs,
                                    const std::array<testtools::SerialConfig, 2>& configs,
                                    Clock::time_point now)
{
    const int interval = stressInterval(direction, payloadSize, requestedIntervalMs, configs);
    if (!interval || frameCount == 0 || frameCount > 1000000)
        return 0;
    directions_ = {};
    const auto activate = [&](std::size_t slot, int tx, int rx) {
        auto& state = directions_[slot];
        state.active = true;
        state.transmitter = tx;
        state.receiver = rx;
        state.payloadSize = payloadSize;
        state.sendPlan.start({std::chrono::milliseconds(interval), frameCount}, now);
    };
    if (direction == 0 || direction == 2)
        activate(0, 0, 1);
    if (direction == 1)
        activate(0, 1, 0);
    else if (direction == 2)
        activate(1, 1, 0);
    stressRunning_ = true;
    awaitingReceive_ = false;
    ++stressGeneration_;
    return interval;
}

std::optional<SerialLinkVerifier::StressFrame>
SerialLinkVerifier::nextStressFrame(std::size_t slot, Clock::time_point now) const
{
    if (!stressRunning_ || awaitingReceive_ || slot >= directions_.size())
        return std::nullopt;
    const auto& state = directions_[slot];
    if (!state.active || !state.sendPlan.isDue(now))
        return std::nullopt;
    return StressFrame{state.transmitter,
                       testtools::encodeIntegrityFrame(state.nextSequence, state.payloadSize)};
}

void SerialLinkVerifier::recordStressAttempt(std::size_t slot,
                                             bool succeeded,
                                             Clock::time_point now)
{
    if (!stressRunning_ || awaitingReceive_ || slot >= directions_.size() ||
        !directions_[slot].active)
        return;
    auto& state = directions_[slot];
    state.sendPlan.recordAttempt(succeeded, now);
    if (succeeded)
        ++state.nextSequence;
}

bool SerialLinkVerifier::beginReceiveWait(bool queuesDrained)
{
    if (!stressRunning_ || awaitingReceive_ || !queuesDrained)
        return false;
    for (const auto& direction : directions_)
        if (direction.active && direction.sendPlan.isRunning())
            return false;
    awaitingReceive_ = true;
    return true;
}

void SerialLinkVerifier::stopStress()
{
    for (auto& direction : directions_)
        if (direction.active)
            direction.sendPlan.stop();
    stressRunning_ = false;
    awaitingReceive_ = false;
}
} // namespace testtools
