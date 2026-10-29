#include "application/serial/serial_recorder.h"
#include "core/protocol/json_value.h"

#include <array>
#include <chrono>
#include <ctime>
#include <exception>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>

namespace testtools {
namespace {
std::int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

class FileSink final : public ISerialRecordSink {
    std::ofstream file_;
  public:
    bool open(const std::filesystem::path& path, std::string& error) override
    {
        file_.open(path, std::ios::binary | std::ios::trunc);
        if (!file_) error = "无法打开录制文件";
        return bool(file_);
    }
    bool write(const char* bytes, std::size_t count, std::string& error) override
    {
        file_.write(bytes, static_cast<std::streamsize>(count));
        if (!file_) error = "录制写入失败，请检查磁盘空间和文件权限";
        return bool(file_);
    }
    bool flush(std::string& error) override
    {
        file_.flush();
        if (!file_) error = "录制刷盘失败";
        return bool(file_);
    }
    bool close(std::string& error) override
    {
        if (file_.is_open()) file_.close();
        if (file_.fail()) { error = "录制文件关闭失败"; return false; }
        return true;
    }
};

std::string prefix(std::int64_t milliseconds, bool transmitted)
{
    const auto seconds = static_cast<std::time_t>(milliseconds / 1000);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    std::ostringstream out;
    out << '[' << std::put_time(&local, "%Y-%m-%d %H:%M:%S") << '.'
        << std::setw(3) << std::setfill('0') << milliseconds % 1000 << "] "
        << (transmitted ? "TX  " : "RX  ");
    return out.str();
}

// 只保留不足四字节的 UTF-8 尾部；非法字节和控制字节可读转义，正文不等待换行。
std::string decode(const serialdemo::ByteBuffer& bytes, std::string& carry, bool finish = false)
{
    std::string input = std::move(carry);
    carry.clear();
    input.append(bytes.begin(), bytes.end());
    std::string result;
    const char* hex = "0123456789ABCDEF";
    const auto escape = [&](unsigned char c) {
        result += "\\x"; result += hex[c >> 4]; result += hex[c & 15];
    };
    for (std::size_t i = 0; i < input.size();) {
        const auto c = static_cast<unsigned char>(input[i]);
        if (c < 0x80) {
            if ((c >= 0x20 && c != 0x7f) || c == '\r' || c == '\n' || c == '\t') result += input[i];
            else escape(c);
            ++i; continue;
        }
        const std::size_t length = c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3
                                    : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
        if (!length) { escape(c); ++i; continue; }
        bool valid = true;
        for (std::size_t j = 1; j < length && i + j < input.size(); ++j)
            valid = valid && (static_cast<unsigned char>(input[i + j]) & 0xc0) == 0x80;
        if (i + 1 < input.size()) {
            const auto second = static_cast<unsigned char>(input[i + 1]);
            valid = valid && !(c == 0xe0 && second < 0xa0) && !(c == 0xed && second >= 0xa0) &&
                    !(c == 0xf0 && second < 0x90) && !(c == 0xf4 && second >= 0x90);
        }
        if (!valid) { escape(c); ++i; continue; }
        if (i + length > input.size()) {
            if (!finish) { carry = input.substr(i); break; }
            escape(c); ++i; continue;
        }
        result.append(input, i, length); i += length;
    }
    return result;
}
}

SerialRecorder::SerialRecorder(SinkFactory factory) : factory_(std::move(factory))
{
    if (!factory_) factory_ = [] { return std::make_unique<FileSink>(); };
}

SerialRecorder::~SerialRecorder()
{
    stop("应用退出");
    { std::lock_guard<std::mutex> lock(mutex_); shutdown_ = true; }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

std::filesystem::path SerialRecorder::metadataPath(const std::filesystem::path& path)
{
    auto result = path; result += ".json"; return result;
}

bool SerialRecorder::start(const SerialRecordOptions& options, std::uint64_t generation,
                           const serialdemo::SerialConfig& config, std::string& error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    error.clear();
    if (status_.busy || shutdown_) { error = "上次录制仍在保存，请稍后重试"; return false; }
    if (options.path.empty() || (options.format != SerialRecordFormat::TaggedText && options.format != SerialRecordFormat::RawRx) ||
        !options.maxQueuedBytes || options.maxQueuedBytes > 64 * 1024 * 1024 ||
        !options.maxQueuedRecords || options.maxQueuedRecords > 65536) {
        error = "录制路径或队列上限无效"; return false;
    }
    const auto nextId = status_.id + 1;
    status_ = {};
    status_.id = nextId; status_.generation = generation;
    status_.state = SerialRecordState::Starting; status_.busy = true;
    status_.format = options.format; status_.path = options.path; status_.startedAtMs = nowMs();
    options_ = options; config_ = config; queue_.clear();
    stopRequested_ = false; startPending_ = true;
    if (!worker_.joinable()) {
        try { worker_ = std::thread([this] { run(); }); }
        catch (const std::exception& e) {
            error = std::string("无法启动录制线程：") + e.what();
            status_.error = error; status_.state = SerialRecordState::Failed;
            status_.endedAtMs = nowMs();
            status_.busy = false; startPending_ = false;
            return false;
        }
    }
    wake_.notify_all();
    return true;
}

void SerialRecorder::push(std::uint64_t generation, bool transmitted, const serialdemo::ByteBuffer& data)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!status_.busy || stopRequested_ || generation != status_.generation || data.empty() ||
        (options_.format == SerialRecordFormat::RawRx && transmitted)) return;
    if (data.size() > options_.maxQueuedBytes - status_.pendingBytes || queue_.size() >= options_.maxQueuedRecords) {
        status_.rejectedDataBytes += data.size();
        status_.error = "录制失败：写入积压超过上限";
        status_.endReason = "写入积压";
        status_.state = SerialRecordState::Stopping; stopRequested_ = true;
    } else {
        queue_.push_back({transmitted, nowMs(), data});
        status_.pendingBytes += data.size(); status_.acceptedDataBytes += data.size();
    }
    wake_.notify_one();
}

