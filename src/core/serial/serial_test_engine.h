#pragma once

#include "core/serial/serial_types.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>

namespace serialdemo {

// 周期发送计划配置，用于定时发送或固定帧数压测。
struct PeriodicSendConfig {
    std::chrono::milliseconds interval{1000};
    std::uint64_t targetCount = 0; // 0 表示持续发送直到手动停止。
};

// 周期发送调度状态机：跟踪下次触发时刻、尝试次数与成功次数。
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

// 完整性压测流统计指标（帧数、丢包、CRC 错帧、重复帧与误码率）。
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

// 根据序号生成确定性的伪随机压测载荷并封装带魔数与 CRC32 的校验帧。
ByteBuffer makeIntegrityPayload(std::uint32_t sequence, std::size_t payloadSize);
ByteBuffer encodeIntegrityFrame(std::uint32_t sequence, std::size_t payloadSize);

// 字节流完整性校验器：支持粘包/半包重同步、序列去重与逐比特误码统计。
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

// 单向压测结果汇总报告（用于 A->B 或 B->A 独立判定）。
struct StressDirectionReport {
    std::uint64_t sentFrames = 0;
    std::uint64_t uniqueFrames = 0;
    std::uint64_t lostFrames = 0;
    std::uint64_t corruptedFrames = 0;
    std::uint64_t framingErrors = 0;
    std::uint64_t bitErrors = 0;
    double bitErrorRate = 0.0;
    bool passed = false;
};

// 根据串口帧格式与波特率估算单帧线速耗时，返回不低于物理线速的安全发送间隔（毫秒）。
int minimumStressIntervalMs(const SerialConfig &config,
                            std::size_t payloadSize,
                            int requestedIntervalMs);

// 将新收到的字节流追加到有界滑动窗口缓冲区，并匹配期望的探测/回环令牌（命中后自动清空 expectedFrame）。
bool appendAndMatchFrame(std::string &buffer,
                         const ByteBuffer &data,
                         std::string &expectedFrame,
                         std::size_t maxBufferBytes = 8192);

// 汇总单个方向的发送计划与接收端完整性统计，给出丢帧数与通过判定。
StressDirectionReport evaluateStressDirection(const PeriodicSendPlan &sendPlan,
                                              const IntegrityStreamVerifier &verifier);

} // namespace serialdemo
