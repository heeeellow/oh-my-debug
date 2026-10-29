#pragma once

#include "core/serial/serial_types.h"
#include <deque>
#include <mutex>
#include <vector>

namespace testtools {

struct SerialReceiveBatch {
    std::uint64_t generation = 0;
    std::vector<serialdemo::ByteBuffer> chunks;
    std::uint64_t droppedBytes = 0, droppedChunks = 0;
    bool more = false;
};

// 显示投递队列：保留接收批次边界，最多保留一个待处理通知，满时淘汰最早批次。
class SerialReceiveQueue final {
  public:
    explicit SerialReceiveQueue(std::size_t maxBytes = 1024 * 1024,
                                std::size_t maxChunks = 2048);
    bool push(std::uint64_t generation, const serialdemo::ByteBuffer& data);
    SerialReceiveBatch take(std::size_t maxBytes = 128 * 1024, std::size_t maxChunks = 128);
  private:
    std::mutex mutex_;
    const std::size_t maxBytes_, maxChunks_;
    std::size_t bytes_ = 0;
    std::uint64_t generation_ = 0, droppedBytes_ = 0, droppedChunks_ = 0;
    bool pending_ = false;
    std::deque<serialdemo::ByteBuffer> queue_;
};

}
