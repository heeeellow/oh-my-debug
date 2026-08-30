#include "application/background_task_group.h"
#include "application/serial_workspace_state.h"
#include "core/agent_protocol.h"
#include "core/async_serial_sender.h"
#include "core/codec.h"
#include "core/command_line.h"
#include "core/serial_test_engine.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace {

int fail(const char *name)
{
    std::cerr << "FAIL " << name << '\n';
    return 1;
}

} // namespace

int main()
{
    std::uint16_t port = 0;
    if (!testtools::parseTcpPort("27183", port) || port != 27183
        || testtools::parseTcpPort("invalid", port)
        || testtools::parseTcpPort("70000", port)
        || testtools::parseTcpPort("0", port)) {
        return fail("TCP port parsing");
    }

    serialdemo::ByteBuffer bytes;
    std::string error;
    if (!serialdemo::parseHex("55 AA 01 0d 0A", bytes, error)
        || serialdemo::toHex(bytes) != "55 AA 01 0D 0A") {
        return fail("HEX codec");
    }
    serialdemo::ByteBuffer lineEndingProbe{0x41, 0x54};
    serialdemo::appendLineEnding(lineEndingProbe, serialdemo::LineEnding::CrLf);
    if (serialdemo::toHex(lineEndingProbe) != "41 54 0D 0A") {
        return fail("line ending codec");
    }

    serialdemo::PeriodicSendPlan sendPlan;
    const auto start = serialdemo::PeriodicSendPlan::Clock::now();
    if (!sendPlan.start({std::chrono::milliseconds(10), 2}, start)
        || !sendPlan.isDue(start)) {
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
    sender.start([](const serialdemo::ByteBuffer &payload) {
        return payload == serialdemo::ByteBuffer{0x41, 0x54};
    }, [&](const serialdemo::ByteBuffer &, std::uint64_t, bool succeeded) {
        std::lock_guard<std::mutex> lock(senderMutex);
        senderCompleted = succeeded;
        senderCondition.notify_one();
    });
    if (!sender.enqueue({0x41, 0x54}, 7)) {
        return fail("async sender enqueue");
    }
    {
        std::unique_lock<std::mutex> lock(senderMutex);
        senderCondition.wait_for(lock, std::chrono::seconds(2), [&] {
            return senderCompleted;
        });
    }
    sender.stop(true);
    if (!senderCompleted || sender.pendingCount() != 0) {
        return fail("async sender completion");
    }

    const auto encoded = testtools::encodeAgentFrame(
        testtools::jsonAgentFrame("{\"protocol\":1}"));
    testtools::AgentFrameDecoder decoder;
    std::vector<testtools::AgentFrame> decoded;
    std::string decodeError;
    if (!decoder.push(serialdemo::ByteBuffer(encoded.begin(), encoded.end()),
                      decoded, decodeError)
        || decoded.size() != 1) {
        return fail("Agent frame codec");
    }

    testtools::SerialWorkspaceState workspace;
    testtools::SerialWorkspaceActivity activity;
    if (!workspace.applyMode(testtools::SerialWorkspaceMode::Dual, activity)) {
        return fail("workspace dual mode");
    }
    activity.channelActive[0] = true;
    const auto blocked = workspace.evaluateModeSwitch(
        testtools::SerialWorkspaceMode::Single, activity);
    if (blocked.allowed
        || blocked.blockedBy != testtools::SerialModeSwitchBlock::HiddenChannelActive) {
        return fail("workspace hidden channel guard");
    }

    testtools::SerialEndpointSelection localA{
        true, testtools::SerialEndpointKind::Local, "COM3", {}};
    auto localB = localA;
    testtools::SerialEndpointSelection agentA{
        true, testtools::SerialEndpointKind::Agent, "/dev/ttyUSB2", "device-a"};
    testtools::SerialEndpointSelection agentB{
        true, testtools::SerialEndpointKind::Agent, "/dev/ttyS2", "device-a"};
    if (testtools::SerialWorkspaceState::endpointConflict(localA, localB)
            != testtools::SerialEndpointConflict::SameResource
        || testtools::SerialWorkspaceState::endpointConflict(agentA, agentB)
            != testtools::SerialEndpointConflict::AgentSingleSession
        || testtools::SerialWorkspaceState::endpointConflict(localA, agentA)
            != testtools::SerialEndpointConflict::None) {
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
