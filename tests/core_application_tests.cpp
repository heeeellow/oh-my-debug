#include "application/agent/agent_bundle.h"
#include "application/agent/agent_service_status.h"
#include "application/device/screen_mirror_service.h"
#include "application/agent/agent_target.h"
#include "application/serial/serial_link_verifier.h"
#include "application/agent/board_test_job_client.h"
#include "application/serial/serial_session.h"
#include "application/serial/serial_port_claims.h"
#include "application/serial/serial_terminal.h"
#include "application/serial/at_command_library.h"
#include "application/serial/serial_workspace_state.h"
#include "application/serial/serial_preferences_writer.h"
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

class FakeMirrorBackend final : public testtools::IScreenMirrorBackend {
public:
    bool start(const std::string& device, std::string&) override { serial = device; ++starts; status.running = true; return true; }
    void stop() override { status.running = false; }
    void requestStop() noexcept override { cancelled = true; }
    testtools::MirrorProcessStatus poll() override { return status; }
    bool control(testtools::MirrorControl value, std::string&) override { lastControl = value; return true; }
    bool screenshot(const std::filesystem::path& value, std::string&) override { path = value; return true; }
    testtools::MirrorProcessStatus status;
    testtools::MirrorControl lastControl = testtools::MirrorControl::Back;
    std::filesystem::path path;
    std::string serial;
    int starts = 0;
    bool cancelled = false;
};

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

    bool supportsControlLine(serialdemo::SerialControlLine) const override { return controlsSupported; }
    bool setControlLine(serialdemo::SerialControlLine, bool enabled, std::string &error) override
    {
        if (failControls) { error = "fake control failure"; return false; }
        controlValues.push_back(enabled);
        return true;
    }
    bool controlsSupported = false;
    bool failControls = false;
    std::vector<bool> controlValues;

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

class FakeAgentConnection final : public testtools::IAgentConnection {
  public:
    bool connect(const std::string&, std::uint16_t, std::string&) override
    {
        return true;
    }
    void close() override {}

    bool request(const std::string& request, std::string& response, std::string& error) override
    {
        requests.push_back(request);
        if (failRequests) {
            error = "fake transport failure";
            return false;
        }
        if (responses.empty()) {
            error = "fake response queue is empty";
            return false;
        }
        response = responses.front();
        responses.erase(responses.begin());
        error.clear();
        return true;
    }

    std::vector<std::string> requests;
    std::vector<std::string> responses;
    bool failRequests = false;
};

int fail(const char* name)
{
    std::cerr << "FAIL " << name << '\n';
    return 1;
}

int testBoardTestJobClient()
{
    auto connection = std::make_shared<FakeAgentConnection>();
    connection->responses = {
        R"({"protocol":1,"request_id":"host-job-1","ok":true,"result":{"job_id":"job-7","capability_id":"memory.capacity","capability_version":1,"state":"succeeded","result":{"completion":"completed","exit_code":0,"provider":{"kind":"agent_native","id":"linux_proc_meminfo","version":1},"observations":{"schema":"memory.capacity/v1","data":{"memtotal_kib":3987456}},"provider_claim":null,"summary":"容量 \"核对\"\n完成","log":""}}})",
        R"({"protocol":1,"request_id":"host-job-2","ok":true,"result":{"job_id":"job-7","capability_id":"memory.capacity","capability_version":1,"state":"running","result":null}})",
        R"({"protocol":1,"request_id":"host-job-3","ok":false,"error":{"code":"resource_busy","message":"another job is active"}})"};
    testtools::BoardTestJobClient client(connection);
    const auto started = client.start("memory.capacity");
    if (!started.ok || started.snapshot.state != testtools::BoardTestJobState::Succeeded ||
        !started.snapshot.result || !started.snapshot.result->provider ||
        started.snapshot.result->provider->kind != "agent_native" ||
        started.snapshot.result->provider->id != "linux_proc_meminfo" ||
        started.snapshot.result->observationSchema != "memory.capacity/v1" ||
        !started.snapshot.result->observations.find("memtotal_kib") ||
        started.snapshot.result->observations.find("memtotal_kib")->int64Value() != 3987456 ||
        started.snapshot.result->providerClaim ||
        started.snapshot.result->summary != "容量 \"核对\"\n完成") {
        return fail("board job client structured success snapshot");
    }
    if (connection->requests.size() != 1 ||
        connection->requests.front().find("\"method\":\"job.start\"") == std::string::npos ||
        connection->requests.front().find("\"capability_id\":\"memory.capacity\"") ==
            std::string::npos) {
        return fail("board job client start request");
    }
    const auto running = client.status("job-7");
    if (!running.ok || running.snapshot.state != testtools::BoardTestJobState::Running ||
        running.snapshot.result) {
        return fail("board job client running snapshot");
    }
    const auto rejected = client.cancel("job-7");
    if (rejected.ok || rejected.error.code != "resource_busy" ||
        rejected.error.message != "another job is active") {
        return fail("board job client structured Agent error");
    }

    if (client.start("/bin/sh").error.code != "invalid_params" ||
        client.status("../1").error.code != "invalid_params" || connection->requests.size() != 3) {
        return fail("board job client input validation");
    }
    connection->responses.push_back(
        R"({"protocol":1,"request_id":"host-job-4","ok":true,"result":{"job_id":"job-10","capability_id":"x.y","capability_version":1,"state":"failed","result":{"completion":"error","exit_code":1,"provider":null,"observations":null,"provider_claim":null,"summary":"failed","log":"failed"}}})");
    const auto valid = client.start("x.y");
    if (!valid.ok || valid.snapshot.jobId != "job-10") {
        return fail("board job client subsequent request");
    }
    connection->responses.push_back(
        R"({"protocol":1,"request_id":"stale-reply","ok":true,"result":{"job_id":"job-10","capability_id":"x.y","capability_version":1,"state":"running"}})");
    const auto mismatched = client.status("job-10");
    if (mismatched.ok || mismatched.error.code != "invalid_response") {
        return fail("board job client response correlation");
    }
    connection->failRequests = true;
    const auto transportFailure = client.status("job-10");
    if (transportFailure.ok || transportFailure.error.code != "transport_error") {
        return fail("board job client transport error");
    }
    connection->failRequests = false;
    connection->responses.push_back(
        R"({"protocol":1,"request_id":"host-job-7","ok":true,"result":{"job_id":"job-10","capability_id":"x.y","capability_version":1,"state":"running","result":{"completion":"completed","exit_code":-1,"provider":null,"observations":null,"provider_claim":null,"summary":"","log":""}}}})");
    if (client.status("job-10").error.code != "invalid_response") {
        return fail("board job client active result rejection");
    }
    connection->responses.push_back(
        R"({"protocol":1,"request_id":"host-job-8","ok":true,"result":{"job_id":"job-10","capability_id":"x.y","capability_version":1,"state":"succeeded","result":{"completion":"error","exit_code":0,"provider":null,"observations":null,"provider_claim":null,"summary":"ok","log":"ok"}}}})");
    if (client.status("job-10").error.code != "invalid_response") {
        return fail("board job client terminal flag consistency");
    }
    return 0;
}