void SerialRecorder::stop(const std::string& reason)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_.busy && !stopRequested_) {
        stopRequested_ = true; status_.state = SerialRecordState::Stopping;
        status_.endReason = reason; wake_.notify_one();
    }
}

void SerialRecorder::waitIdle()
{
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [this] { return !status_.busy; });
}

SerialRecordSnapshot SerialRecorder::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_); return status_;
}

bool SerialRecorder::writeMetadata(std::string& error)
{
    const auto status = snapshot();
    const auto document = JsonValue::object({
        {"schema_version", JsonValue::integer(1)},
        {"complete", JsonValue::boolean(status.state == SerialRecordState::Completed)},
        {"recording_id", JsonValue::integer(static_cast<std::int64_t>(status.id))},
        {"generation", JsonValue::integer(static_cast<std::int64_t>(status.generation))},
        {"format", JsonValue::string(status.format == SerialRecordFormat::RawRx ? "raw_rx" : "tagged_text")},
        {"endpoint", JsonValue::string(config_.endpoint)},
        {"source", JsonValue::string(config_.adbSerial.empty() ? "local" : "agent")},
        {"adb_serial", JsonValue::string(config_.adbSerial)},
        {"baud_rate", JsonValue::integer(config_.baudRate)},
        {"data_bits", JsonValue::integer(config_.dataBits)},
        {"parity", JsonValue::integer(static_cast<int>(config_.parity))},
        {"stop_bits", JsonValue::integer(static_cast<int>(config_.stopBits))},
        {"flow_control", JsonValue::integer(static_cast<int>(config_.flowControl))},
        {"started_at_ms", JsonValue::integer(status.startedAtMs)},
        {"ended_at_ms", JsonValue::integer(status.endedAtMs)},
        {"accepted_data_bytes", JsonValue::integer(static_cast<std::int64_t>(status.acceptedDataBytes))},
        {"written_file_bytes", JsonValue::integer(static_cast<std::int64_t>(status.writtenFileBytes))},
        {"rejected_data_bytes", JsonValue::integer(static_cast<std::int64_t>(status.rejectedDataBytes))},
        {"error", JsonValue::string(status.error)},
        {"end_reason", JsonValue::string(status.endReason)}
    });
    std::string json; JsonError jsonError;
    if (!serializeJson(document, json, jsonError)) { error = "无法生成录制元数据"; return false; }
    std::ofstream file(metadataPath(status.path), std::ios::binary | std::ios::trunc);
    file.write(json.data(), static_cast<std::streamsize>(json.size()));
    file.flush(); file.close();
    if (!file) { error = "无法保存录制元数据"; return false; }
    return true;
}

void SerialRecorder::run()
{
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return shutdown_ || startPending_; });
            if (!startPending_) return;
            startPending_ = false;
        }
        recordOne();
    }
}

