#pragma once

#include "core/serial/serial_types.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace serialdemo {

class AsyncSerialSender {
public:
    using Writer = std::function<bool(const ByteBuffer &)>;
    using Completion = std::function<void(const ByteBuffer &, std::uint64_t tag, bool succeeded)>;

    AsyncSerialSender() = default;
    ~AsyncSerialSender();
    AsyncSerialSender(const AsyncSerialSender &) = delete;
    AsyncSerialSender &operator=(const AsyncSerialSender &) = delete;

    void start(Writer writer, Completion completion, std::size_t maximumQueuedBytes = 8 * 1024 * 1024);
    void stop(bool drain = false);
    bool enqueue(ByteBuffer data, std::uint64_t tag = 0);
    bool isRunning() const;
    std::size_t pendingCount() const;

private:
    struct Item {
        ByteBuffer data;
        std::uint64_t tag = 0;
    };

    void workerMain();

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<Item> queue_;
    Writer writer_;
    Completion completion_;
    std::thread worker_;
    std::size_t maximumQueuedBytes_ = 0;
    std::size_t queuedBytes_ = 0;
    std::size_t inFlight_ = 0;
    bool accepting_ = false;
    bool stopping_ = false;
    bool drain_ = false;
};

} // namespace serialdemo
