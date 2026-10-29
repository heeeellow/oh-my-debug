#include "application/serial/serial_session.h"

#include <utility>

namespace testtools {

SerialSession::~SerialSession()
{
    close();
    setDataCallback({});
    setStatusCallback({});
    setSendCallback({});
}

void SerialSession::setDataCallback(DataCallback callback)
{
    std::lock_guard<std::mutex> lock(callbackMutex_);
    dataCallback_ = std::move(callback);
}

void SerialSession::setStatusCallback(StatusCallback callback)
{
    std::lock_guard<std::mutex> lock(callbackMutex_);
    statusCallback_ = std::move(callback);
}

void SerialSession::setSendCallback(SendCallback callback)
{
    std::lock_guard<std::mutex> lock(callbackMutex_);
    sendCallback_ = std::move(callback);
}

std::uint64_t SerialSession::open(std::unique_ptr<serialdemo::ISerialTransport> transport,
                                  const serialdemo::SerialConfig& config)
{
    close();
    if (!transport) {
        return generation();
    }

    const auto sharedTransport =
        std::shared_ptr<serialdemo::ISerialTransport>(std::move(transport));
    std::uint64_t currentGeneration = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        currentGeneration = ++generation_;
        transport_ = sharedTransport;
        config_ = config;
        state_ = SerialSessionState::Connecting;
        closing_ = false;
    }

    sharedTransport->setDataCallback([this, currentGeneration](const serialdemo::ByteBuffer& data) {
        handleData(currentGeneration, data);
    });
    sharedTransport->setStatusCallback(
        [this, currentGeneration](serialdemo::TransportState state, const std::string& message) {
            handleStatus(currentGeneration, state, message);
        });

    sender_.start(
        [this, currentGeneration](const serialdemo::ByteBuffer& data) {
            std::shared_ptr<serialdemo::ISerialTransport> currentTransport;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (generation_ != currentGeneration) {
                    return false;
                }
                currentTransport = transport_;
            }
            return currentTransport && currentTransport->isOpen() && currentTransport->write(data);
        },
        [this,
         currentGeneration](const serialdemo::ByteBuffer& data, std::uint64_t tag, bool succeeded) {
            handleSendCompletion(currentGeneration, data, tag, succeeded);
        });

    sharedTransport->open(config);
    return currentGeneration;
}

std::uint64_t SerialSession::closeAsync(CloseCallback callback)
{
    if (closeWorker_.joinable()) {
        closeWorker_.join();
    }

    std::shared_ptr<serialdemo::ISerialTransport> transport;
    std::uint64_t closeGeneration = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        recorder_.stop("串口断开");
        if (!transport_) {
            state_ = SerialSessionState::Disconnected;
            closing_ = false;
            return generation_;
        }
        closeGeneration = ++generation_;
        state_ = SerialSessionState::Disconnecting;
        closing_ = true;
        scheduledSendPlan_.stop();
        scheduledSendPayload_.clear();
        transport = std::move(transport_);
    }

    transport->setDataCallback({});
    transport->setStatusCallback({});
    closeWorker_ = std::thread([this,
                                transport = std::move(transport),
                                closeGeneration,
                                callback = std::move(callback)]() mutable {
        sender_.stop(false);
        transport->close();
        recorder_.waitIdle();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation_ == closeGeneration) {
                state_ = SerialSessionState::Disconnected;
                closing_ = false;
            }
        }
        if (callback) {
            callback(closeGeneration);
        }
    });
    return closeGeneration;
}

void SerialSession::close()
{
    recorder_.stop("串口断开");
    recorder_.waitIdle();
    if (closeWorker_.joinable()) {
        closeWorker_.join();
    }

    std::shared_ptr<serialdemo::ISerialTransport> transport;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!transport_) {
            state_ = SerialSessionState::Disconnected;
            closing_ = false;
            return;
        }
        ++generation_;
        state_ = SerialSessionState::Disconnecting;
        closing_ = true;
        scheduledSendPlan_.stop();
        scheduledSendPayload_.clear();
        transport = std::move(transport_);
    }

    transport->setDataCallback({});
    transport->setStatusCallback({});
    sender_.stop(false);
    transport->close();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = SerialSessionState::Disconnected;
        closing_ = false;
    }
}

