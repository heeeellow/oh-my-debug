#include "application/serial/serial_session.h"
#include "application/serial/serial_receive_queue.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace testtools;
using testtools::ByteBuffer;
namespace {
void check(bool result, const char* message) { if (!result) throw std::runtime_error(message); }
bool await(const std::function<bool()>& predicate)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do { if (predicate()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
    while (std::chrono::steady_clock::now() < until);
    return predicate();
}
std::string read(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
class Transport final : public testtools::ISerialTransport {
  public:
    std::atomic<bool> opened{false}, failWrites{false};
    void open(const testtools::SerialConfig&) override
    { opened = true; publishStatus(testtools::TransportState::Connected, "fixture"); }
    void close() override { opened = false; }
    bool write(const ByteBuffer&) override { return opened && !failWrites; }
    bool isOpen() const override { return opened; }
    void receive(const ByteBuffer& data) { publishData(data); }
    void unplug() { opened = false; publishStatus(testtools::TransportState::Error, "unplug"); }
};
struct SinkState {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, release = false, block = false, blockClose = false, closing = false;
    int failure = 0;
    std::string data;
    std::thread::id caller, writer;
};
class Sink final : public ISerialRecordSink {
    std::shared_ptr<SinkState> state_;
  public:
    explicit Sink(std::shared_ptr<SinkState> state) : state_(std::move(state)) {}
    bool open(const std::filesystem::path&, std::string&) override
    { state_->writer = std::this_thread::get_id(); return state_->failure != 1; }
    bool write(const char* data, std::size_t count, std::string&) override
    {
        std::unique_lock<std::mutex> lock(state_->mutex);
        check(state_->writer == std::this_thread::get_id(), "one writer thread");
        state_->entered = true; state_->changed.notify_all();
        if (state_->block && !state_->changed.wait_for(lock, std::chrono::seconds(5),
            [&] { return state_->release; })) return false;
        if (state_->failure == 2) return false;
        state_->data.append(data, count); return true;
    }
    bool flush(std::string&) override { return state_->failure != 3; }
    bool close(std::string&) override
    {
        std::unique_lock<std::mutex> lock(state_->mutex);
        state_->closing = true; state_->changed.notify_all();
        if (state_->blockClose && !state_->changed.wait_for(lock, std::chrono::seconds(5),
            [&] { return state_->release; })) return false;
        return state_->failure != 4;
    }
};
void testQueue()
{
    SerialReceiveQueue queue(8, 3);
    check(queue.push(4, {1, 2, 3}), "first notification");
    check(!queue.push(4, {4, 5, 6}) && !queue.push(4, {7, 8, 9}), "coalesced notifications");
    auto batch = queue.take(3, 1);
    check(batch.droppedBytes == 3 && batch.droppedChunks == 1 && batch.chunks.size() == 1 &&
          batch.chunks[0] == ByteBuffer({4, 5, 6}) && batch.more, "byte cap drops oldest and bounded drain");
    check(!queue.push(4, {10}), "drain continuation owns pending notification");
    batch = queue.take();
    check(!batch.more && batch.chunks.size() == 2, "preserve original RX chunks");
    check(queue.push(4, ByteBuffer(9, 0)) && queue.take().droppedBytes == 9, "oversize rejected visibly");
    check(queue.push(5, {11}) && !queue.push(4, {99}), "reject old generation");
    check(!queue.push(6, {12}), "new generation reuses pending notification");
    batch = queue.take();
    check(batch.generation == 6 && batch.chunks.size() == 1 && batch.chunks[0][0] == 12 &&
          !batch.droppedBytes, "new connection does not show old data or loss");
    SerialReceiveQueue tiny(1000, 2);
    tiny.push(1, {1}); tiny.push(1, {2}); tiny.push(1, {3});
    check(tiny.take().droppedBytes == 1, "count cap bounds tiny allocations");
}
void testFiles(const std::filesystem::path& root)
{
    SerialSession session;
    SerialRecordOptions options{root / "raw.bin", SerialRecordFormat::RawRx};
    std::string error;
    check(!session.startRecording(options, 0, error), "requires open session");
    auto owned = std::make_unique<Transport>(); auto* transport = owned.get();
    testtools::SerialConfig config; config.endpoint = "COM_FIXTURE";
    const auto generation = session.open(std::move(owned), config);
    check(session.startRecording(options, generation, error), "start raw");
    // 不消费界面回调，验证超过显示缓冲容量的数据仍按原始顺序落盘。
    ByteBuffer input(1200 * 1024);
    for (std::size_t i = 0; i < input.size(); ++i) input[i] = static_cast<std::uint8_t>(i);
    transport->receive(input);
    check(session.send({9, 9}, 0), "send without UI echo");
    check(await([&] { return session.snapshot().sentBytes == 2; }), "TX completed");
    session.stopRecording();
    check(await([&] { return !session.recordingSnapshot().busy; }), "raw drained");
    const auto saved = read(options.path);
    check(saved == std::string(input.begin(), input.end()), "raw exact, no TX or header, larger than display");
    auto meta = read(SerialRecorder::metadataPath(options.path));
    check(meta.find("\"complete\":true") != std::string::npos && meta.find("COM_FIXTURE") != std::string::npos,
          "raw metadata complete and endpoint");
    check(session.isOpen(), "stop recording keeps port");
    const auto firstId = session.recordingSnapshot().id;
    check(session.startRecording(options, generation, error), "existing target checked by worker");
    check(await([&] { return !session.recordingSnapshot().busy; }) &&
          session.recordingSnapshot().state == SerialRecordState::Failed && read(options.path) == saved &&
          read(SerialRecorder::metadataPath(options.path)) == meta, "existing files preserved");
    options.path = root / "text.log"; options.format = SerialRecordFormat::TaggedText;
    check(!session.startRecording(options, generation + 1, error), "stale start refused");
    check(session.startRecording(options, generation, error), "start text");
    session.stopRecording("stale action", firstId);
    check(session.recordingSnapshot().busy, "stale stop cannot stop new recording");
    transport->receive({0xe4}); transport->receive({0xb8, 0xad, 0, '!', '\n'});
    check(session.send({'T', 'X'}, 0), "text TX without display echo");
    check(await([&] { return session.snapshot().sentBytes == 4; }), "text TX completes");
    transport->failWrites = true;
    session.send({'N', 'O'}, 1);
    check(await([&] { return session.snapshot().pendingWrites == 0; }), "failed TX handled");
    transport->receive({0xf0});
    transport->unplug();
    check(await([&] { return !session.recordingSnapshot().busy; }), "unplug drains recording");
    const auto text = read(options.path);
    check(text.find("RX  中\\x00!") != std::string::npos && text.find("TX  TX") != std::string::npos &&
          text.find("TX  NO") == std::string::npos && text.find("\\xE4") == std::string::npos && text.find("\\xF0") != std::string::npos,
          "incremental UTF8 and only successful TX");
    check(session.recordingSnapshot().endReason == "串口断开", "unplug reason");
    options.path = root / "exit.bin"; options.format = SerialRecordFormat::RawRx;
    {
        SerialSession exitSession;
        auto value = std::make_unique<Transport>(); auto* sender = value.get();
        const auto gen = exitSession.open(std::move(value), config);
        check(exitSession.startRecording(options, gen, error), "exit recording start");
        sender->receive({0, 1, 2, 255});
    }
    check(read(options.path) == std::string("\0\1\2\xff", 4), "owned exit drains accepted bytes");
}
void testFailures(const std::filesystem::path& root)
{
    std::string error;
    for (int failure = 1; failure <= 4; ++failure) {
        auto state = std::make_shared<SinkState>(); state->failure = failure;
        SerialRecorder recorder([state] { return std::make_unique<Sink>(state); });
        SerialRecordOptions options{root / ("failure" + std::to_string(failure) + ".bin"), SerialRecordFormat::RawRx};
        check(recorder.start(options, 1, {}, error), "inject failure");
        recorder.push(1, false, {1, 2}); recorder.stop();
        check(await([&] { return !recorder.snapshot().busy; }) &&
              recorder.snapshot().state == SerialRecordState::Failed && !recorder.snapshot().error.empty(),
              "open/write/flush/close failure observable");
    }
    SerialRecordOptions options{root / "metadata.bin", SerialRecordFormat::RawRx};
    std::filesystem::create_directory(SerialRecorder::metadataPath(options.path)); options.overwrite = true;
    SerialRecorder metaFailure;
    check(metaFailure.start(options, 1, {}, error), "metadata failure start"); metaFailure.stop();
    check(await([&] { return !metaFailure.snapshot().busy; }) &&
          metaFailure.snapshot().state == SerialRecordState::Failed, "metadata failure cannot claim completion");

    {
        auto failed = std::make_shared<SinkState>(); failed->failure = 2; failed->blockClose = true;
        SerialRecorder recorder([failed] { return std::make_unique<Sink>(failed); });
        SerialRecordOptions broken{root / "failed-closing.bin", SerialRecordFormat::RawRx};
        check(recorder.start(broken, 1, {}, error), "write failure while closing");
        recorder.push(1, false, {1});
        {
            std::unique_lock<std::mutex> lock(failed->mutex);
            check(failed->changed.wait_for(lock, std::chrono::seconds(5), [&] { return failed->closing; }), "failed file close entered");
        }
        recorder.push(1, false, {2, 3});
        check(recorder.snapshot().acceptedDataBytes == 1, "failed recorder stops accepting before slow close");
        { std::lock_guard<std::mutex> lock(failed->mutex); failed->release = true; } failed->changed.notify_all();
        check(await([&] { return !recorder.snapshot().busy; }), "failed close completes");
    }

    auto state = std::make_shared<SinkState>(); state->block = true;
    SerialRecorder slow([state] { return std::make_unique<Sink>(state); });
    options = {root / "overflow.bin", SerialRecordFormat::RawRx}; options.maxQueuedBytes = 8;
    check(slow.start(options, 7, {}, error), "slow start"); slow.push(7, false, {1, 2, 3, 4});
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        check(state->changed.wait_for(lock, std::chrono::seconds(5), [&] { return state->entered; }), "slow write entered");
    }
    const auto begin = std::chrono::steady_clock::now();
    slow.push(6, false, {99}); slow.push(7, false, {5, 6, 7, 8, 9, 10, 11, 12}); slow.push(7, false, {13});
    check(std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(100), "callback never waits disk");
    check(slow.snapshot().busy && slow.snapshot().state == SerialRecordState::Stopping &&
          slow.snapshot().pendingBytes == 8 && slow.snapshot().rejectedDataBytes == 1, "overflow stops accepting visibly");
    { std::lock_guard<std::mutex> lock(state->mutex); state->release = true; } state->changed.notify_all();
    check(await([&] { return !slow.snapshot().busy; }), "overflow drains accepted bytes");
    check(slow.snapshot().state == SerialRecordState::Failed && state->data.size() == 12 &&
          state->writer != std::this_thread::get_id() &&
          read(SerialRecorder::metadataPath(options.path)).find("\"complete\":false") != std::string::npos,
          "overflow file explicitly incomplete, background writer, stale data absent");
}
}
int main()
{
    const auto temporary = std::filesystem::canonical(std::filesystem::temp_directory_path());
    const auto root = temporary /
        ("testtools-recording-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        check(root.parent_path() == temporary && std::filesystem::create_directory(root), "create isolated test directory");
        testQueue(); testFiles(root); testFailures(root);
        check(std::filesystem::canonical(root).parent_path() == temporary, "cleanup stays inside owned temporary directory");
        std::filesystem::remove_all(root);
        std::cout << "PASS serial recording, lifecycle, disk failures, bounded display delivery\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << "; evidence=" << root << '\n'; return 1;
    }
}
