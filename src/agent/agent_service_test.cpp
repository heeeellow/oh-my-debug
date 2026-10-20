#include "agent_service.h"
#include "board_capability.h"
#include "board_test_job.h"
#include "core/protocol/agent_version.h"
#include "native_board_observations.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

namespace {
using namespace testtools::agent;
bool has(const std::string& s, const std::string& x)
{
    return s.find(x) != std::string::npos;
}
int bad(const char* n, const std::string& d = {})
{
    std::cerr << "FAIL " << n << ' ' << d << '\n';
    return 1;
}
bool done(BoardTestJobState s)
{
    return s == BoardTestJobState::succeeded || s == BoardTestJobState::failed ||
           s == BoardTestJobState::cancelled || s == BoardTestJobState::timed_out;
}
bool flag(const std::atomic_bool& f)
{
    for (int i = 0; i < 2000 && !f.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return f.load();
}
bool wait(BoardTestJobManager& m, std::uint64_t id, BoardTestJobSnapshot& out)
{
    for (int i = 0; i < 2000; ++i) {
        auto s = m.snapshot(id);
        if (s && done(s->state)) {
            out = *s;
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}
void put(const std::filesystem::path& p, const std::string& s)
{
    std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p);
    f << s;
}

struct Fixture {
    std::filesystem::path root;
    NativeObservationRoots roots;
    Fixture()
    {
        root = std::filesystem::temp_directory_path() /
               ("testtools-agent-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        roots.procRoot = root / "proc";
        roots.sysRoot = root / "sys";
        put(roots.procRoot / "meminfo", "MemTotal: 4096000 kB\n");
        auto b = roots.sysRoot / "block";
        put(b / "mmcblk0" / "removable", "0\n");
        put(b / "mmcblk0" / "size", "16384\n");
        put(b / "mmcblk0" / "stat", "1 2 3 4 5 6 7 8 9 10 11 12 13\n");
        put(b / "mmcblk0" / "device" / "manfid", "0x15\n");
        put(b / "mmcblk0" / "device" / "name", "TESTEMMC\n");
        put(b / "mmcblk0" / "device" / "fwrev", "1.2\n");
        put(b / "mmcblk0" / "device" / "serial", "abc\n");
        put(b / "mmcblk0" / "device" / "pre_eol_info", "1\n");
        put(b / "mmcblk0" / "device" / "life_time", "0x02 0x03\n");
        put(b / "mmcblk0p1" / "removable", "0\n");
        put(b / "mmcblk0p1" / "size", "32768\n");
        put(b / "mmcblk0boot0" / "removable", "0\n");
        put(b / "loop0" / "removable", "0\n");
    }
    ~Fixture()
    {
        std::error_code e;
        std::filesystem::remove_all(root, e);
    }
};

BoardTestRunResult successfulResult()
{
    BoardTestRunResult result;
    result.succeeded = true;
    result.exitCode = 0;
    result.provider = {"test", "fixture", 1};
    result.observationSchema = "fixture/v1";
    result.observations = testtools::JsonValue::object();
    return result;
}

int cancellationCompletionRace()
{
    Fixture fixture;
    std::atomic_bool entered{false};
    AgentService service(
        [&](const BoardTestRunRequest&, const std::atomic_bool& cancel) {
            entered = true;
            // The provider completed its work before noticing the cancellation.
            while (!cancel.load())
                std::this_thread::yield();
            return successfulResult();
        },
        [&] { return availableBoardCapabilities(fixture.roots); });
    const auto started = service.handleRequest(
        R"({"protocol":1,"request_id":"start","method":"job.start","params":{"capability_id":"memory.capacity"}})");
    if (!has(started, "\"job_id\":\"job-1\"") || !flag(entered))
        return bad("cancel/complete setup", started);
    const auto cancelled = service.handleRequest(
        R"({"protocol":1,"request_id":"cancel","method":"job.cancel","params":{"job_id":"job-1"}})");
    if (!has(cancelled, "\"ok\":true"))
        return bad("cancel accepted before completion", cancelled);
    for (int attempt = 0; attempt < 2000; ++attempt) {
        const auto status = service.handleRequest(
            R"({"protocol":1,"request_id":"status","method":"job.status","params":{"job_id":"job-1"}})");
        if (has(status, "\"state\":\"cancelled\"")) {
            if (!has(status, "\"completion\":\"cancelled\""))
                return bad("cancel/complete consistency", status);
            const auto lateCancel = service.handleRequest(
                R"({"protocol":1,"request_id":"late","method":"job.cancel","params":{"job_id":"job-1"}})");
            return has(lateCancel, "job_not_active") ? 0 : bad("late cancel", lateCancel);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return bad("cancel/complete convergence");
}

int responseSnapshotRace()
{
    BoardTestJobManager manager([](const BoardTestRunRequest&, const std::atomic_bool&) {
        return successfulResult();
    });
    const auto first = manager.start({"memory.capacity", 1});
    BoardTestJobSnapshot completed;
    if (!first || !wait(manager, first->jobId, completed))
        return bad("snapshot race setup");
    // Another control connection starts a task before the first reply is serialized.
    const auto second = manager.start({"storage.device.info", 1});
    const auto reply = first;
    if (!second || !reply || reply->jobId != first->jobId ||
        reply->state != BoardTestJobState::running || manager.snapshot(first->jobId))
        return bad("start reply snapshot lost during turnover");

    BoardTestJobManager cancelling([](const BoardTestRunRequest&, const std::atomic_bool& cancel) {
        while (!cancel.load())
            std::this_thread::yield();
        return successfulResult();
    });
    const auto active = cancelling.start({"memory.capacity", 1});
    if (!active)
        return bad("cancel snapshot setup");
    const auto cancelReply = cancelling.cancel(active->jobId);
    if (!cancelReply || !wait(cancelling, active->jobId, completed) ||
        completed.state != BoardTestJobState::cancelled || completed.result.succeeded ||
        !completed.result.cancelled)
        return bad("cancel snapshot convergence");
    const auto next = cancelling.start({"storage.device.info", 1});
    if (!next || cancelling.snapshot(active->jobId) ||
        cancelReply->jobId != active->jobId || cancelReply->state != BoardTestJobState::cancelling)
        return bad("cancel reply snapshot lost during turnover");
    return 0;
}

int concurrentReplies()
{
    Fixture fixture;
    const auto definitions = availableBoardCapabilities(fixture.roots);
    AgentService service(
        [](const BoardTestRunRequest&, const std::atomic_bool&) { return successfulResult(); },
        [definitions] { return definitions; });
    std::atomic_bool begin{false};
    std::atomic_int failures{0}, started{0};
    const auto caller = [&] {
        while (!begin.load())
            std::this_thread::yield();
        for (int i = 0; i < 300; ++i) {
            const auto response = service.handleRequest(
                R"({"protocol":1,"request_id":"start","method":"job.start","params":{"capability_id":"memory.capacity"}})");
            testtools::JsonValue document;
            std::string error;
            if (!testtools::parseJson(response, document, error)) {
                ++failures;
                continue;
            }
            const auto* result = document.find("result");
            if (!result) {
                if (!has(response, "resource_busy"))
                    ++failures;
                std::this_thread::yield();
                continue;
            }
            const auto* id = result->find("job_id");
            if (!id || !id->isString() || !has(response, "\"state\":\"running\"")) {
                ++failures;
                continue;
            }
            ++started;
            const auto cancellation = service.handleRequest(
                "{\"protocol\":1,\"request_id\":\"cancel\",\"method\":\"job.cancel\","
                "\"params\":{\"job_id\":\"" + id->stringValue() + "\"}}");
            if (!has(cancellation, "job_not_active") &&
                !(has(cancellation, "\"job_id\":\"" + id->stringValue() + "\"") &&
                  has(cancellation, "\"state\":\"cancelling\"")))
                ++failures;
        }
    };
    std::thread first(caller), second(caller);
    begin = true;
    first.join();
    second.join();
    return failures.load() == 0 && started.load() > 0
               ? 0 : bad("concurrent start/cancel replies");
}

int timeoutCompletionConsistency()
{
    Fixture fixture;
    const auto definitions = availableBoardCapabilities(fixture.roots);
    AgentService service(
        [](const BoardTestRunRequest&, const std::atomic_bool&) {
            auto result = successfulResult();
            // Timeout remains authoritative if a provider reports mixed flags.
            result.timedOut = true;
            result.cancelled = true;
            return result;
        },
        [definitions] { return definitions; });
    service.handleRequest(
        R"({"protocol":1,"request_id":"start","method":"job.start","params":{"capability_id":"memory.capacity"}})");
    for (int i = 0; i < 2000; ++i) {
        const auto status = service.handleRequest(
            R"({"protocol":1,"request_id":"status","method":"job.status","params":{"job_id":"job-1"}})");
        if (has(status, "\"state\":\"timed_out\""))
            return has(status, "\"completion\":\"timed_out\"")
                       ? 0 : bad("timeout completion consistency", status);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return bad("timeout completion convergence");
}

int jobs()
{
    std::atomic_bool entered{false}, release{false};
    BoardTestJobManager m([&](const BoardTestRunRequest&, const std::atomic_bool& c) {
        entered = true;
        while (!release.load() && !c.load())
            std::this_thread::yield();
        BoardTestRunResult r;
        r.provider = {"test", "fixture", 1};
        r.observationSchema = "fixture/v1";
        r.observations =
            testtools::JsonValue::object({{"value", testtools::JsonValue::string("ok")}});
        r.succeeded = !c.load();
        r.cancelled = c.load();
        r.exitCode = r.succeeded ? 0 : 130;
        r.log = std::string(70 * 1024, 'x');
        return r;
    });
    auto id = m.start({"memory.capacity", 1});
    if (!id || !flag(entered) || m.start({"storage.device.info", 1}))
        return bad("job start/busy");
    release = true;
    BoardTestJobSnapshot s;
    if (!wait(m, id->jobId, s) || s.state != BoardTestJobState::succeeded ||
        s.result.log.size() != 64 * 1024 || s.result.observationSchema != "fixture/v1")
        return bad("job success/limits");
    if (m.cancel(id->jobId) || !m.snapshot(id->jobId) ||
        m.snapshot(id->jobId)->state != BoardTestJobState::succeeded)
        return bad("completed job cannot be cancelled");
    BoardTestJobManager f([](const BoardTestRunRequest&, const std::atomic_bool&) {
        BoardTestRunResult r;
        r.summary = "failed";
        return r;
    });
    auto fi = f.start({"memory.capacity", 1});
    if (!fi || !wait(f, fi->jobId, s) || s.state != BoardTestJobState::failed)
        return bad("job failure");
    BoardTestJobManager t([](const BoardTestRunRequest&, const std::atomic_bool&) {
        BoardTestRunResult r;
        r.timedOut = true;
        return r;
    });
    auto ti = t.start({"memory.capacity", 1});
    if (!ti || !wait(t, ti->jobId, s) || s.state != BoardTestJobState::timed_out)
        return bad("job timeout");
    std::atomic_bool ce{false};
    BoardTestJobManager c([&](const BoardTestRunRequest&, const std::atomic_bool& x) {
        ce = true;
        while (!x.load())
            std::this_thread::yield();
        BoardTestRunResult r;
        r.cancelled = true;
        return r;
    });
    auto ci = c.start({"memory.capacity", 1});
    if (!ci || !flag(ce) || !c.cancel(ci->jobId) || !wait(c, ci->jobId, s) ||
        s.state != BoardTestJobState::cancelled)
        return bad("job cancel");
    BoardTestJobManager inv([](const BoardTestRunRequest&, const std::atomic_bool&) {
        BoardTestRunResult r;
        r.succeeded = true;
        return r;
    });
    auto ii = inv.start({"memory.capacity", 1});
    if (!ii || !wait(inv, ii->jobId, s) || s.state != BoardTestJobState::failed ||
        !has(s.result.summary, "invalid structured"))
        return bad("structured rejection");
    BoardTestJobManager oversized([](const BoardTestRunRequest&, const std::atomic_bool&) {
        BoardTestRunResult r;
        r.succeeded = true;
        r.provider = {"test", "fixture", 1};
        r.observationSchema = "fixture/v1";
        r.observations = testtools::JsonValue::object(
            {{"oversized", testtools::JsonValue::string(std::string(300 * 1024, 'o'))}});
        return r;
    });
    auto oi = oversized.start({"memory.capacity", 1});
    if (!oi || !wait(oversized, oi->jobId, s) || s.state != BoardTestJobState::failed ||
        !has(s.result.summary, "invalid structured"))
        return bad("observation size limit");
    if (m.start({"../escape", 1}) || m.snapshot(9999))
        return bad("job id boundary");

    std::atomic_bool destructorEntered{false};
    const auto destructorStart = std::chrono::steady_clock::now();
    {
        BoardTestJobManager stopping(
            [&destructorEntered](const BoardTestRunRequest&, const std::atomic_bool& cancel) {
                destructorEntered = true;
                while (!cancel.load())
                    std::this_thread::yield();
                BoardTestRunResult result;
                result.cancelled = true;
                return result;
            });
        const auto stoppingId = stopping.start({"memory.capacity", 1});
        if (!stoppingId || !flag(destructorEntered))
            return bad("destructor setup");
    }
    if (std::chrono::steady_clock::now() - destructorStart > std::chrono::seconds(2))
        return bad("destructor convergence");
    return 0;
}

int protocol()
{
    Fixture q;
    auto mem = readNativeMemoryCapacity(q.roots);
    auto dev = enumerateNativeStorage(q.roots);
    if (!mem.memTotalKiB || *mem.memTotalKiB != 4096000 || dev.size() != 1 ||
        dev[0].name != "mmcblk0" || dev[0].kind != StorageDeviceKind::eMMC ||
        !dev[0].capacityBytes || *dev[0].capacityBytes != 16384ULL * 512)
        return bad("native fixture");
    auto defs = availableBoardCapabilities(q.roots);
    if (defs.size() != 3)
        return bad("capability count");
    for (auto& d : defs)
        if (d.provider.kind != "agent_native" || d.provider.version != 1 || d.risks.size() != 1 ||
            d.risks[0] != "read_only" || d.resources.size() != 1 || d.observationSchema.empty() ||
            !d.supportsCancel || d.defaultTimeoutMs <= 0)
            return bad("capability metadata");
    std::atomic_bool notCancelled{false};
    const auto memoryResult = runBoardCapability({"memory.capacity", 1}, notCancelled, q.roots);
    const auto infoResult = runBoardCapability({"storage.device.info", 1}, notCancelled, q.roots);
    const auto healthResult =
        runBoardCapability({"storage.device.health", 1}, notCancelled, q.roots);
    if (!memoryResult.succeeded || memoryResult.provider.id != "linux_proc_meminfo" ||
        !infoResult.succeeded || !healthResult.succeeded ||
        !has(testtools::serializeJson(infoResult.observations), "capacity_bytes") ||
        !has(testtools::serializeJson(healthResult.observations), "life_time_a"))
        return bad("native provider dispatch");
    auto run = [&](const BoardTestRunRequest& r, const std::atomic_bool& c) {
        return runBoardCapability(r, c, q.roots);
    };
    AgentService a(run, [&q] { return availableBoardCapabilities(q.roots); });
    auto list = a.handleRequest(
        "{\"protocol\":1,\"request_id\":\"l\",\"method\":\"capability.list\",\"params\":{}}");
    if (!has(list, "memory.capacity") || !has(list, "storage.device.info") ||
        !has(list, "storage.device.health") || !has(list, "memory.capacity/v1") ||
        !has(list, "linux_proc_meminfo"))
        return bad("capability.list", list);
    AgentService duplicateRegistry(run, [defs] {
        auto duplicated = defs;
        duplicated.push_back(defs.front());
        return duplicated;
    });
    const auto duplicateList = duplicateRegistry.handleRequest(
        "{\"protocol\":1,\"request_id\":\"d\",\"method\":\"capability.list\",\"params\":{}}");
    const auto duplicateStart = duplicateRegistry.handleRequest(
        "{\"protocol\":1,\"request_id\":\"ds\",\"method\":\"job.start\","
        "\"params\":{\"capability_id\":\"memory.capacity\"}}");
    if (has(duplicateList, "\"id\":\"memory.capacity\"") ||
        !has(duplicateStart, "\"code\":\"capability_unavailable\""))
        return bad("duplicate capability rejection", duplicateList);
    const auto unavailable =
        a.handleRequest("{\"protocol\":1,\"request_id\":\"u\",\"method\":\"job.start\","
                        "\"params\":{\"capability_id\":\"storage.performance.sequential\"}}");
    if (!has(unavailable, "\"code\":\"capability_unavailable\""))
        return bad("unavailable capability", unavailable);
    auto st = a.handleRequest("{\"protocol\":1,\"request_id\":\"s\",\"method\":\"job.start\","
                              "\"params\":{\"capability_id\":\"memory.capacity\"}}");
    if (!has(st, "running"))
        return bad("job.start", st);
    std::string status;
    for (int i = 0; i < 200; ++i) {
        status = a.handleRequest("{\"protocol\":1,\"request_id\":\"q\",\"method\":\"job.status\","
                                 "\"params\":{\"job_id\":\"job-1\"}}");
        if (has(status, "succeeded"))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!has(status, "succeeded") || !has(status, "memtotal_kib") ||
        !has(status, "memory.capacity/v1"))
        return bad("structured status", status);
    std::atomic_bool ce{false};
    AgentService ca(
        [&](const BoardTestRunRequest&, const std::atomic_bool& x) {
            ce = true;
            while (!x.load())
                std::this_thread::yield();
            BoardTestRunResult r;
            r.cancelled = true;
            return r;
        },
        [&q] { return availableBoardCapabilities(q.roots); });
    auto cs = ca.handleRequest("{\"protocol\":1,\"request_id\":\"s\",\"method\":\"job.start\","
                               "\"params\":{\"capability_id\":\"storage.device.info\"}}");
    if (!flag(ce) || !has(ca.handleRequest("{\"protocol\":1,\"request_id\":\"c\",\"method\":\"job."
                                           "cancel\",\"params\":{\"job_id\":\"job-1\"}}"),
                          "cancelling"))
        return bad("protocol cancel", cs);
    const char* bads[] = {
        "not-json",
        "{\"protocol\":1,\"protocol\":1,\"request_id\":\"x\",\"method\":\"capability.list\","
        "\"params\":{}}",
        "{\"protocol\":1,\"request_id\":\"x\",\"method\":\"capability.list\",\"params\":{},"
        "\"extra\":1}",
        "{\"protocol\":1,\"request_id\":\"x\",\"method\":\"capability.list\",\"params\":{\"x\":1}}",
        "{\"protocol\":1,\"request_id\":\"x\",\"method\":\"job.start\",\"params\":{\"capability_"
        "id\":\"memory.capacity\",\"x\":1}}",
        "{\"protocol\":1,\"request_id\":\"x\",\"method\":\"job.status\",\"params\":{\"job_id\":"
        "\"job-1\",\"x\":1}}",
        "{\"protocol\":1,\"request_id\":\"x\",\"method\":\"job.cancel\",\"params\":{\"job_id\":\".."
        "/1\"}}"};
    for (auto r : bads)
        if (!has(a.handleRequest(r), "\"ok\":false"))
            return bad("strict JSON");
    return 0;
}
} // namespace
int main()
{
    auto h = AgentService().handleRequest(
        "{\"protocol\":1,\"request_id\":\"h\",\"method\":\"session.hello\",\"params\":{}}");
    if (!has(h, "\"ok\":true") || !has(h, testtools::AgentVersion))
        return 1;
    if (cancellationCompletionRace() | responseSnapshotRace())
        return 1;
    if (jobs() || protocol() || concurrentReplies() || timeoutCompletionConsistency())
        return 1;
    std::cout << "PASS generic read-only capability and structured job contract\n";
    return 0;
}
