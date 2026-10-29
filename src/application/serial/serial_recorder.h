#pragma once

#include "core/serial/serial_types.h"
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace testtools {

enum class SerialRecordFormat { TaggedText, RawRx };
enum class SerialRecordState { Idle, Starting, Recording, Stopping, Completed, Failed };

struct SerialRecordOptions {
    std::filesystem::path path;
    SerialRecordFormat format = SerialRecordFormat::TaggedText;
    bool overwrite = false;
    std::size_t maxQueuedBytes = 4 * 1024 * 1024;
    std::size_t maxQueuedRecords = 8192;
};

struct SerialRecordSnapshot {
    SerialRecordState state = SerialRecordState::Idle;
    bool busy = false;
    std::uint64_t id = 0, generation = 0;
    SerialRecordFormat format = SerialRecordFormat::TaggedText;
    std::filesystem::path path;
    std::uint64_t acceptedDataBytes = 0, writtenFileBytes = 0, rejectedDataBytes = 0;
    std::size_t pendingBytes = 0;
    std::int64_t startedAtMs = 0, endedAtMs = 0;
    std::string error, endReason;
};

// 固定写入接口便于验证磁盘失败与慢写；实现只能由录制工作线程调用。
class ISerialRecordSink {
  public:
    virtual ~ISerialRecordSink() = default;
    virtual bool open(const std::filesystem::path& path, std::string& error) = 0;
    virtual bool write(const char* bytes, std::size_t count, std::string& error) = 0;
    virtual bool flush(std::string& error) = 0;
    virtual bool close(std::string& error) = 0;
};

class SerialRecorder final {
  public:
    using SinkFactory = std::function<std::unique_ptr<ISerialRecordSink>()>;
    explicit SerialRecorder(SinkFactory factory = {});
    ~SerialRecorder();
    SerialRecorder(const SerialRecorder&) = delete;
    SerialRecorder& operator=(const SerialRecorder&) = delete;

    bool start(const SerialRecordOptions& options, std::uint64_t generation,
               const serialdemo::SerialConfig& config, std::string& error);
    void push(std::uint64_t generation, bool transmitted, const serialdemo::ByteBuffer& data);
    void stop(const std::string& reason = "用户停止");
    void waitIdle();
    SerialRecordSnapshot snapshot() const;
    static std::filesystem::path metadataPath(const std::filesystem::path& path);

  private:
    struct Chunk {
        bool transmitted = false;
        std::int64_t timeMs = 0;
        serialdemo::ByteBuffer bytes;
    };
    void run();
    void recordOne();
    bool writeMetadata(std::string& error);
    SinkFactory factory_;
    mutable std::mutex mutex_;
    std::condition_variable wake_, idle_;
    std::thread worker_;
    bool shutdown_ = false, startPending_ = false, stopRequested_ = false;
    std::deque<Chunk> queue_;
    SerialRecordOptions options_;
    serialdemo::SerialConfig config_;
    SerialRecordSnapshot status_;
};

}
