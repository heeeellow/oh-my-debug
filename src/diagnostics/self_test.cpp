  #include "self_test.h"

#include "adapters/adb/adb_transport.h"
#include "adapters/adb/process_runner.h"
#include "adapters/agent/agent_client.h"
#include "adapters/agent/agent_serial_transport.h"
#include "core/protocol/agent_protocol.h"
#include "core/protocol/agent_version.h"
#include "core/serial/async_serial_sender.h"
#include "core/serial/codec.h"
#include "core/serial/serial_test_engine.h"
#include "core/support/command_line.h"
#include "application/agent/adb_agent_deployment.h"
#include "application/serial/serial_workspace_state.h"
#include "application/support/background_task_group.h"

#include <atomic>
#include <chrono>
#include <algorithm>
#include <iostream>
#include <condition_variable>
#include <mutex>

namespace serialdemo {

namespace {

std::string trim(std::string value)
{
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ')) {
        value.pop_back();
    }
    return value;
}

} // namespace

int runSelfTest()
{
    ByteBuffer bytes;
    std::string error;
    std::uint16_t parsedPort = 0;
    if (!testtools::parseTcpPort("27183", parsedPort) || parsedPort != 27183
        || testtools::parseTcpPort("not-a-port", parsedPort)
        || testtools::parseTcpPort("70000", parsedPort)
        || testtools::parseTcpPort("0", parsedPort)) {
        std::cerr << "FAIL command line port validation\n";
        return 9;
    }
    if (!parseHex("55 AA 01 0d 0A", bytes, error) || toHex(bytes) != "55 AA 01 0D 0A") {
        std::cerr << "FAIL codec: " << error << '\n';
        return 10;
    }
    ByteBuffer lineEndingProbe{0x41, 0x54};
    appendLineEnding(lineEndingProbe, LineEnding::CrLf);
    if (toHex(lineEndingProbe) != "41 54 0D 0A") {
        std::cerr << "FAIL line ending codec\n";
        return 11;
    }

    const auto frame0 = encodeIntegrityFrame(0, 32);
    const auto frame1 = encodeIntegrityFrame(1, 32);
    auto damagedFrame = encodeIntegrityFrame(2, 32);
    damagedFrame[16] ^= 0x05;
    IntegrityStreamVerifier verifier;
    verifier.push(ByteBuffer{0x00, 0x01, 'T'});
    verifier.push(ByteBuffer(frame0.begin(), frame0.begin() + 7));
    ByteBuffer remainder(frame0.begin() + 7, frame0.end());
    remainder.insert(remainder.end(), frame1.begin(), frame1.end());
    remainder.insert(remainder.end(), damagedFrame.begin(), damagedFrame.end());
    verifier.push(remainder);
    verifier.push(frame1);
    const auto &integrity = verifier.stats();
    if (integrity.frames != 4 || integrity.uniqueFrames != 3 || integrity.validFrames != 3
        || integrity.corruptedFrames != 1 || integrity.duplicateFrames != 1
        || integrity.bitErrors != 2 || integrity.framingErrors == 0) {
        std::cerr << "FAIL integrity stream verifier\n";
        return 12;
    }

    PeriodicSendPlan sendPlan;
    const auto start = PeriodicSendPlan::Clock::now();
    if (!sendPlan.start({std::chrono::milliseconds(10), 2}, start)
        || !sendPlan.isDue(start)) {
        std::cerr << "FAIL periodic send start\n";
        return 13;
    }
    sendPlan.recordAttempt(true, start);
    sendPlan.recordAttempt(false, start + std::chrono::milliseconds(10));
    if (sendPlan.isRunning() || sendPlan.attempts() != 2 || sendPlan.successes() != 1) {
        std::cerr << "FAIL periodic send accounting\n";
        return 14;
    }

    std::mutex senderMutex;
    std::condition_variable senderCondition;
    int senderCompletions = 0;
    std::uint64_t senderTag = 0;
    AsyncSerialSender sender;
    sender.start([](const ByteBuffer &payload) { return payload == ByteBuffer{0x41, 0x54}; },
                 [&](const ByteBuffer &, std::uint64_t tag, bool succeeded) {
                     std::lock_guard<std::mutex> lock(senderMutex);
                     if (succeeded) {
                         ++senderCompletions;
                         senderTag = tag;
                     }
                     senderCondition.notify_one();
                 });
    if (!sender.enqueue(ByteBuffer{0x41, 0x54}, 7)) {
        std::cerr << "FAIL async sender enqueue\n";
        return 15;
    }
    {
        std::unique_lock<std::mutex> lock(senderMutex);
        senderCondition.wait_for(lock, std::chrono::seconds(2), [&] { return senderCompletions == 1; });
    }
    sender.stop(true);
    if (senderCompletions != 1 || senderTag != 7 || sender.pendingCount() != 0) {
        std::cerr << "FAIL async sender completion\n";
        return 16;
    }

    const auto encodedAgentFrame = testtools::encodeAgentFrame(
        testtools::jsonAgentFrame("{\"protocol\":1,\"request_id\":\"unit\"}"));
    testtools::AgentFrameDecoder agentDecoder;
    std::vector<testtools::AgentFrame> agentFrames;
    std::string agentFrameError;
    agentDecoder.push(ByteBuffer(encodedAgentFrame.begin(), encodedAgentFrame.begin() + 3),
                      agentFrames, agentFrameError);
    if (!agentFrames.empty()
        || !agentDecoder.push(ByteBuffer(encodedAgentFrame.begin() + 3, encodedAgentFrame.end()),
                              agentFrames, agentFrameError)
        || agentFrames.size() != 1
        || testtools::agentFrameText(agentFrames.front()).find("request_id") == std::string::npos) {
        std::cerr << "FAIL agent frame split/coalesce codec: " << agentFrameError << '\n';
        return 17;
    }

    testtools::SerialWorkspaceState workspaceState;
    testtools::SerialWorkspaceActivity workspaceActivity;
    if (!workspaceState.applyMode(testtools::SerialWorkspaceMode::Dual, workspaceActivity)) {
        std::cerr << "FAIL serial workspace mode transition\n";
        return 18;
    }
    workspaceActivity.channelActive[0] = true;
    const auto blockedMode = workspaceState.evaluateModeSwitch(
        testtools::SerialWorkspaceMode::Single, workspaceActivity);
    if (blockedMode.allowed
        || blockedMode.blockedBy != testtools::SerialModeSwitchBlock::HiddenChannelActive) {
        std::cerr << "FAIL serial workspace hidden connection guard\n";
        return 19;
    }
    testtools::SerialEndpointSelection localA{
        true, testtools::SerialEndpointKind::Local, "COM3", {}};
    testtools::SerialEndpointSelection localB = localA;
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
        std::cerr << "FAIL serial workspace endpoint conflict rules\n";
        return 23;
    }

    std::atomic<int> completedTasks{0};
    testtools::BackgroundTaskGroup taskGroup;
    taskGroup.start([&completedTasks] { ++completedTasks; });
    taskGroup.start([&completedTasks] { ++completedTasks; });
    taskGroup.wait();
    if (completedTasks != 2 || taskGroup.retainedTaskCount() != 0) {
        std::cerr << "FAIL background task ownership\n";
        return 24;
    }

    AdbTransport adb;
    auto devices = adb.availableDevices(error);
    if (!error.empty() || devices.empty()) {
        std::cerr << "FAIL adb devices: " << error << '\n';
        return 20;
    }
    const auto online = std::find_if(devices.begin(), devices.end(), [](const AdbDevice &device) {
        return device.state == "device";
    });
    if (online == devices.end()) {
        std::cerr << "FAIL adb device state=" << devices.front().state << '\n';
        return 22;
    }
    const auto serial = online->serial;
    const auto ports = adb.availableRemotePorts(serial, error);
    if (!error.empty()) {
        std::cerr << "FAIL adb ports: " << error << '\n';
        return 21;
    }

    const auto forward = ProcessRunner::run("adb",
                                             {"-s", serial, "forward", "tcp:0", "dev:/dev/null"},
                                             std::chrono::seconds(5));
    if (forward.timedOut || forward.exitCode != 0) {
        std::cerr << "FAIL adb dev forward: " << forward.standardError << '\n';
        return 30;
    }

    int localPort = 0;
    try {
        localPort = std::stoi(trim(forward.standardOutput));
    } catch (...) {
        std::cerr << "FAIL invalid adb forward port\n";
        return 31;
    }

    const auto cleanup = ProcessRunner::run("adb",
                                             {"-s", serial, "forward", "--remove", "tcp:" + std::to_string(localPort)},
                                             std::chrono::seconds(5));
    if (cleanup.timedOut || cleanup.exitCode != 0) {
        std::cerr << "FAIL cleanup adb forward\n";
        return 32;
    }

    std::cout << "PASS codec+line-ending+serial-stress; adb_device=" << serial << "; remote_ports=" << ports.size()
              << "; safe_dev_forward=tcp:" << localPort << '\n';
    return 0;
}