bool SerialSession::send(serialdemo::ByteBuffer data, std::uint64_t tag)
{
    if (data.empty()) {
        return true;
    }
    std::shared_ptr<serialdemo::ISerialTransport> transport;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        transport = transport_;
    }
    if (!transport || !transport->isOpen() || !sender_.isRunning()) {
        return false;
    }
    return sender_.enqueue(std::move(data), tag);
}

bool SerialSession::supportsControlLine(serialdemo::SerialControlLine line) const
{
    std::shared_ptr<serialdemo::ISerialTransport> transport;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != SerialSessionState::Connected || closing_) return false;
        transport = transport_;
    }
    return transport && transport->supportsControlLine(line);
}

bool SerialSession::setControlLine(std::uint64_t expectedGeneration,
                                   serialdemo::SerialControlLine line, bool enabled,
                                   std::string &error)
{
    std::shared_ptr<serialdemo::ISerialTransport> transport;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // 排队到达的点击操作绝不能作用到随后替换的新连接代次上。
        if (generation_ != expectedGeneration || state_ != SerialSessionState::Connected ||
            closing_ || scheduledSendPlan_.isRunning()) {
            error = "连接已变化或正在计划发送，未修改控制线";
            return false;
        }
        transport = transport_;
    }
    if (!transport || !transport->supportsControlLine(line)) {
        error = "当前串口或流控设置不支持此控制线";
        return false;
    }
    error.clear();
    return transport->setControlLine(line, enabled, error);
}

bool SerialSession::startScheduledSend(serialdemo::ByteBuffer payload,
                                       const serialdemo::PeriodicSendConfig& config,
                                       std::uint64_t tag,
                                       serialdemo::PeriodicSendPlan::Clock::time_point now)
{
    if (payload.empty() || !isOpen()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!transport_ || !transport_->isOpen() || scheduledSendPlan_.isRunning() ||
        !scheduledSendPlan_.start(config, now)) {
        return false;
    }
    scheduledSendPayload_ = std::move(payload);
    ++scheduledSendId_;
    scheduledSendTag_ = tag;
    return true;
}

ScheduledSendSnapshot SerialSession::stopScheduledSend()
{
    std::lock_guard<std::mutex> lock(mutex_);
    ScheduledSendSnapshot result{scheduledSendPlan_.isRunning(),
                                 scheduledSendPlan_.attempts(),
                                 scheduledSendPlan_.successes(), scheduledSendId_};
    scheduledSendPlan_.stop();
    scheduledSendPayload_.clear();
    return result;
}

ScheduledSendTick
SerialSession::processScheduledSend(serialdemo::PeriodicSendPlan::Clock::time_point now)
{
    serialdemo::ByteBuffer payload;
    std::uint64_t tag = 0;
    std::uint64_t currentGeneration = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!scheduledSendPlan_.isDue(now)) {
            return ScheduledSendTick::Idle;
        }
        payload = scheduledSendPayload_;
        tag = scheduledSendTag_;
        currentGeneration = generation_;
    }

    const bool queued = send(std::move(payload), tag);
    bool running = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation_ != currentGeneration || !scheduledSendPlan_.isRunning()) {
            return ScheduledSendTick::Failed;
        }
        scheduledSendPlan_.recordAttempt(queued, now);
        running = scheduledSendPlan_.isRunning();
        if (!queued || !running) {
            scheduledSendPayload_.clear();
        }
    }
    if (!queued) {
        return ScheduledSendTick::Failed;
    }
    return running ? ScheduledSendTick::Queued : ScheduledSendTick::Completed;
}