int testJsonValueContracts()
{
    testtools::JsonValue value;
    testtools::JsonError error;
    testtools::JsonLimits limits;
    if (!testtools::parseJson(R"({"text":"中文","items":[true,null,-7]})", value, error) ||
        !value.isObject() || value.find("items")->arrayItems().size() != 3) {
        return fail("shared JsonValue strict parse");
    }
    const auto expect =
        [&](const std::string& input, testtools::JsonErrorCode code, const char* name) {
            if (testtools::parseJson(input, value, error) || error.code != code)
                return fail(name);
            return 0;
        };
    if (expect(R"({"a":1,"a":2})",
               testtools::JsonErrorCode::DuplicateObjectKey,
               "JsonValue duplicate key rejection") ||
        expect("true trailing",
               testtools::JsonErrorCode::TrailingData,
               "JsonValue trailing data rejection") ||
        expect("{\"a\":",
               testtools::JsonErrorCode::UnexpectedEnd,
               "JsonValue truncated input rejection") ||
        expect("9223372036854775808",
               testtools::JsonErrorCode::IntegerOverflow,
               "JsonValue integer overflow rejection") ||
        expect(
            "1.5", testtools::JsonErrorCode::NonIntegerNumber, "JsonValue non-integer rejection") ||
        expect(std::string("\"\xC0\xAF\"", 4),
               testtools::JsonErrorCode::InvalidUtf8,
               "JsonValue invalid UTF-8 rejection")) {
        return 1;
    }
    limits.max_input_bytes = 3;
    if (testtools::parseJson("null ", value, error, limits) ||
        error.code != testtools::JsonErrorCode::InputTooLarge) {
        return fail("JsonValue input limit");
    }
    limits = {};
    limits.max_depth = 2;
    if (testtools::parseJson("[[[0]] ]", value, error, limits) ||
        error.code != testtools::JsonErrorCode::DepthLimit) {
        return fail("JsonValue depth limit");
    }
    limits = {};
    limits.max_string_bytes = 2;
    if (testtools::parseJson(R"("abc")", value, error, limits) ||
        error.code != testtools::JsonErrorCode::StringLimit) {
        return fail("JsonValue string limit");
    }
    limits = {};
    limits.max_array_items = 2;
    if (testtools::parseJson("[1,2,3]", value, error, limits) ||
        error.code != testtools::JsonErrorCode::ArrayLimit) {
        return fail("JsonValue array limit");
    }
    limits = {};
    limits.max_object_members = 1;
    if (testtools::parseJson(R"({"a":1,"b":2})", value, error, limits) ||
        error.code != testtools::JsonErrorCode::ObjectLimit) {
        return fail("JsonValue object limit");
    }
    limits = {};
    limits.max_output_bytes = 3;
    std::string encoded;
    if (testtools::serializeJson(testtools::JsonValue::string("abc"), encoded, error, limits) ||
        error.code != testtools::JsonErrorCode::OutputTooLarge) {
        return fail("JsonValue output limit");
    }
    return 0;
}

} // namespace