int runAgentSmokeTest(const std::string &host, std::uint16_t port)
{
    testtools::AgentClient client;
    std::string error;
    if (!client.connect(host, port, error)) {
        std::cerr << "FAIL " << error << '\n';
        return 40;
    }
    std::string hello;
    if (!client.request("{\"protocol\":1,\"request_id\":\"smoke-hello\","
                        "\"method\":\"session.hello\",\"params\":{}}",
                        hello, error)
        || hello.find("\"protocol_min\":1") == std::string::npos
        || hello.find("\"ok\":true") == std::string::npos) {
        std::cerr << "FAIL agent hello: " << (error.empty() ? hello : error) << '\n';
        return 41;
    }
    std::string capabilities;
    if (!client.request("{\"protocol\":1,\"request_id\":\"smoke-capabilities\","
                        "\"method\":\"capability.list\",\"params\":{}}",
                        capabilities, error)
        || capabilities.find("\"id\":\"system.info\"") == std::string::npos) {
        std::cerr << "FAIL agent capabilities: " << (error.empty() ? capabilities : error) << '\n';
        return 42;
    }
    std::cout << "PASS agent-v1 hello+capability.list host=" << host << " port=" << port << '\n';
    return 0;
}

int runAdbSerialSmokeTest(const std::string &serial, const std::string &endpoint)
{
    AdbTransport transport;
    SerialConfig config;
    config.adbSerial = serial;
    config.endpoint = endpoint;
    config.allowAdbRoot = true;
    config.baudRate = 115200;

    std::mutex statusMutex;
    std::condition_variable statusCondition;
    std::vector<std::string> messages;
    bool completed = false;
    bool connected = false;
    transport.setStatusCallback([&](TransportState state, const std::string &message) {
        std::lock_guard<std::mutex> lock(statusMutex);
        messages.push_back(message);
        if (state == TransportState::Connected || state == TransportState::Error) {
            completed = true;
            connected = state == TransportState::Connected;
            statusCondition.notify_one();
        }
    });
    transport.open(config);
    {
        std::unique_lock<std::mutex> lock(statusMutex);
        statusCondition.wait_for(lock, std::chrono::seconds(15), [&] { return completed; });
    }
    transport.setStatusCallback({});
    transport.close();
    for (const auto &message : messages) {
        std::cout << message << '\n';
    }
    if (!completed) {
        std::cerr << "FAIL ADB serial open timed out\n";
        return 50;
    }
    if (!connected) {
        std::cerr << "FAIL ADB serial open rejected\n";
        return 51;
    }
    std::cout << "PASS adb-serial-open serial=" << serial << " endpoint=" << endpoint << '\n';
    return 0;
}

