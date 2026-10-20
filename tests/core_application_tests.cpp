#include "application/agent/agent_bundle.h"
#include "application/agent/agent_target.h"
#include "application/agent/board_profile.h"
#include "application/agent/board_test_job_client.h"
#include "application/serial/serial_session.h"
#include "application/serial/at_command_library.h"
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

int testBoardProfiles()
{
    const std::string profileText = R"({"profile_id":"bad"})";
    testtools::JsonValue document;
    testtools::JsonError jsonError;
    if (!testtools::parseJson(profileText, document, jsonError))
        return fail("Board Profile JSON");
    document = testtools::JsonValue::object(
        {{"profile_id", testtools::JsonValue::string("rk3572")},
         {"profile_version", testtools::JsonValue::integer(2)},
         {"identity",
          testtools::JsonValue::object({{"board", testtools::JsonValue::string("evb")},
                                        {"product", testtools::JsonValue::string("p1")}})},
         {"memory",
          testtools::JsonValue::object(
              {{"memtotal_kib",
                testtools::JsonValue::object(
                    {{"min", testtools::JsonValue::integer(3900000)},
                     {"max", testtools::JsonValue::integer(4000000)}})}})}});
    testtools::Profile profile;
    std::string profileError;
    if (testtools::parseBoardProfile(
            testtools::JsonValue::object({{"profile_id", testtools::JsonValue::string("bad")}}),
            profile,
            profileError) ||
        profileError.empty()) {
        return fail("Board Profile parse");
    }
    if (!testtools::parseBoardProfile(document, profile, profileError) ||
        profile.profileId != "rk3572" || profile.profileVersion != 2 || !profile.memory ||
        profile.memory->minimumKiB != 3900000 || profile.memory->maximumKiB != 4000000) {
        return fail("Board Profile valid parse");
    }
    const auto root = std::filesystem::temp_directory_path() /
                      ("testtools-board-profiles-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root);
    std::ofstream(root / "invalid.json") << profileText;
    std::ofstream(root / "valid.json") << testtools::serializeJson(document);
    const auto loaded = testtools::loadBoardProfiles(root);
    if (loaded.ok || loaded.profiles.size() != 1 || loaded.errors.size() != 1 ||
        loaded.profiles.front().profileVersion != 2) {
        std::filesystem::remove_all(root, ec);
        return fail("Board Profile load diagnostics");
    }
    testtools::BoardIdentity identity{{{"board", "evb"}, {"product", "p1"}}};
    testtools::BoardObservations observations;
    observations.memory.memtotalKiB = 3950000;
    const std::vector<testtools::Profile> profiles{profile};
    const auto pass = testtools::evaluateBoardProfile(identity, observations, profiles);
    if (pass.verdict != testtools::ProfileVerdict::Pass || pass.matchedProfileCount != 1 ||
        !pass.profileId || *pass.profileId != "rk3572") {
        std::filesystem::remove_all(root, ec);
        return fail("Board Profile PASS evaluation");
    }
    observations.memory.memtotalKiB = 4100000;
    const auto failed = testtools::evaluateBoardProfileForCapability(
        identity, observations, profiles, "memory.capacity");
    if (failed.verdict != testtools::ProfileVerdict::Fail) {
        std::filesystem::remove_all(root, ec);
        return fail("Board Profile FAIL evaluation");
    }
    const auto verify =
        testtools::evaluateBoardProfileForCapability(identity, {}, profiles, "memory.capacity");
    if (verify.verdict != testtools::ProfileVerdict::Verify) {
        std::filesystem::remove_all(root, ec);
        return fail("Board Profile VERIFY evaluation");
    }
    auto duplicate = profile;
    duplicate.profileId = "rk3572-copy";
    const auto multiple =
        testtools::evaluateBoardProfile(identity, observations, {profile, duplicate});
    const auto noMatch = testtools::evaluateBoardProfile(
        testtools::BoardIdentity{{{"board", "other"}}}, observations, profiles);
    std::filesystem::remove_all(root, ec);
    if (multiple.verdict != testtools::ProfileVerdict::Verify ||
        multiple.matchedProfileCount != 2 || noMatch.verdict != testtools::ProfileVerdict::Verify ||
        noMatch.matchedProfileCount != 0) {
        return fail("Board Profile unique match guard");
    }

    auto storageProfile = profile;
    storageProfile.memory.reset();
    testtools::StorageExpectation storageExpectation;
    storageExpectation.type = "emmc";
    storageProfile.storage = storageExpectation;
    testtools::BoardObservations storageObservations;
    storageObservations.storage.push_back({"emmc", 32000000000ULL, {1, 1, 1}});
    const auto healthWithoutThreshold = testtools::evaluateBoardProfileForCapability(
        identity, storageObservations, {storageProfile}, "storage.device.health");
    if (healthWithoutThreshold.verdict != testtools::ProfileVerdict::Verify) {
        return fail("Board Profile health requires a health expectation");
    }
    storageObservations.storage.front().type.clear();
    const auto unknownStorageType = testtools::evaluateBoardProfileForCapability(
        identity, storageObservations, {storageProfile}, "storage.device.info");
    if (unknownStorageType.verdict != testtools::ProfileVerdict::Verify) {
        return fail("Board Profile unknown storage type remains VERIFY");
    }
    return 0;
}

} // namespace