void SerialRecorder::recordOne()
{
    std::string error;
    std::unique_ptr<ISerialRecordSink> sink;
    std::array<std::string, 2> carry;
    std::array<std::int64_t, 2> carryTime{};
    bool opened = false;
    try {
        std::error_code ec;
        const bool exists = std::filesystem::exists(options_.path, ec);
        if (ec) error = "无法检查录制目标";
        const bool metaExists = std::filesystem::exists(metadataPath(options_.path), ec);
        if (ec) error = "无法检查录制元数据目标";
        if (!options_.overwrite && (exists || metaExists)) error = "录制文件或元数据已存在，请更换名称";
        if (error.empty()) {
            sink = factory_();
            if (!sink) error = "无法创建录制写入器";
            else {
                opened = sink->open(options_.path, error);
                if (!opened && error.empty()) error = "无法打开录制文件";
            }
        }
        if (opened && error.empty() && writeMetadata(error)) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!stopRequested_) status_.state = SerialRecordState::Recording;
            }
            auto lastFlush = std::chrono::steady_clock::now();
            for (;;) {
                Chunk chunk;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    wake_.wait_for(lock, std::chrono::milliseconds(250), [this] {
                        return shutdown_ || stopRequested_ || !queue_.empty();
                    });
                    if ((stopRequested_ || shutdown_) && queue_.empty()) break;
                    if (!queue_.empty()) {
                        chunk = std::move(queue_.front()); queue_.pop_front();
                        status_.pendingBytes -= chunk.bytes.size();
                    }
                }
                if (!chunk.bytes.empty()) {
                    std::string text;
                    const char* data = reinterpret_cast<const char*>(chunk.bytes.data());
                    std::size_t count = chunk.bytes.size();
                    if (options_.format == SerialRecordFormat::TaggedText) {
                        const auto direction = chunk.transmitted ? 1 : 0;
                        const bool hadCarry = !carry[direction].empty();
                        const auto time = hadCarry ? carryTime[direction] : chunk.timeMs;
                        text = decode(chunk.bytes, carry[direction]);
                        if (!carry[direction].empty() && (!hadCarry || !text.empty())) carryTime[direction] = chunk.timeMs;
                        if (!text.empty()) text = prefix(time, chunk.transmitted) + text + '\n';
                        data = text.data(); count = text.size();
                    }
                    if (!sink->write(data, count, error)) {
                        if (error.empty()) error = "录制写入失败";
                        break;
                    }
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_.writtenFileBytes += count;
                }
                if (std::chrono::steady_clock::now() - lastFlush >= std::chrono::seconds(1)) {
                    if (!sink->flush(error)) {
                        if (error.empty()) error = "录制刷盘失败";
                        break;
                    }
                    lastFlush = std::chrono::steady_clock::now();
                }
            }
            if (error.empty() && options_.format == SerialRecordFormat::TaggedText) {
                for (int direction = 0; direction < 2; ++direction) {
                    auto tail = decode({}, carry[direction], true);
                    if (!tail.empty()) {
                        tail = prefix(carryTime[direction], direction == 1) + tail + '\n';
                        if (!sink->write(tail.data(), tail.size(), error)) {
                            if (error.empty()) error = "录制写入失败";
                            break;
                        }
                        std::lock_guard<std::mutex> lock(mutex_); status_.writtenFileBytes += tail.size();
                    }
                }
            }
            if (error.empty() && !sink->flush(error) && error.empty()) error = "录制刷盘失败";
        }
        if (!error.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            stopRequested_ = true; status_.state = SerialRecordState::Stopping;
            status_.error = error;
        }
        if (opened) {
            std::string closeError;
            if (!sink->close(closeError) && error.empty())
                error = closeError.empty() ? "录制文件关闭失败" : closeError;
        }
    } catch (const std::exception& e) { error = std::string("录制失败：") + e.what(); }
      catch (...) { error = "录制失败：未知写入异常"; }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopRequested_ = true;
        if (!error.empty()) status_.error = error;
        if (!status_.error.empty() && status_.endReason.empty()) status_.endReason = "录制错误";
        status_.endedAtMs = nowMs();
        status_.state = status_.error.empty() ? SerialRecordState::Completed : SerialRecordState::Failed;
        queue_.clear(); status_.pendingBytes = 0;
    }
    // 元数据仍由同一工作线程收尾；完成前保持 busy，阻止重新使用目标。
    if (opened) {
        std::string metaError;
        try { writeMetadata(metaError); } catch (...) { metaError = "录制元数据保存失败"; }
        if (!metaError.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.error = metaError; status_.state = SerialRecordState::Failed;
        }
    }
    { std::lock_guard<std::mutex> lock(mutex_); status_.busy = false; }
    idle_.notify_all();
}

}