int runAgentSerialSmokeTest(const std::string &serial, std::uint16_t agentControlPort,
                            const std::string &endpoint)
{
    AgentSerialTransport transport(serial, agentControlPort);
    SerialConfig config;
    config.adbSerial = serial;
    config.endpoint = endpoint;
    config.baudRate = 115200;

    std::mutex statusMutex;
    std::condition_variable statusCondition;
    std::vector<std::string> messages;
    bool completed = false;
    bool connected = false;
    bool failedAfterConnect = false;
    transport.setStatusCallback([&](TransportState state, const std::string &message) {
        std::lock_guard<std::mutex> lock(statusMutex);
        messages.push_back(message);
        if (state == TransportState::Connected || state == TransportState::Error) {
            if (connected && state == TransportState::Error) {
                failedAfterConnect = true;
            }
            connected = connected || state == TransportState::Connected;
            completed = true;
            statusCondition.notify_all();
        }
    });
    transport.open(config);
    {
        std::unique_lock<std::mutex> lock(statusMutex);
        statusCondition.wait_for(lock, std::chrono::seconds(15), [&] { return completed; });
        if (connected) {
            statusCondition.wait_for(lock, std::chrono::seconds(5), [&] {
                return failedAfterConnect;
            });
        }
    }
    const bool stayedOpen = connected && transport.isOpen() && !failedAfterConnect;
    transport.setStatusCallback({});
    transport.close();
    for (const auto &message : messages) {
        std::cout << message << '\n';
    }
    if (!completed || !connected) {
        std::cerr << "FAIL Agent serial open failed\n";
        return 60;
    }
    if (!stayedOpen) {
        std::cerr << "FAIL Agent serial stream did not remain open for 5 seconds\n";
        return 61;
    }
    std::cout << "PASS agent-serial-hold serial=" << serial
              << " endpoint=" << endpoint << " seconds=5 no_data_sent\n";
    return 0;
}