int main()
{
    if (testJsonValueContracts() != 0 || testBoardProfiles() != 0) {
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
    if (txResult.updatedLastEntry || txResult.addedEntries != 1 ||
        testtools::SerialLogBuffer::formatEntry(logBuffer.entries().front(), false, true) !=
            "[10:00:00.001] TX  AT\\r\\n\n" ||
        testtools::SerialLogBuffer::formatEntry(logBuffer.entries().front(), true, false) !=
            "TX  41 54 0D 0A\n") {
        return fail("serial log buffer TX formatting");
    }
    // Split UTF-8 "中" (E4 B8 AD) + "OK\r\n" across two RX chunks within 250ms
    const auto rxPart1 = logBuffer.appendRx({0xE4, 0xB8}, 1010, "10:00:00.010");
    if (rxPart1.addedEntries != 1 ||
        testtools::SerialLogBuffer::formatEntry(logBuffer.entries().back(), false, false) !=
            "RX  \n") {
        return fail("serial log buffer incomplete trailing UTF-8 holdback");
    }
    const auto rxPart2 =
        logBuffer.appendRx({0xAD, 0x4F, 0x4B, 0x0D, 0x0A}, 1050, "10:00:00.050");
    if (!rxPart2.updatedLastEntry || rxPart2.addedEntries != 0 || logBuffer.entries().size() != 2 ||
        testtools::SerialLogBuffer::formatEntry(logBuffer.entries().back(), false, false) !=
            "RX  \xE4\xB8\xAD"
            "OK\n" ||
        testtools::SerialLogBuffer::formatEntry(logBuffer.entries().back(), true, true) !=
            "[10:00:00.010] RX  E4 B8 AD 4F 4B 0D 0A\n") {
        return fail("serial log buffer RX coalescing and split UTF-8 safety");
    }

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
    prefs.shortcutBarVisible = false;
    prefs.channels[1].useAgent = true;
    prefs.channels[1].endpoint = "/dev/ttyUSB2";
    prefs.channels[1].baudRate = 921600;
    prefs.channels[1].receiveHex = true;
    prefs.channels[1].timestamp = true;
    prefs.channels[1].autoScroll = false;
    prefs.channels[1].sendHex = true;
    std::string prefError;
    const auto prefPath =
        std::filesystem::temp_directory_path() /
        ("testtools-serial-prefs-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
         ".json");
    testtools::SerialWorkspacePreferences loadedPrefs;
    if (!testtools::SerialPreferencesStore::save(prefPath, prefs, prefError) ||
        !testtools::SerialPreferencesStore::load(prefPath, loadedPrefs, prefError) ||
        loadedPrefs.shortcutBarVisible != false || !loadedPrefs.channels[1].useAgent ||
        loadedPrefs.channels[1].endpoint != "/dev/ttyUSB2" ||
        loadedPrefs.channels[1].baudRate != 921600 || !loadedPrefs.channels[1].receiveHex ||
        !loadedPrefs.channels[1].timestamp || loadedPrefs.channels[1].autoScroll ||
        !loadedPrefs.channels[1].sendHex) {
        std::error_code cleanupError;
        std::filesystem::remove(prefPath, cleanupError);
        return fail("serial preferences store roundtrip");
    }
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

    std::cout << "PASS core and application contracts without Qt or devices\n";
    return 0;
}
