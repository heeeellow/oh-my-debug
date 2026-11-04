#pragma once

#include "core/serial/async_serial_sender.h"
#include "core/serial/serial_test_engine.h"
#include "core/serial/serial_transport.h"
#include "application/serial/serial_recorder.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace testtools {

enum class SerialSessionState { Disconnected, Connecting, Connected, Disconnecting };

struct ScheduledSendSnapshot {
    bool running = false;
    std::uint64_t attempts = 0;
    std::uint64_t submitted = 0;
    std::uint64_t id = 0;
};

struct SerialSessionSnapshot {
    std::uint64_t generation = 0;
    SerialSessionState state = SerialSessionState::Disconnected;
    bool active = false;
    bool open = false;
    std::uint64_t receivedBytes = 0;
    std::uint64_t sentBytes = 0;
    std::size_t pendingWrites = 0;
    ScheduledSendSnapshot scheduledSend;
};

enum class ScheduledSendTick { Idle, Queued, Completed, Failed };

// 单路串口会话编排器：管理连接代次（Generation）、异步发送队列、定时发送计划与收发字节计数。
class SerialSession {
  public:
    using DataCallback =
        std::function<void(std::uint64_t generation, const testtools::ByteBuffer& data)>;
    using StatusCallback = std::function<void(
        std::uint64_t generation, testtools::TransportState state, const std::string& message)>;
    using SendCallback = std::function<void(std::uint64_t generation,
                                            const testtools::ByteBuffer& data,
                                            std::uint64_t tag,
                                            bool succeeded,
                                            bool scheduledSendStopped)>;
    using CloseCallback = std::function<void(std::uint64_t generation)>;

    SerialSession() = default;
    ~SerialSession();
    SerialSession(const SerialSession&) = delete;
    SerialSession& operator=(const SerialSession&) = delete;

    void setDataCallback(DataCallback callback);
    void setStatusCallback(StatusCallback callback);
    void setSendCallback(SendCallback callback);

    std::uint64_t open(std::unique_ptr<testtools::ISerialTransport> transport,
                       const testtools::SerialConfig& config);
    std::uint64_t closeAsync(CloseCallback callback = {});
    void close();

    bool send(testtools::ByteBuffer data, std::uint64_t tag = 0);
    bool supportsControlLine(testtools::SerialControlLine line) const;
    bool setControlLine(std::uint64_t expectedGeneration, testtools::SerialControlLine line,
                         bool enabled, std::string &error);
    bool startScheduledSend(testtools::ByteBuffer payload,
                            const testtools::PeriodicSendConfig& config,
                            std::uint64_t tag = 0,
                            testtools::PeriodicSendPlan::Clock::time_point now =
                                testtools::PeriodicSendPlan::Clock::now());
    ScheduledSendSnapshot stopScheduledSend();
    ScheduledSendTick processScheduledSend(testtools::PeriodicSendPlan::Clock::time_point now =
                                               testtools::PeriodicSendPlan::Clock::now());

    SerialSessionSnapshot snapshot() const;
    void resetCounters();
    std::uint64_t generation() const;
    bool isActive() const;
    bool isOpen() const;

    bool startRecording(const SerialRecordOptions& options, std::uint64_t expectedGeneration,
                        std::string& error);
    void stopRecording(const std::string& reason = "用户停止", std::uint64_t expectedId = 0);
    SerialRecordSnapshot recordingSnapshot() const;

  private:
    bool isCurrentGeneration(std::uint64_t generation) const;
    void handleData(std::uint64_t generation, const testtools::ByteBuffer& data);
    void handleStatus(std::uint64_t generation,
                      testtools::TransportState state,
                      const std::string& message);
    void handleSendCompletion(std::uint64_t generation,
                              const testtools::ByteBuffer& data,
                              std::uint64_t tag,
                              bool succeeded);

    mutable std::mutex mutex_;
    std::shared_ptr<testtools::ISerialTransport> transport_;
    testtools::AsyncSerialSender sender_;
    testtools::PeriodicSendPlan scheduledSendPlan_;
    testtools::ByteBuffer scheduledSendPayload_;
    std::uint64_t scheduledSendTag_ = 0;
    std::uint64_t scheduledSendId_ = 0;
    std::uint64_t generation_ = 0;
    SerialSessionState state_ = SerialSessionState::Disconnected;
    std::uint64_t receivedBytes_ = 0;
    std::uint64_t sentBytes_ = 0;
    bool closing_ = false;
    std::thread closeWorker_;
    testtools::SerialConfig config_;
    SerialRecorder recorder_;

    mutable std::mutex callbackMutex_;
    DataCallback dataCallback_;
    StatusCallback statusCallback_;
    SendCallback sendCallback_;
};

} // namespace testtools