int runAgentDeploymentSmokeTest(const std::string &serial,
                                const std::string &applicationDirectory)
{
    testtools::AdbAgentDeployment deployment;
    const auto result = deployment.deployAndForward(serial, applicationDirectory,
        [](const std::string &message) { std::cout << message << '\n'; });
    if (!result.succeeded) {
        std::cerr << "FAIL Agent deployment: " << result.error << '\n';
        return 70;
    }

    testtools::AgentClient client;
    std::string error;
    std::string response;
    const auto cleanup = [&] {
        client.close();
        deployment.removeForward(serial, result.localPort);
    };
    if (!client.connect("127.0.0.1", result.localPort, error)
        || !client.request("{\"protocol\":1,\"request_id\":\"deploy-info\","
                           "\"method\":\"system.info\",\"params\":{}}",
                           response, error)
        || response.find(std::string("\"platform\":\"") + result.platform + "\"") ==
               std::string::npos
        || response.find("\"effective_uid\":0") == std::string::npos) {
        cleanup();
        std::cerr << "FAIL deployed Agent system.info: "
                  << (error.empty() ? response : error) << '\n';
        return 71;
    }
    if (!client.request("{\"protocol\":1,\"request_id\":\"deploy-serial-list\","
                        "\"method\":\"serial.list\",\"params\":{}}",
                        response, error)
        || (result.platform == "android" &&
            response.find("\"endpoint\":\"/dev/ttyUSB2\"") == std::string::npos)
        || (result.platform == "linux" &&
            response.find("\"endpoints\":[") == std::string::npos)) {
        cleanup();
        std::cerr << "FAIL deployed Agent serial.list: "
                  << (error.empty() ? response : error) << '\n';
        return 72;
    }
    cleanup();
    std::cout << "PASS adb-agent-deploy platform=" << result.platform
              << " target=" << result.target
              << " serial=" << serial << " serial_endpoints=discovered\n";
    return 0;
}

int runAgentReconnectSmokeTest(const std::string &serial)
{
    testtools::AdbAgentDeployment deployment;
    const auto result = deployment.reconnect(serial,
        [](const std::string &message) { std::cout << message << '\n'; });
    if (!result.succeeded) {
        std::cerr << "FAIL Agent reconnect: " << result.error << '\n';
        return 73;
    }
    testtools::AgentClient client;
    std::string response;
    std::string error;
    const bool connected = client.connect("127.0.0.1", result.localPort, error)
        && client.request("{\"protocol\":1,\"request_id\":\"reconnect-hello\","
                          "\"method\":\"session.hello\",\"params\":{}}",
                          response, error)
        && response.find(std::string("\"agent_version\":\"") + testtools::AgentVersion + "\"")
           != std::string::npos;
    client.close();
    deployment.removeForward(serial, result.localPort);
    if (!connected) {
        std::cerr << "FAIL existing Agent handshake: "
                  << (error.empty() ? response : error) << '\n';
        return 74;
    }
    std::cout << "PASS existing-agent-reconnect platform=" << result.platform
              << " target=" << result.target
              << " serial=" << serial << '\n';
    return 0;
}

int runAgentAtSmokeTest(const std::string &serial, const std::string &endpoint)
{
    testtools::AdbAgentDeployment deployment;
    const auto connection = deployment.reconnect(serial,
        [](const std::string &message) { std::cout << message << '\n'; });
    if (!connection.succeeded) {
        std::cerr << "FAIL Agent reconnect: " << connection.error << '\n';
        return 75;
    }

    AgentSerialTransport transport(serial, connection.localPort);
    std::mutex mutex;
    std::condition_variable condition;
    bool connected = false;
    bool failed = false;
    std::string status;
    ByteBuffer received;
    transport.setStatusCallback([&](TransportState state, const std::string &message) {
        std::lock_guard<std::mutex> lock(mutex);
        status = message;
        connected = connected || state == TransportState::Connected;
        failed = failed || state == TransportState::Error;
        condition.notify_all();
    });
    transport.setDataCallback([&](const ByteBuffer &data) {
        std::lock_guard<std::mutex> lock(mutex);
        received.insert(received.end(), data.begin(), data.end());
        condition.notify_all();
    });

    SerialConfig config;
    config.endpoint = endpoint;
    config.baudRate = 115200;
    transport.open(config);
    {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait_for(lock, std::chrono::seconds(10), [&] { return connected || failed; });
    }
    if (!connected) {
        transport.close();
        deployment.removeForward(serial, connection.localPort);
        std::cerr << "FAIL Agent AT open: " << status << '\n';
        return 76;
    }

    const ByteBuffer command{'A', 'T', '\r', '\n'};
    if (!transport.write(command)) {
        transport.close();
        deployment.removeForward(serial, connection.localPort);
        std::cerr << "FAIL Agent AT write\n";
        return 77;
    }
    bool receivedOk = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        receivedOk = condition.wait_for(lock, std::chrono::seconds(5), [&] {
            const std::string text(received.begin(), received.end());
            return text.find("OK") != std::string::npos || failed;
        }) && !failed;
    }
    transport.close();
    deployment.removeForward(serial, connection.localPort);
    if (!receivedOk) {
        std::cerr << "FAIL Agent AT response: status=" << status
                  << " bytes=" << toHex(received) << '\n';
        return 78;
    }
    std::cout << "PASS agent-at serial=" << serial << " endpoint=" << endpoint
              << " tx=41 54 0D 0A rx=" << toHex(received) << '\n';
    return 0;
}

} // namespace serialdemo
