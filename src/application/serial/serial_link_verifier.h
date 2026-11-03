#pragma once
#include "core/serial/codec.h"
#include "core/serial/serial_test_engine.h"
#include <array>
#include <optional>

namespace testtools {

// 双端验证和可靠性压测各自拥有状态；普通计划发送仍由 SerialSession 管理。
class SerialLinkVerifier {
  public:
    using Clock = serialdemo::PeriodicSendPlan::Clock;
    struct ProbeStatus {
        bool running = false;
        bool aToBPassed = false;
        bool bToAPassed = false;
        std::uint64_t generation = 0;
    };
    struct StressDirection {
        bool active = false;
        int transmitter = 0;
        int receiver = 1;
        std::size_t payloadSize = 256;
        std::uint32_t nextSequence = 0;
        serialdemo::PeriodicSendPlan sendPlan;
        serialdemo::IntegrityStreamVerifier verifier;
    };
    struct StressFrame {
        int transmitter = 0;
        serialdemo::ByteBuffer bytes;
    };

    std::array<serialdemo::ByteBuffer, 2>
    startProbe(const std::string& token, const std::array<serialdemo::LineEnding, 2>& endings);
    const ProbeStatus& probe() const
    {
        return probe_;
    }
    bool expireProbe(std::uint64_t generation);
    void stopProbe();
    // 接收状态变化时返回 true，供界面更新双向结果；压测字节不进入普通日志。
    bool receive(int receiver, const serialdemo::ByteBuffer& bytes);

    static int stressInterval(int direction,
                              std::size_t payloadSize,
                              int requestedIntervalMs,
                              const std::array<serialdemo::SerialConfig, 2>& configs);
    int startStress(int direction,
                    std::uint64_t frameCount,
                    std::size_t payloadSize,
                    int requestedIntervalMs,
                    const std::array<serialdemo::SerialConfig, 2>& configs,
                    Clock::time_point now = Clock::now());
    bool stressRunning() const
    {
        return stressRunning_;
    }
    bool awaitingReceive() const
    {
        return awaitingReceive_;
    }
    std::uint64_t stressGeneration() const
    {
        return stressGeneration_;
    }
    const std::array<StressDirection, 2>& directions() const
    {
        return directions_;
    }
    std::optional<StressFrame> nextStressFrame(std::size_t slot, Clock::time_point now) const;
    void recordStressAttempt(std::size_t slot, bool succeeded, Clock::time_point now);
    bool beginReceiveWait(bool queuesDrained);
    void stopStress();

  private:
    ProbeStatus probe_;
    std::array<std::string, 2> expectedFrames_;
    std::array<std::string, 2> detectionBuffers_;
    std::array<StressDirection, 2> directions_;
    bool stressRunning_ = false;
    bool awaitingReceive_ = false;
    std::uint64_t stressGeneration_ = 0;
};
} // namespace testtools