int main()
{
    {
        using S = testtools::LocalPortChoiceState;
        const auto choose = testtools::chooseLocalPort;
        if (choose("COM3", false, {"COM1", "COM3"}, {"COM1", "COM3", "COM5"}, true).endpoint != "COM3" ||
            choose("com03", false, {"COM3"}, {"COM3"}, true).endpoint != "COM3")
            return fail("refresh retains available selection and normalizes PC aliases");
        if (choose("COM3", false, {"COM1", "COM3"}, {"COM1", "COM5"}, true).endpoint != "COM5" ||
            !choose("COM3", false, {"COM1", "COM3"}, {"COM1", "COM5", "COM6"}, true).endpoint.empty() ||
            !choose("COM3", false, {"COM1", "COM3"}, {"COM1", "COM5"}, false).endpoint.empty())
            return fail("only an explicit refresh chooses a sole new port, never an arbitrary survivor");
        if (choose("COM3", false, {}, {"COM1"}, false).state != S::Missing ||
            choose("", false, {}, {}, false).state != S::Empty ||
            choose("", false, {}, {"COM1", "COM2"}, false).state != S::Choose ||
            choose("", false, {}, {"COM5"}, false).endpoint != "COM5")
            return fail("missing, empty and initial port choices are distinct");
        const auto manual = choose("COM99", true, {"COM3"}, {"COM5"}, true);
        if (manual.endpoint != "COM99" || manual.state != S::Manual)
            return fail("an explicit manual endpoint survives enumeration");

        const auto directory = std::filesystem::temp_directory_path() /
            ("testtools-pref-writer-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const auto path = directory / "preferences.json";
        std::mutex gateMutex;
        std::condition_variable gate;
        bool entered = false, release = false;
        std::vector<std::string> saved;
        {
            testtools::SerialPreferencesWriter writer(path,
                [&](const auto& file, const auto& prefs, std::string& error) {
                    const auto& endpoint = prefs.channels[0].endpoint;
                    if (endpoint == "first") {
                        std::unique_lock<std::mutex> lock(gateMutex);
                        entered = true;
                        gate.notify_one();
                        gate.wait(lock, [&] { return release; });
                    }
                    if (endpoint == "fail") {
                        error = "simulated storage failure";
                        return false;
                    }
                    saved.push_back(endpoint);
                    return testtools::SerialPreferencesStore::save(file, prefs, error);
                });
            testtools::SerialWorkspacePreferences prefs;
            prefs.channels[0].endpoint = "first";
            writer.submit(prefs);
            bool started;
            {
                std::unique_lock<std::mutex> lock(gateMutex);
                started = gate.wait_for(lock, std::chrono::seconds(2), [&] { return entered; });
            }
            prefs.channels[0].endpoint = "second";
            writer.submit(prefs);
            prefs.channels[0].endpoint = "latest";
            prefs.channels[0].manualEndpoint = true;
            writer.submit(prefs);
            {
                std::lock_guard<std::mutex> lock(gateMutex);
                release = true;
            }
            gate.notify_one();
            writer.waitIdle();
            testtools::SerialWorkspacePreferences loaded;
            std::string error;
            if (!started || writer.pending() || saved != std::vector<std::string>{"first", "latest"} ||
                !testtools::SerialPreferencesStore::load(path, loaded, error) ||
                loaded.channels[0].endpoint != "latest" || !loaded.channels[0].manualEndpoint)
                return fail("serialized preferences writer coalesces to the latest snapshot");
            prefs.channels[0].endpoint = "fail";
            writer.submit(prefs);
            writer.waitIdle();
            if (writer.error() != "simulated storage failure")
                return fail("background preference errors are observable");
            prefs.channels[0].endpoint = "recovered";
            writer.submit(prefs);
            writer.waitIdle();
            if (!writer.error().empty())
                return fail("a successful settings retry clears its error");
            prefs.channels[0].endpoint = "on-exit";
            writer.submit(prefs);
        }
        testtools::SerialWorkspacePreferences loaded;
        std::string error;
        if (!testtools::SerialPreferencesStore::load(path, loaded, error) ||
            loaded.channels[0].endpoint != "on-exit")
            return fail("writer destruction drains the accepted final snapshot");
        std::error_code ec;
        std::filesystem::remove(path, ec);
        std::filesystem::remove(directory, ec);
    }
    {
        testtools::SerialPortClaims claims;
        std::string error;
        const auto key = testtools::SerialPortClaims::resourceKey(false, {}, "COM3");
        if (key != testtools::SerialPortClaims::resourceKey(false, {}, "\\\\.\\com03"))
            return fail("local port claims normalize Windows aliases");
        auto terminalClaim = claims.acquire(key, "terminal", error);
        if (!terminalClaim || claims.acquire(key, "monitor", error) ||
            error.find("terminal") == std::string::npos)
            return fail("independent pages cannot claim the same serial port");
        terminalClaim.reset();
        if (!claims.acquire(key, "monitor", error))
            return fail("serial claim releases on disconnect or failure");
        const auto board = testtools::SerialPortClaims::resourceKey(true, "A", "/dev/ttyS1");
        auto boardClaim = claims.acquire(board, "terminal", error);
        if (!boardClaim ||
            claims.acquire(testtools::SerialPortClaims::resourceKey(true, "A", "/dev/ttyS9"), "monitor", error) ||
            !claims.acquire(testtools::SerialPortClaims::resourceKey(true, "B", "/dev/ttyS1"), "other board", error))
            return fail("Agent serial claims keep per-device single-session scope");
    }
    {
        auto backend = std::make_unique<FakeMirrorBackend>();
        auto* fixture = backend.get();
        testtools::ScreenMirrorService mirror(std::move(backend));
        if (mirror.start({"A", "unauthorized", {}}).success || fixture->starts != 0)
            return fail("mirror refuses unauthorized device without starting backend");
        if (!mirror.start({"A", "device", {}}).success || mirror.snapshot().serial != "A")
            return fail("mirror captures selected device");
        if (mirror.start({"B", "device", {}}).success || fixture->starts != 1 || fixture->serial != "A")
            return fail("mirror does not migrate running session on selection change");
        if (!mirror.control(testtools::MirrorControl::Home).success
            || fixture->lastControl != testtools::MirrorControl::Home
            || !mirror.screenshot("capture.png").success || fixture->serial != "A")
            return fail("mirror controls and screenshot belong to current mirror target");
        fixture->status = {false, 2, "device disconnected"};
        if (mirror.poll().running || mirror.snapshot().detail != "device disconnected")
            return fail("mirror process failure reaches state");
        if (!mirror.start({"B", "device", {}}).success || fixture->serial != "B")
            return fail("mirror can restart on newly selected device after exit");
        mirror.requestShutdown(); mirror.stop();
        if (!fixture->cancelled || mirror.snapshot().running || mirror.control(testtools::MirrorControl::Back).success
            || mirror.start({"B", "device", {}}).success)
            return fail("mirror shutdown rejects new device operations");
    }
    if (testJsonValueContracts() != 0) {
        return 1;
    }
    if (testBoardTestJobClient() != 0) {
        return 1;
    }
    const auto androidArm32 = testtools::parseAndroidAgentAbi("armeabi-v7a");
    const auto androidArm64 = testtools::parseAndroidAgentAbi("arm64-v8a");
    const auto linuxArm32 = testtools::parseLinuxAgentMachine("armv7l");
    const auto linuxArm64 = testtools::parseLinuxAgentMachine("aarch64");
    const auto ubuntuGlibc = testtools::parseGlibcVersion("ldd (Ubuntu GLIBC 2.35-0ubuntu3) 2.35");
    const auto buildrootGlibc =
        testtools::parseGlibcVersion("GNU C Library (Buildroot) stable release version 2.42.");
    if (!androidArm32 || *androidArm32 != testtools::AgentArchitecture::ArmV7 || !androidArm64 ||
        *androidArm64 != testtools::AgentArchitecture::Arm64 || !linuxArm32 ||
        *linuxArm32 != testtools::AgentArchitecture::ArmV7 || !linuxArm64 ||
        *linuxArm64 != testtools::AgentArchitecture::Arm64 || !ubuntuGlibc ||
        *ubuntuGlibc != "2.35" || !buildrootGlibc || *buildrootGlibc != "2.42" ||
        !testtools::glibcVersionAtLeast(*buildrootGlibc, 2, 31) ||
        testtools::glibcVersionAtLeast("2.30", 2, 31) ||
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
    std::string controlError;
    fakeTransportView->controlsSupported = true;
    if (!session.supportsControlLine(serialdemo::SerialControlLine::Dtr) ||
        !session.setControlLine(sessionGeneration, serialdemo::SerialControlLine::Dtr, true, controlError) ||
        session.setControlLine(sessionGeneration - 1, serialdemo::SerialControlLine::Dtr, false, controlError) ||
        fakeTransportView->controlValues != std::vector<bool>{true}) {
        return fail("control line stale generation must not reach transport");
    }
    fakeTransportView->failControls = true;
    if (session.setControlLine(sessionGeneration, serialdemo::SerialControlLine::Rts, false, controlError) ||
        controlError != "fake control failure" || session.snapshot().sentBytes != 0) {
        return fail("control line driver failure must not count as data TX");
    }
    fakeTransportView->failControls = false;
    fakeTransportView->controlsSupported = false;
    if (session.setControlLine(sessionGeneration, serialdemo::SerialControlLine::Dtr, false, controlError)) {
        return fail("unsupported control line rejected");
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
    session.resetCounters();
    sessionSnapshot = session.snapshot();
    if (sessionSnapshot.receivedBytes != 0 || sessionSnapshot.sentBytes != 0) {
        return fail("serial session counter reset");
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

    if (workspace.evaluateModeSwitch(testtools::SerialWorkspaceMode::Terminal, activity).allowed) {
        return fail("terminal cannot hide active channel A");
    }
    activity.channelActive = {false, true};
    if (!workspace.applyMode(testtools::SerialWorkspaceMode::Terminal, activity)
        || !workspace.applyMode(testtools::SerialWorkspaceMode::Single, activity)) {
        return fail("single/terminal share active channel B");
    }
    activity.scheduledSendRunning[1] = true;
    if (workspace.evaluateModeSwitch(testtools::SerialWorkspaceMode::Terminal, activity).allowed) {
        return fail("terminal switch preserves running scheduled send guard");
    }

    using TerminalKey = testtools::TerminalKey;
    if (testtools::terminalKeyBytes(TerminalKey::Enter, serialdemo::LineEnding::Cr) != serialdemo::ByteBuffer{13}
        || testtools::terminalKeyBytes(TerminalKey::Up, serialdemo::LineEnding::Cr) != serialdemo::ByteBuffer{27,'[','A'}
        || testtools::terminalPasteBytes("a\r\nb\nc\rd", serialdemo::LineEnding::CrLf)
            != serialdemo::ByteBuffer{'a',13,10,'b',13,10,'c',13,10,'d'}) {
        return fail("terminal keys and pasted line endings");
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

    testtools::SerialLogBuffer logBuffer;
    const auto txResult = logBuffer.appendTx({0x41, 0x54, 0x0D, 0x0A}, 1000, "10:00:00.001");
    if (txResult.addedEntries != 1 ||
        testtools::SerialLogBuffer::formatEntry(logBuffer.entries().front(), false, true) !=
            "[10:00:00.001] TX  AT\\r\\n\n" ||
        testtools::SerialLogBuffer::formatEntry(logBuffer.entries().front(), true, false) !=
            "TX  41 54 0D 0A\n") {
        return fail("serial log buffer TX formatting");
    }
    // A receive boundary is retained even at the same timestamp, without an LF.
    logBuffer.appendRx({0x41}, 1001, "10:00:00.001");
    logBuffer.appendRx({0x42}, 1001, "10:00:00.001");
    if (logBuffer.size() != 3 ||
        logBuffer.formatAll(false, false) != "TX  AT\\r\\n\nRX  A\nRX  B\n") {
        return fail("serial log separates back-to-back receive callbacks");
    }
    // Incomplete UTF-8 is visible as escaped bytes, never hidden or merged later.
    const auto rxPart1 = logBuffer.appendRx({0xE4, 0xB8}, 1010, "10:00:00.010");
    if (rxPart1.addedEntries != 1 ||
        testtools::SerialLogBuffer::formatEntry(logBuffer.entries().back(), false, false) !=
            "RX  \\xE4\\xB8\n") {
        return fail("serial log buffer incomplete trailing UTF-8 escaping");
    }
    const auto rxPart2 =
        logBuffer.appendRx({0xAD, 0x4F, 0x4B, 0x0D, 0x0A}, 1050, "10:00:00.050");
    if (rxPart2.addedEntries != 1 || logBuffer.entries().size() != 5 ||
        testtools::SerialLogBuffer::formatEntry(logBuffer.entries().back(), false, false) !=
            "RX  \\xADOK\n" ||
        testtools::SerialLogBuffer::formatEntry(logBuffer.entries().back(), true, true) !=
            "[10:00:00.050] RX  AD 4F 4B 0D 0A\n" ||
        logBuffer.entries()[3].bytes != serialdemo::ByteBuffer{0xE4, 0xB8}) {
        return fail("serial log receive boundaries and original bytes");
    }

    const auto lines = logBuffer.appendRx({0xE4, 0xB8, 0xAD, 'O', 'K', '\r', '\n', 'C'},
                                          1051, "10:00:00.051");
    if (lines.addedEntries != 2 || logBuffer.entries().back().bytes != serialdemo::ByteBuffer{'C'}) {
        return fail("serial log splits embedded newlines within a receive callback");
    }
    const auto unfiltered = logBuffer.formatAll(false, false);
    const auto retainedBytes = logBuffer.totalBytes();
    if (logBuffer.formatAll(false, false, "中OK") != "RX  中OK\n" ||
        logBuffer.formatAll(true, false, "E4 B8 AD").find("E4 B8 AD") == std::string::npos ||
        !logBuffer.formatAll(false, false, "missing").empty() ||
        logBuffer.totalBytes() != retainedBytes || logBuffer.formatAll(false, false) != unfiltered) {
        return fail("log filter preserves UTF-8, HEX and retained data");
    }
    testtools::SerialLogBuffer limitedLog(1);
    limitedLog.appendRx({0x41, 0x0A}, 1, "");
    limitedLog.appendRx({0x42, 0x0A}, 2, "");
    if (!limitedLog.truncated() || limitedLog.formatAll(false, false) != "RX  B\n") {
        return fail("log retention exposes truncation");
    }
    limitedLog.clear();
    if (limitedLog.truncated()) return fail("log clear resets truncation indicator");
    limitedLog.appendTx({0x7f}, 1, "");
    if (limitedLog.formatAll(false, false) != "TX  \\x7F\n") return fail("monitor escapes terminal DEL bytes");

    testtools::AtCommandLibrary atCommands;
    std::string atCommandError;
    if (atCommands.commands().empty() ||
        !atCommands.add({"\u8fd0\u8425\u5546", "AT+COPS?"}, atCommandError) ||
        !atCommands.add({"\u63e1\u624b\u5e27", "55 aa 01 0d 0a", true}, atCommandError) ||
        atCommands.add({"\u9519\u8befHEX", "55 ZZ", true}, atCommandError) ||
        atCommands.add({"\u8fd0\u8425\u5546", "AT+COPS?"}, atCommandError)) {
        return fail("AT command library validation");
    }
    atCommands.setLineEnding(testtools::AtCommandLineEnding::Lf);
    const auto atCommandPath =
        std::filesystem::temp_directory_path() /
        ("testtools-at-commands-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
         ".json");
    if (!testtools::AtCommandStore::save(atCommandPath, atCommands, atCommandError)) {
        return fail("AT command store save");
    }
    testtools::AtCommandLibrary loadedAtCommands;
    if (!testtools::AtCommandStore::load(atCommandPath, loadedAtCommands, atCommandError) ||
        loadedAtCommands.commands().size() != atCommands.commands().size() ||
        !loadedAtCommands.commands().back().hex ||
        loadedAtCommands.commands().back().command != "55 AA 01 0D 0A" ||
        loadedAtCommands.lineEnding() != testtools::AtCommandLineEnding::Lf) {
        std::error_code cleanupError;
        std::filesystem::remove(atCommandPath, cleanupError);
        return fail("AT command store roundtrip");
    }
    testtools::AtCommandLibrary importedCommands;
    importedCommands.setLineEnding(testtools::AtCommandLineEnding::CrLf);
    if (!testtools::AtCommandStore::importCommands(atCommandPath, importedCommands, atCommandError) ||
        importedCommands.commands().size() != atCommands.commands().size() ||
        !importedCommands.commands().back().hex ||
        importedCommands.lineEnding() != testtools::AtCommandLineEnding::CrLf) {
        return fail("command import keeps active line ending");
    }
    {
        std::ofstream corrupt(atCommandPath, std::ios::binary | std::ios::trunc);
        corrupt << R"({"schema_version":2,"line_ending":"lf","commands":[{"name":"bad","command":"ZZ","hex":true}]})";
    }
    if (testtools::AtCommandStore::importCommands(atCommandPath, importedCommands, atCommandError) ||
        importedCommands.commands().size() != atCommands.commands().size() ||
        importedCommands.lineEnding() != testtools::AtCommandLineEnding::CrLf ||
        testtools::AtCommandStore::importCommands(atCommandPath.string() + ".missing", importedCommands, atCommandError)) {
        return fail("failed import preserves live command library");
    }
    {
        std::ofstream legacy(atCommandPath, std::ios::binary | std::ios::trunc);
        legacy << R"({"schema_version":1,"commands":[{"name":"AT","command":"AT","line_ending":"cr"}]})";
    }
    testtools::AtCommandLibrary migratedAtCommands;
    if (!testtools::AtCommandStore::load(atCommandPath, migratedAtCommands, atCommandError) ||
        migratedAtCommands.commands().size() != 1 ||
        migratedAtCommands.commands().front().command != "AT" ||
        migratedAtCommands.lineEnding() != testtools::AtCommandLineEnding::CrLf) {
        std::error_code cleanupError;
        std::filesystem::remove(atCommandPath, cleanupError);
        return fail("AT command store v1 migration");
    }
    {
        std::error_code cleanupError;
        std::filesystem::remove(atCommandPath, cleanupError);
    }

    testtools::SerialWorkspacePreferences prefs;
    prefs.terminalLineEnding = 1;
    prefs.terminalBackspace = 8;
    prefs.shortcutBarVisible = false;
    prefs.shortcutSidebarWidth = 320;
    prefs.channels[1].useAgent = true;
    prefs.channels[1].endpoint = "/dev/ttyUSB2";
    prefs.channels[1].baudRate = 921600;
    prefs.channels[1].receiveHex = true;
    prefs.channels[1].timestamp = true;
    prefs.channels[1].autoScroll = false;
    prefs.channels[1].sendHex = true;
    prefs.channels[1].highlightKeywords = true;
    std::string prefError;
    const auto prefPath =
        std::filesystem::temp_directory_path() /
        ("testtools-serial-prefs-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
         ".json");
    testtools::SerialWorkspacePreferences loadedPrefs;
    if (!testtools::SerialPreferencesStore::save(prefPath, prefs, prefError) ||
        !testtools::SerialPreferencesStore::load(prefPath, loadedPrefs, prefError) ||
        loadedPrefs.shortcutBarVisible != false || loadedPrefs.shortcutSidebarWidth != 320 || loadedPrefs.terminalLineEnding != 1
        || loadedPrefs.terminalBackspace != 8 || !loadedPrefs.channels[1].useAgent ||
        loadedPrefs.channels[1].endpoint != "/dev/ttyUSB2" ||
        loadedPrefs.channels[1].baudRate != 921600 || !loadedPrefs.channels[1].receiveHex ||
        !loadedPrefs.channels[1].timestamp || loadedPrefs.channels[1].autoScroll ||
        !loadedPrefs.channels[1].sendHex || !loadedPrefs.channels[1].highlightKeywords) {
        std::error_code cleanupError;
        std::filesystem::remove(prefPath, cleanupError);
        return fail("serial preferences store roundtrip");
    }
    {
        std::ofstream legacy(prefPath, std::ios::binary | std::ios::trunc);
        legacy << R"({"schema_version":1,"shortcut_bar_visible":true,"channels":[{},{}]})";
    }
    if (!testtools::SerialPreferencesStore::load(prefPath, loadedPrefs, prefError) ||
        !loadedPrefs.shortcutBarVisible || loadedPrefs.shortcutSidebarWidth != 240 || !loadedPrefs.channels[1].highlightKeywords) {
        std::filesystem::remove(prefPath);
        return fail("legacy serial preferences default sidebar width");
    }
    {
        std::ofstream legacy(prefPath, std::ios::binary | std::ios::trunc);
        legacy << R"({"schema_version":1,"terminal_font":"Cascadia Mono","terminal_font_size":14,"terminal_line_spacing":2,"channels":[{}, {"highlight_keywords":false}]})";
    }
    if (!testtools::SerialPreferencesStore::load(prefPath, loadedPrefs, prefError) ||
        loadedPrefs.terminalFontSize != 15 || loadedPrefs.terminalLineSpacing != 3 || loadedPrefs.channels[1].highlightKeywords)
        return fail("legacy typography upgrades while explicit highlight-off stays off");
    loadedPrefs.terminalFontSize = 14; loadedPrefs.terminalLineSpacing = 2;
    if (!testtools::SerialPreferencesStore::save(prefPath, loadedPrefs, prefError) ||
        !testtools::SerialPreferencesStore::load(prefPath, loadedPrefs, prefError) ||
        loadedPrefs.terminalFontSize != 14 || loadedPrefs.terminalLineSpacing != 2 || loadedPrefs.channels[1].highlightKeywords)
        return fail("explicit typography and highlight choices survive subsequent loads");
    {
        std::error_code cleanupError;
        std::filesystem::remove(prefPath, cleanupError);
    }

    std::atomic<int> completedTasks{0};
    testtools::BackgroundTaskGroup tasks;
    tasks.start([&completedTasks] { ++completedTasks; });
    tasks.start([&completedTasks] { ++completedTasks; });
    tasks.wait();
    if (completedTasks != 2 || tasks.retainedTaskCount() != 0) {
        return fail("background task ownership");
    }

    {
        const std::string hello = R"({"ok":true,"result":{"agent_version":"0.3.7"}})";
        const std::string capabilities = R"({"ok":true,"result":{"capabilities":[{"id":"serial.stream"},{"id":"memory.capacity"}]}})";
        const std::string systemInfo = R"({"ok":true,"result":{"os_release":"Linux","architecture":"aarch64","model":"rk3572","board":"evb","product":"p1","cpu_cores":4,"memory_total_kib":3987456,"effective_uid":0}})";
        std::string error;
        if (!testtools::validateAgentService(hello, capabilities, "0.3.7", error) ||
            testtools::validateAgentService(hello, capabilities, "0.3.8", error) ||
            testtools::validateAgentService(hello, R"({"ok":true,"result":{"capabilities":[]}})", "0.3.7", error) ||
            testtools::validateAgentService(hello, R"({"ok":false,"result":{"capabilities":[{"id":"serial.stream"}]}})", "0.3.7", error) ||
            testtools::validateAgentService("broken", capabilities, "0.3.7", error)) return fail("service readiness requires handshake, version and serial capability");

        const auto snapshot = testtools::parseAgentServiceSnapshot(hello, capabilities, systemInfo);
        if (snapshot.agentVersion != "0.3.7" || snapshot.systemInfo.osRelease != "Linux" ||
            snapshot.systemInfo.architecture != "aarch64" || snapshot.systemInfo.model != "rk3572" ||
            snapshot.systemInfo.board != "evb" || snapshot.systemInfo.product != "p1" ||
            snapshot.systemInfo.cpuCores != 4 || snapshot.systemInfo.memoryTotalKib != 3987456 ||
            snapshot.systemInfo.effectiveUid != 0 ||
            !snapshot.hasCapability("serial.stream") || !snapshot.hasCapability("memory.capacity") ||
            snapshot.hasCapability("missing.capability")) {
            return fail("agent service snapshot parsing");
        }
    }

    {
        serialdemo::SerialConfig config;
        config.baudRate = 9600;
        config.dataBits = 8;
        config.parity = serialdemo::Parity::None;
        config.stopBits = serialdemo::StopBits::One;
        if (serialdemo::minimumStressIntervalMs(config, 256, 10) != 284 ||
            serialdemo::minimumStressIntervalMs(config, 256, 500) != 500) {
            return fail("stress test minimum wire-time interval");
        }

        std::string detectionBuffer;
        std::string expectedFrame = "SERIAL_POC_A_TO_B_42";
        const serialdemo::ByteBuffer chunk1{'S', 'E', 'R', 'I', 'A', 'L', '_'};
        const serialdemo::ByteBuffer chunk2{'P', 'O', 'C', '_', 'A', '_', 'T', 'O', '_', 'B', '_', '4', '2', '\n'};
        if (serialdemo::appendAndMatchFrame(detectionBuffer, chunk1, expectedFrame) ||
            expectedFrame.empty() ||
            !serialdemo::appendAndMatchFrame(detectionBuffer, chunk2, expectedFrame) ||
            !expectedFrame.empty()) {
            return fail("bidirectional frame sliding buffer match");
        }

        serialdemo::PeriodicSendPlan plan;
        plan.start({std::chrono::milliseconds(10), 2});
        const auto now = serialdemo::PeriodicSendPlan::Clock::now();
        plan.recordAttempt(true, now);
        plan.recordAttempt(true, now + std::chrono::milliseconds(10));
        serialdemo::IntegrityStreamVerifier verifier;
        verifier.push(serialdemo::encodeIntegrityFrame(0, 64));
        const auto partialReport = serialdemo::evaluateStressDirection(plan, verifier);
        if (partialReport.passed || partialReport.sentFrames != 2 || partialReport.lostFrames != 1) {
            return fail("stress direction evaluation detects lost frames");
        }
        verifier.push(serialdemo::encodeIntegrityFrame(1, 64));
        const auto passReport = serialdemo::evaluateStressDirection(plan, verifier);
        if (!passReport.passed || passReport.sentFrames != 2 || passReport.lostFrames != 0) {
            return fail("stress direction evaluation passes when all frames match");
        }
    }

    {
        testtools::SerialLinkVerifier link;
        auto frames = link.startProbe("p0", {serialdemo::LineEnding::CrLf, serialdemo::LineEnding::None});
        if (std::string(frames[0].begin(), frames[0].end()) != "SERIAL_POC_A_TO_B_p0\r\n")
            return fail("probe preserves wire token and per-endpoint line ending");
        const auto old = link.probe().generation;
        link.receive(1, {frames[0].begin(), frames[0].begin() + 7});
        if (link.probe().aToBPassed) return fail("probe must wait for complete fragmented token");
        link.receive(1, {frames[0].begin() + 7, frames[0].end()});
        link.receive(0, frames[1]);
        if (link.probe().running || !link.probe().aToBPassed || !link.probe().bToAPassed)
            return fail("probe directions complete independently");
        frames = link.startProbe("new", {serialdemo::LineEnding::None, serialdemo::LineEnding::None});
        if (link.expireProbe(old) || !link.probe().running || !link.expireProbe(link.probe().generation))
            return fail("old probe timeout cannot stop new probe");
        link.receive(1, frames[0]);
        if (link.probe().aToBPassed) return fail("expired probe cannot accept late data");

        std::array<serialdemo::SerialConfig, 2> configs;
        configs[0].baudRate = configs[1].baudRate = 9600;
        const auto now = testtools::SerialLinkVerifier::Clock::time_point{};
        const int interval = link.startStress(0, 2, 256, 10, configs, now);
        if (interval != 284 || !link.nextStressFrame(0, now) || link.nextStressFrame(1, now))
            return fail("stress scheduling preserves selected direction and physical rate limit");
        const auto first = link.nextStressFrame(0, now);
        link.recordStressAttempt(0, true, now); link.receive(1, first->bytes);
        if (link.nextStressFrame(0, now + std::chrono::milliseconds(283)) || link.beginReceiveWait(true))
            return fail("stress cannot send early or finish while frames remain");
        const auto later = now + std::chrono::milliseconds(interval);
        if (!link.nextStressFrame(0, later)) return fail("stress next frame due");
        link.recordStressAttempt(0, true, later);
        if (link.beginReceiveWait(false) || !link.beginReceiveWait(true) || link.nextStressFrame(0, later))
            return fail("stress waits for accepted writes before collecting tail bytes");
        const auto& direction = link.directions()[0];
        const auto report = serialdemo::evaluateStressDirection(direction.sendPlan, direction.verifier);
        if (report.passed || report.lostFrames != 1) return fail("extracted stress state preserves loss accounting");
        const auto generation = link.stressGeneration();
        link.stopStress();
        if (link.stressRunning() || link.awaitingReceive() || link.nextStressFrame(0, later))
            return fail("stress cancellation stops further submissions");
        if (!link.startStress(1, 1, 8, 10, configs, now) || link.stressGeneration() == generation ||
            link.directions()[0].transmitter != 1 || link.directions()[0].nextSequence != 0)
            return fail("new stress has fresh identity, direction and sequence");
        if (link.startStress(3, 1, 8, 10, configs, now) || !link.stressRunning())
            return fail("invalid stress request cannot replace an active state");
        link.recordStressAttempt(0, false, now);
        if (link.nextStressFrame(0, later)) return fail("failed stress submission stops its send plan");
        link.stopStress();
    }

    std::cout << "PASS core and application contracts without Qt or devices\n";
    return 0;
}
