#include "core/serial/async_serial_sender.h"

#include <algorithm>
#include <utility>

namespace serialdemo {

AsyncSerialSender::~AsyncSerialSender()
{
    stop(false);
}

void AsyncSerialSender::start(Writer writer, Completion completion,
                              std::size_t maximumQueuedBytes)
{
    stop(false);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        writer_ = std::move(writer);
        completion_ = std::move(completion);
        maximumQueuedBytes_ = maximumQueuedBytes;
        queuedBytes_ = 0;
        inFlight_ = 0;
        accepting_ = true;
        stopping_ = false;
        drain_ = false;
    }
    worker_ = std::thread(&AsyncSerialSender::workerMain, this);
}

void AsyncSerialSender::stop(bool drain)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!worker_.joinable()) {
            accepting_ = false;
            writer_ = {};
            completion_ = {};
            return;
        }
        accepting_ = false;
        stopping_ = true;
        drain_ = drain;
        if (!drain_) {
            queue_.clear();
            queuedBytes_ = 0;
        }
    }
    condition_.notify_all();
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
        worker_.join();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        queuedBytes_ = 0;
        inFlight_ = 0;
        writer_ = {};
        completion_ = {};
        stopping_ = false;
        drain_ = false;
    }
}

bool AsyncSerialSender::enqueue(ByteBuffer data, std::uint64_t tag)
{
    if (data.empty()) {
        return true;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!accepting_ || data.size() > maximumQueuedBytes_
            || queuedBytes_ > maximumQueuedBytes_ - data.size()) {
            return false;
        }
        queuedBytes_ += data.size();
        queue_.push_back({std::move(data), tag});
    }
    condition_.notify_one();
    return true;
}

bool AsyncSerialSender::isRunning() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return accepting_;
}

std::size_t AsyncSerialSender::pendingCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size() + inFlight_;
}

void AsyncSerialSender::workerMain()
{
    while (true) {
        Item item;
        Writer writer;
        Completion completion;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_ && (!drain_ || queue_.empty())) {
                break;
            }
            item = std::move(queue_.front());
            queue_.pop_front();
            ++inFlight_;
            writer = writer_;
            completion = completion_;
        }

        bool succeeded = static_cast<bool>(writer);
        constexpr std::size_t WriteChunkSize = 1024;
        for (std::size_t offset = 0; succeeded && offset < item.data.size();
             offset += WriteChunkSize) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stopping_ && !drain_) {
                    succeeded = false;
                    break;
                }
            }
            const std::size_t chunkSize = std::min(WriteChunkSize, item.data.size() - offset);
            ByteBuffer chunk(item.data.begin() + static_cast<std::ptrdiff_t>(offset),
                             item.data.begin() + static_cast<std::ptrdiff_t>(offset + chunkSize));
            succeeded = writer(chunk);
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queuedBytes_ -= item.data.size();
            --inFlight_;
        }
        if (completion) {
            completion(item.data, item.tag, succeeded);
        }
    }
}

} // namespace serialdemo
