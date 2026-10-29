#include "application/serial/serial_receive_queue.h"
#include <algorithm>
#include <utility>

namespace testtools {

SerialReceiveQueue::SerialReceiveQueue(std::size_t maxBytes, std::size_t maxChunks)
    : maxBytes_(std::max(std::size_t{1}, maxBytes)), maxChunks_(std::max(std::size_t{1}, maxChunks)) {}

bool SerialReceiveQueue::push(std::uint64_t generation, const serialdemo::ByteBuffer& data)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation < generation_ || data.empty()) return false;
    if (generation != generation_) {
        generation_ = generation;
        queue_.clear(); bytes_ = 0; droppedBytes_ = droppedChunks_ = 0;
        // 已投递的通知仍可消费新代次，避免断开/重连时重复排队。
    }
    while (!queue_.empty() && (data.size() > maxBytes_ - bytes_ || queue_.size() >= maxChunks_)) {
        droppedBytes_ += queue_.front().size(); ++droppedChunks_;
        bytes_ -= queue_.front().size(); queue_.pop_front();
    }
    if (data.size() > maxBytes_) {
        droppedBytes_ += data.size(); ++droppedChunks_;
    } else {
        queue_.push_back(data); bytes_ += data.size();
    }
    const bool notify = !pending_;
    pending_ = true;
    return notify;
}

SerialReceiveBatch SerialReceiveQueue::take(std::size_t maxBytes, std::size_t maxChunks)
{
    std::lock_guard<std::mutex> lock(mutex_);
    SerialReceiveBatch result;
    result.generation = generation_;
    result.droppedBytes = droppedBytes_; result.droppedChunks = droppedChunks_;
    droppedBytes_ = droppedChunks_ = 0;
    std::size_t consumed = 0;
    while (!queue_.empty() && (result.chunks.empty() ||
           (result.chunks.size() < maxChunks && consumed + queue_.front().size() <= maxBytes))) {
        consumed += queue_.front().size();
        result.chunks.push_back(std::move(queue_.front())); queue_.pop_front();
    }
    bytes_ -= consumed;
    result.more = !queue_.empty(); pending_ = result.more;
    return result;
}

}
