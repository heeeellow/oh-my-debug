#include "application/agent/agent_bundle.h"
#include "application/agent/agent_target.h"
#include "application/serial/serial_session.h"
#include "application/serial/serial_workspace_state.h"
#include "application/support/background_task_group.h"
#include "core/protocol/agent_protocol.h"
#include "core/protocol/agent_version.h"
#include "core/serial/async_serial_sender.h"
#include "core/serial/codec.h"
#include "core/serial/serial_test_engine.h"
#include "core/support/command_line.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

class FakeSerialTransport final : public serialdemo::ISerialTransport {
  public:
    void open(const serialdemo::SerialConfig&) override
    {
        open_.store(true);
        publishStatus(serialdemo::TransportState::Connected, "fake connected");
    }

    void close() override
    {
        const int delay = closeDelayMs.load();
        if (delay > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }
        const bool wasOpen = open_.exchange(false);
        if (wasOpen) {
            publishStatus(serialdemo::TransportState::Closed, "fake closed");
        }
    }

    bool write(const serialdemo::ByteBuffer& data) override
    {
        if (!open_.load() || failWrites.load()) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        writes.push_back(data);
        return true;
    }

    bool isOpen() const override
    {
        return open_.load();
    }

    void emitData(const serialdemo::ByteBuffer& data)
    {
        publishData(data);
    }

    void emitDisconnect()
    {
        open_.store(false);
        publishStatus(serialdemo::TransportState::Error, "fake disconnected");
    }

    std::atomic<bool> failWrites{false};
    std::atomic<int> closeDelayMs{0};
    std::mutex mutex_;
    std::vector<serialdemo::ByteBuffer> writes;

  private:
    std::atomic<bool> open_{false};
};

int fail(const char* name)
{
    std::cerr << "FAIL " << name << '\n';
    return 1;
}

} // namespace