SerialSessionSnapshot SerialSession::snapshot() const
{
    std::shared_ptr<serialdemo::ISerialTransport> transport;
    SerialSessionSnapshot result;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        transport = transport_;
        result.generation = generation_;
        result.state = state_;
        result.active = static_cast<bool>(transport_) || closing_;
        result.receivedBytes = receivedBytes_;
        result.sentBytes = sentBytes_;
        result.scheduledSend = {scheduledSendPlan_.isRunning(),
                                scheduledSendPlan_.attempts(),
                                scheduledSendPlan_.successes(), scheduledSendId_};
    }
    result.open = transport && transport->isOpen();
    result.pendingWrites = sender_.pendingCount();
    return result;
}

void SerialSession::resetCounters()
{
    std::lock_guard<std::mutex> lock(mutex_);
    receivedBytes_ = 0;
    sentBytes_ = 0;
}

std::uint64_t SerialSession::generation() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_;
}

bool SerialSession::isActive() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<bool>(transport_) || closing_;
}

bool SerialSession::isOpen() const
{
    std::shared_ptr<serialdemo::ISerialTransport> transport;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        transport = transport_;
    }
    return transport && transport->isOpen();
}

bool SerialSession::isCurrentGeneration(std::uint64_t generation) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_ == generation;
}

bool SerialSession::startRecording(const SerialRecordOptions& options,
                                   std::uint64_t expectedGeneration, std::string& error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation_ != expectedGeneration || state_ != SerialSessionState::Connected ||
        closing_ || !transport_ || !transport_->isOpen()) {
        error = "串口连接已变化，未开始录制"; return false;
    }
    return recorder_.start(options, generation_, config_, error);
}

void SerialSession::stopRecording(const std::string& reason, std::uint64_t expectedId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!expectedId || recorder_.snapshot().id == expectedId) recorder_.stop(reason);
}

SerialRecordSnapshot SerialSession::recordingSnapshot() const { return recorder_.snapshot(); }

void SerialSession::handleData(std::uint64_t generation, const serialdemo::ByteBuffer& data)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation_ != generation) {
            return;
        }
        receivedBytes_ += data.size();
        recorder_.push(generation, false, data);
    }
    DataCallback callback;
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        callback = dataCallback_;
    }
    if (callback && isCurrentGeneration(generation)) {
        callback(generation, data);
    }
}

void SerialSession::handleStatus(std::uint64_t generation,
                                 serialdemo::TransportState state,
                                 const std::string& message)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation_ != generation) {
            return;
        }
        if (state == serialdemo::TransportState::Connected) {
            state_ = SerialSessionState::Connected;
        } else if (state != serialdemo::TransportState::Progress) {
            recorder_.stop("串口断开");
            state_ = SerialSessionState::Disconnected;
            scheduledSendPlan_.stop();
            scheduledSendPayload_.clear();
        }
    }
    StatusCallback callback;
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        callback = statusCallback_;
    }
    if (callback && isCurrentGeneration(generation)) {
        callback(generation, state, message);
    }
}

void SerialSession::handleSendCompletion(std::uint64_t generation,
                                         const serialdemo::ByteBuffer& data,
                                         std::uint64_t tag,
                                         bool succeeded)
{
    bool scheduledSendStopped = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation_ != generation) {
            return;
        }
        if (succeeded) {
            sentBytes_ += data.size();
            recorder_.push(generation, true, data);
        } else {
            scheduledSendStopped = scheduledSendPlan_.isRunning();
            scheduledSendPlan_.stop();
            scheduledSendPayload_.clear();
        }
    }
    SendCallback callback;
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        callback = sendCallback_;
    }
    if (callback && isCurrentGeneration(generation)) {
        callback(generation, data, tag, succeeded, scheduledSendStopped);
    }
}

} // namespace testtools