int main()
{
    const auto androidArm32 = testtools::parseAndroidAgentAbi("armeabi-v7a");
    const auto androidArm64 = testtools::parseAndroidAgentAbi("arm64-v8a");
    const auto linuxArm32 = testtools::parseLinuxAgentMachine("armv7l");
    const auto linuxArm64 = testtools::parseLinuxAgentMachine("aarch64");
    if (!androidArm32 || *androidArm32 != testtools::AgentArchitecture::ArmV7 || !androidArm64 ||
        *androidArm64 != testtools::AgentArchitecture::Arm64 || !linuxArm32 ||
        *linuxArm32 != testtools::AgentArchitecture::ArmV7 || !linuxArm64 ||
        *linuxArm64 != testtools::AgentArchitecture::Arm64 ||
        testtools::parseAndroidAgentAbi("x86_64") || testtools::parseLinuxAgentMachine("x86_64")) {
        return fail("ARM-only Agent target mapping");
    }
    const auto bundleRoot =
        std::filesystem::temp_directory_path() /
        ("testtools-agent-bundle-contract-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code bundleEc;
    std::filesystem::remove_all(bundleRoot, bundleEc);
    std::filesystem::create_directories(bundleRoot / "agents/android/armeabi-v7a");
    std::filesystem::create_directories(bundleRoot / "agents/android/arm64-v8a");
    std::filesystem::create_directories(bundleRoot / "agents/linux/aarch64/glibc-2.31");
    for (const auto& path : {"agents/android/armeabi-v7a/testtools-agent",
                             "agents/android/arm64-v8a/testtools-agent",
                             "agents/linux/aarch64/glibc-2.31/testtools-agent"}) {
        std::ofstream(bundleRoot / path, std::ios::binary) << "hello";
    }
    std::ofstream(bundleRoot / "agents/android/arm64-v8a/runner.bin", std::ios::binary)
        << std::string(1000, 'a');
    const char* digest = "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824";
    const char* runnerDigest = "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3";
    std::ofstream(bundleRoot / "manifest.json")
        << R"({"schema_version":2,"product":"TestTools","agent_version":")"
        << testtools::AgentVersion
        << R"(","protocol":{"minimum":1,"maximum":1},"build":{"build_id":"m1-test"},"packages":[)"
        << R"({"id":"android/armeabi-v7a","platform":"android","architecture":"armv7","root":"agents/android/armeabi-v7a","android":{"abi":"armeabi-v7a","minimum_api":23},"files":[{"path":"testtools-agent","role":"agent","executable":true,"size":5,"sha256":")"
        << digest << R"("}]},)"
        << R"({"id":"android/arm64-v8a","platform":"android","architecture":"aarch64","root":"agents/android/arm64-v8a","android":{"abi":"arm64-v8a","minimum_api":23},"files":[{"path":"testtools-agent","role":"agent","executable":true,"size":5,"sha256":")"
        << digest
        << R"("},{"path":"runner.bin","role":"runner","executable":false,"size":1000,"sha256":")"
        << runnerDigest << R"("}]},)"
        << R"({"id":"linux/aarch64/glibc-2.31","platform":"linux","architecture":"aarch64","root":"agents/linux/aarch64/glibc-2.31","linux":{"libc":"glibc","minimum_version":"2.31"},"files":[{"path":"testtools-agent","role":"agent","executable":true,"size":5,"sha256":")"
        << digest << R"("}]}]})";
    std::string bundleError;
    const auto bundle = testtools::loadAgentBundle(bundleRoot, bundleError);
    if (!bundle || !testtools::selectAndroidAgentPackage(*bundle, "arm64-v8a") ||
        !testtools::selectLinuxAgentPackage(*bundle, "aarch64", "2.35")) {
        return fail("Agent Bundle v2 target selection");
    }
    const std::string validManifest = [&] {
        std::ifstream input(bundleRoot / "manifest.json", std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    }();
    const auto rejectsManifest = [&](const std::string& text, const char* name) {
        std::ofstream output(bundleRoot / "manifest.json", std::ios::trunc);
        output << text;
        output.close();
        if (testtools::loadAgentBundle(bundleRoot, bundleError))
            return fail(name);
        return 0;
    };
    std::string mutated = validManifest;
    mutated.replace(mutated.find("\"size\":5"), 9, "\"size\":6");
    if (rejectsManifest(mutated, "Agent Bundle size rejection") != 0)
        return 1;
    mutated = validManifest;
    const auto hashStart = mutated.find("\"sha256\":\"") + 10;
    mutated.replace(
        hashStart, 64, "0000000000000000000000000000000000000000000000000000000000000000");
    if (rejectsManifest(mutated, "Agent Bundle hash rejection") != 0)
        return 1;
    mutated = validManifest;
    const std::string oldPath = "\"path\":\"testtools-agent\"";
    const auto pathStart = mutated.find(oldPath);
    mutated.replace(pathStart, oldPath.size(), "\"path\":\"../testtools-agent\"");
    if (rejectsManifest(mutated, "Agent Bundle traversal rejection") != 0)
        return 1;
    mutated = validManifest;
    const auto firstFiles = mutated.find("\"files\":[") + 9;
    const auto firstFileEnd = mutated.find("}]", firstFiles);
    std::string duplicateFile = mutated.substr(firstFiles, firstFileEnd - firstFiles + 1);
    const auto duplicateRole = duplicateFile.find("\"role\":\"agent\"");
    duplicateFile.replace(duplicateRole, 14, "\"role\":\"config\"");
    mutated.insert(firstFileEnd + 1, "," + duplicateFile);
    if (rejectsManifest(mutated, "Agent Bundle duplicate install path rejection") != 0)
        return 1;
    mutated = validManifest;
    mutated.replace(mutated.find("\"role\":\"agent\""), 14, "\"role\":\"runner\"");
    if (rejectsManifest(mutated, "Agent Bundle missing agent rejection") != 0)
        return 1;
    mutated = validManifest;
    mutated.replace(mutated.find("\"executable\":true"), 17, "\"executable\":false");
    if (rejectsManifest(mutated, "Agent Bundle executable agent rejection") != 0)
        return 1;
    {
        std::ofstream corrupt(bundleRoot / "manifest.json", std::ios::trunc);
        corrupt << "{\"schema_version\":2,\"product\":\"TestTools\",\"agent_version\":\""
                << testtools::AgentVersion
                << "\",\"protocol\":{\"minimum\":1,\"maximum\":1},\"build\":{\"build_id\":\"m1-"
                   "test\"},\"packages\":[]}";
    }
    if (testtools::loadAgentBundle(bundleRoot, bundleError))
        return fail("Agent Bundle missing package rejection");
    std::filesystem::remove_all(bundleRoot, bundleEc);

    std::uint16_t port = 0;
    if (!testtools::parseTcpPort("27183", port) || port != 27183 ||
        testtools::parseTcpPort("invalid", port) || testtools::parseTcpPort("70000", port) ||
        testtools::parseTcpPort("0", port)) {
        return fail("TCP port parsing");
    }

    serialdemo::ByteBuffer bytes;
    std::string error;
    if (!serialdemo::parseHex("55 AA 01 0d 0A", bytes, error) ||
        serialdemo::toHex(bytes) != "55 AA 01 0D 0A") {
        return fail("HEX codec");
    }
    serialdemo::ByteBuffer lineEndingProbe{0x41, 0x54};
    serialdemo::appendLineEnding(lineEndingProbe, serialdemo::LineEnding::CrLf);
    if (serialdemo::toHex(lineEndingProbe) != "41 54 0D 0A") {
        return fail("line ending codec");
    }

    serialdemo::PeriodicSendPlan sendPlan;
    const auto start = serialdemo::PeriodicSendPlan::Clock::now();
    if (!sendPlan.start({std::chrono::milliseconds(10), 2}, start) || !sendPlan.isDue(start)) {
        return fail("periodic send start");
    }
    sendPlan.recordAttempt(true, start);
    sendPlan.recordAttempt(false, start + std::chrono::milliseconds(10));
    if (sendPlan.isRunning() || sendPlan.attempts() != 2 || sendPlan.successes() != 1) {
        return fail("periodic send accounting");
    }

    const auto frame0 = serialdemo::encodeIntegrityFrame(0, 32);
    const auto frame1 = serialdemo::encodeIntegrityFrame(1, 32);
    serialdemo::IntegrityStreamVerifier verifier;
    verifier.push(frame0);
    verifier.push(frame1);
    if (verifier.stats().uniqueFrames != 2 || verifier.stats().bitErrors != 0) {
        return fail("integrity stream verifier");
    }

    std::mutex senderMutex;
    std::condition_variable senderCondition;
    bool senderCompleted = false;
    serialdemo::AsyncSerialSender sender;
    sender.start(
        [](const serialdemo::ByteBuffer& payload) {
            return payload == serialdemo::ByteBuffer{0x41, 0x54};
        },
        [&](const serialdemo::ByteBuffer&, std::uint64_t, bool succeeded) {
            std::lock_guard<std::mutex> lock(senderMutex);
            senderCompleted = succeeded;
            senderCondition.notify_one();
        });
    if (!sender.enqueue({0x41, 0x54}, 7)) {
        return fail("async sender enqueue");
    }
    {
        std::unique_lock<std::mutex> lock(senderMutex);
        senderCondition.wait_for(lock, std::chrono::seconds(2), [&] { return senderCompleted; });
    }
    sender.stop(true);
    if (!senderCompleted || sender.pendingCount() != 0) {
        return fail("async sender completion");
    }

    std::mutex sessionMutex;
    std::condition_variable sessionCondition;
    int sessionSendCompletions = 0;
    std::uint64_t callbackGeneration = 0;
    bool lastSendSucceeded = false;
    bool scheduleStoppedByFailure = false;
    testtools::SerialSession session;
    session.setSendCallback([&](std::uint64_t generation,
                                const serialdemo::ByteBuffer&,
                                std::uint64_t,
                                bool succeeded,
                                bool scheduledSendStopped) {
        std::lock_guard<std::mutex> lock(sessionMutex);
        callbackGeneration = generation;
        lastSendSucceeded = succeeded;
        scheduleStoppedByFailure = scheduledSendStopped;
        ++sessionSendCompletions;
        sessionCondition.notify_all();
    });
    auto fakeTransport = std::make_unique<FakeSerialTransport>();
    auto* fakeTransportView = fakeTransport.get();
    const auto sessionGeneration = session.open(std::move(fakeTransport), {});
    if (!session.isActive() || !session.isOpen() ||
        session.snapshot().state != testtools::SerialSessionState::Connected) {
        return fail("serial session open state");
    }
    fakeTransportView->emitData({0x4F, 0x4B});
    if (!session.send({0x41, 0x54}, 1)) {
        return fail("serial session enqueue");
    }
    {
        std::unique_lock<std::mutex> lock(sessionMutex);
        sessionCondition.wait_for(
            lock, std::chrono::seconds(2), [&] { return sessionSendCompletions == 1; });
    }
    auto sessionSnapshot = session.snapshot();
    if (sessionSendCompletions != 1 || callbackGeneration != sessionGeneration ||
        !lastSendSucceeded || sessionSnapshot.receivedBytes != 2 ||
        sessionSnapshot.sentBytes != 2) {
        return fail("serial session generation and counters");
    }

    const auto scheduleStart = serialdemo::PeriodicSendPlan::Clock::now();
    if (!session.startScheduledSend({0x41}, {std::chrono::milliseconds(10), 2}, 1, scheduleStart)) {
        return fail("serial session scheduled send start");
    }
    if (session.startScheduledSend({0x42}, {std::chrono::milliseconds(10), 1}, 0, scheduleStart)) {
        return fail("serial session duplicate schedule guard");
    }
    if (session.processScheduledSend(scheduleStart) != testtools::ScheduledSendTick::Queued ||
        session.processScheduledSend(scheduleStart + std::chrono::milliseconds(10)) !=
            testtools::ScheduledSendTick::Completed) {
        return fail("serial session scheduled send lifecycle");
    }
    {
        std::unique_lock<std::mutex> lock(sessionMutex);
        sessionCondition.wait_for(
            lock, std::chrono::seconds(2), [&] { return sessionSendCompletions == 3; });
    }
    sessionSnapshot = session.snapshot();
    if (sessionSendCompletions != 3 || sessionSnapshot.scheduledSend.running ||
        sessionSnapshot.scheduledSend.attempts != 2 ||
        sessionSnapshot.scheduledSend.submitted != 2) {
        return fail("serial session scheduled send accounting");
    }

    fakeTransportView->failWrites.store(true);
    scheduleStoppedByFailure = false;
    if (!session.startScheduledSend({0x42}, {std::chrono::milliseconds(10), 0}, 0, scheduleStart) ||
        session.processScheduledSend(scheduleStart) != testtools::ScheduledSendTick::Queued) {
        return fail("serial session failed schedule enqueue");
    }
    {
        std::unique_lock<std::mutex> lock(sessionMutex);
        sessionCondition.wait_for(
            lock, std::chrono::seconds(2), [&] { return sessionSendCompletions == 4; });
    }
    if (sessionSendCompletions != 4 || lastSendSucceeded || !scheduleStoppedByFailure ||
        session.snapshot().scheduledSend.running) {
        return fail("serial session write failure convergence");
    }
    fakeTransportView->failWrites.store(false);

    const auto previousGeneration = session.generation();
    if (!session.startScheduledSend({0x41}, {std::chrono::milliseconds(10), 0}, 0) ||
        !session.snapshot().scheduledSend.running) {
        return fail("serial session continuous schedule start");
    }
    fakeTransportView->emitDisconnect();
    sessionSnapshot = session.snapshot();
    if (sessionSnapshot.state != testtools::SerialSessionState::Disconnected ||
        sessionSnapshot.open || sessionSnapshot.scheduledSend.running) {
        return fail("serial session unexpected disconnect state");
    }
    std::uint64_t closeCallbackGeneration = 0;
    bool closeCompleted = false;
    fakeTransportView->closeDelayMs.store(50);
    const auto closeGeneration = session.closeAsync([&](std::uint64_t generation) {
        std::lock_guard<std::mutex> lock(sessionMutex);
        closeCallbackGeneration = generation;
        closeCompleted = true;
        sessionCondition.notify_all();
    });
    sessionSnapshot = session.snapshot();
    if (closeGeneration == previousGeneration || !sessionSnapshot.active ||
        sessionSnapshot.state != testtools::SerialSessionState::Disconnecting) {
        return fail("serial session asynchronous disconnect start");
    }
    {
        std::unique_lock<std::mutex> lock(sessionMutex);
        sessionCondition.wait_for(lock, std::chrono::seconds(2), [&] { return closeCompleted; });
    }
    if (!closeCompleted || closeCallbackGeneration != closeGeneration || session.isActive() ||
        session.isOpen() || session.snapshot().scheduledSend.running ||
        session.snapshot().state != testtools::SerialSessionState::Disconnected) {
        return fail("serial session asynchronous disconnect convergence");
    }
    auto reopenedTransport = std::make_unique<FakeSerialTransport>();
    const auto reopenedGeneration = session.open(std::move(reopenedTransport), {});
    if (reopenedGeneration <= closeGeneration || !session.isOpen()) {
        return fail("serial session reopen after asynchronous disconnect");
    }
    session.close();

    const auto encoded = testtools::encodeAgentFrame(testtools::jsonAgentFrame("{\"protocol\":1}"));
    testtools::AgentFrameDecoder decoder;
    std::vector<testtools::AgentFrame> decoded;
    std::string decodeError;
    if (!decoder.push(
            serialdemo::ByteBuffer(encoded.begin(), encoded.end()), decoded, decodeError) ||
        decoded.size() != 1) {
        return fail("Agent frame codec");
    }

    testtools::SerialWorkspaceState workspace;
    testtools::SerialWorkspaceActivity activity;
    if (!workspace.applyMode(testtools::SerialWorkspaceMode::Dual, activity)) {
        return fail("workspace dual mode");
    }
    activity.channelActive[0] = true;
    const auto blocked =
        workspace.evaluateModeSwitch(testtools::SerialWorkspaceMode::Single, activity);
    if (blocked.allowed ||
        blocked.blockedBy != testtools::SerialModeSwitchBlock::HiddenChannelActive) {
        return fail("workspace hidden channel guard");
    }

    testtools::SerialEndpointSelection localA{
        true, testtools::SerialEndpointKind::Local, "COM3", {}};
    auto localB = localA;
    testtools::SerialEndpointSelection agentA{
        true, testtools::SerialEndpointKind::Agent, "/dev/ttyUSB2", "device-a"};
    testtools::SerialEndpointSelection agentB{
        true, testtools::SerialEndpointKind::Agent, "/dev/ttyS2", "device-a"};
    if (testtools::SerialWorkspaceState::endpointConflict(localA, localB) !=
            testtools::SerialEndpointConflict::SameResource ||
        testtools::SerialWorkspaceState::endpointConflict(agentA, agentB) !=
            testtools::SerialEndpointConflict::AgentSingleSession ||
        testtools::SerialWorkspaceState::endpointConflict(localA, agentA) !=
            testtools::SerialEndpointConflict::None) {
        return fail("workspace endpoint conflicts");
    }

    std::atomic<int> completedTasks{0};
    testtools::BackgroundTaskGroup tasks;
    tasks.start([&completedTasks] { ++completedTasks; });
    tasks.start([&completedTasks] { ++completedTasks; });
    tasks.wait();
    if (completedTasks != 2 || tasks.retainedTaskCount() != 0) {
        return fail("background task ownership");
    }

    std::cout << "PASS core and application contracts without Qt or devices\n";
    return 0;
}
