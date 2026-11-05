#include "agent/toolbox.h"
#include "application/toolbox/toolbox_service.h"
#include "core/toolbox/toolbox_contract.h"
#include "core/toolbox/iperf_json.h"
#include <chrono>
#include <iostream>
#include <fstream>
#include <future>
#include <iterator>
#include <stdexcept>
#include <thread>

using namespace testtools;
#if defined(TESTTOOLS_BOARD_GPIO_FIXTURES)
void runBoardGpioFixtures();
#endif
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
JsonValue readArgs() { return JsonValue::object({{"chip", JsonValue::string("gpiochip0")}, {"offset", JsonValue::integer(2)}}); }
JsonValue holdArgs() { auto value = readArgs(); value.objectItems()["level"] = JsonValue::integer(1); value.objectItems()["hold_sec"] = JsonValue::integer(1); value.objectItems()["drive"] = JsonValue::string("push_pull"); return value; }
class Platform final : public agent::IToolboxPlatform {
  public:
    std::atomic<int> calls{0}, released{0}; std::atomic_bool clientDone{false}, ignoreStop{false}, releaseOverride{false};
    std::shared_ptr<agent::ToolContext> last;
    JsonValue gpioInventory(const std::string&, std::int64_t, int) override { return JsonValue::object({{"schema", JsonValue::string("gpio.inventory/v1")}, {"chips", JsonValue::array()}, {"pins", JsonValue::array()}, {"total", JsonValue::integer(0)}, {"next_cursor", JsonValue::integer(0)}}); }
    JsonValue networkInventory() override { return JsonValue::object({{"interfaces", JsonValue::array()}}); }
    bool hasRunner() const override { return true; }
    agent::BoardTestRunResult run(const agent::BoardTestRunRequest& request, const std::atomic_bool& cancel) override
    {
        last = request.toolbox; clientDone = false; ++calls;
        agent::BoardTestRunResult result; result.provider = {"agent_native", "fixture", 1}; result.observationSchema = request.capabilityId + "/v1";
        last->update(request.capabilityId == "network.iperf3.server" ? "ready" : "holding");
        if (request.capabilityId != "gpio.pin.read")
            while (!releaseOverride && (ignoreStop || (!cancel && !last->expired() && !(request.capabilityId == "network.iperf3.server" && clientDone)))) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        ++released;
        result.cancelled = cancel; result.timedOut = agent::toolClockMs() >= last->leaseUntil.load();
        result.succeeded = !result.cancelled && !result.timedOut; result.exitCode = result.succeeded ? 0 : -1;
        result.observations = JsonValue::object({{"released", JsonValue::boolean(true)}}); return result;
    }
};
struct Fixture {
    std::shared_ptr<Platform> platform = std::make_shared<Platform>();
    agent::BoardTestJobManager jobs{[](const agent::BoardTestRunRequest& req, const std::atomic_bool& cancel) { return req.toolbox->execute(req, cancel); }};
    agent::ToolboxAgent agent{jobs, platform};
    const std::string owner = makeToolToken();
    JsonValue identity() { return JsonValue::object({{"owner_id", JsonValue::string(owner)}, {"instance_id", JsonValue::string(agent.instance())}}); }
    JsonValue start(const std::string& id, const JsonValue& args)
    {
        const auto ticket = agent.handle("toolbox.prepare", identity()); check(ticket.ok, "prepare failed");
        auto params = identity(); params.objectItems()["operation_id"] = *ticket.result.find("operation_id");
        params.objectItems()["capability_id"] = JsonValue::string(id); params.objectItems()["arguments"] = args;
        return params;
    }
    JsonValue jobParams(const JsonValue& state)
    {
        auto params = identity(); params.objectItems()["job_id"] = *state.find("job_id"); return params;
    }
    void waitStarted() { for (int i = 0; i < 200 && platform->calls == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1)); check(platform->calls > 0, "worker did not start"); }
};
class Connection final : public IAgentConnection {
  public:
    explicit Connection(Fixture& f, bool lose = false) : fixture(f), loseStart(lose) {}
    bool connect(const std::string&, std::uint16_t, std::string&) override { return true; }
    void close() override {}
    bool request(const std::string& text, std::string& response, std::string& error) override
    {
        JsonValue request; if (!parseJson(text, request, error)) return false;
        const auto method = jsonString(request, "method"); const auto reply = fixture.agent.handle(method, *request.find("params"));
        if (method == "job.start" && loseStart) { loseStart = false; error = "simulated lost start response"; return false; }
        auto envelope = JsonValue::object({{"protocol", JsonValue::integer(1)}, {"request_id", JsonValue::string(jsonString(request, "request_id"))}, {"ok", JsonValue::boolean(reply.ok)}});
        envelope.objectItems()[reply.ok ? "result" : "error"] = reply.ok ? reply.result : JsonValue::object({{"code", JsonValue::string(reply.code)}, {"message", JsonValue::string(reply.message)}});
        response = serializeJson(envelope); return true;
    }
    Fixture& fixture; bool loseStart;
};
class Host final : public IToolboxHost {
  public:
    explicit Host(std::shared_ptr<Platform> p) : platform(std::move(p)) {}
    std::shared_ptr<Platform> platform; int calls = 0, failAt = 0;
    JsonValue interfaces() override { return JsonValue::object(); }
    bool available(std::string&) override { return true; }
    bool validateRoute(std::uint32_t, const std::string&, const std::string&, std::string&) override { return true; }
    ToolProcessResult run(const std::vector<std::string>&, int, const std::atomic_bool&, const std::function<bool(const char*, std::size_t)>& sink) override
    {
        ++calls;
        if (failAt == calls) { platform->clientDone = true; return {true, false, false, 1, "fixture client failed"}; }
        const std::string records = "{\"event\":\"start\",\"data\":{\"connected\":[{\"local_host\":\"192.0.2.1\",\"remote_host\":\"192.0.2.2\"}]}}\n"
            "{\"event\":\"interval\",\"data\":{\"sum\":{\"bits_per_second\":90000000.4,\"end\":1.0}}}\n"
            "{\"event\":\"end\",\"data\":{\"sum_sent\":{\"bits_per_second\":90000000.5,\"retransmits\":0},\"sum_received\":{\"bits_per_second\":89900000.2}}}\n";
        const bool ok = sink(records.data(), records.size()); platform->clientDone = true;
        return {true, false, !ok, ok ? 0 : -1, {}};
    }
};
void validation()
{
    RockchipGpioNumber pinNumber; std::string conversionError;
    check(convertRockchipGpio("GPIO3_B4", pinNumber, conversionError) && pinNumber.number == 108 && pinNumber.offset == 12, "GPIO name conversion failed");
    check(convertRockchipGpio("108", pinNumber, conversionError) && pinNumber.name == "GPIO3_B4", "GPIO reverse conversion failed");
    check(convertRockchipGpio("gpio4_b7", pinNumber, conversionError) && pinNumber.number == 143, "GPIO case conversion failed");
    for (const auto* bad : {"-1", "4096", "GPIO4_E0", "GPIO3_B8", "GPIO128_A0", "1.2", ""})
        check(!convertRockchipGpio(bad, pinNumber, conversionError), "invalid GPIO conversion accepted");
    std::string error; auto args = readArgs(); check(validateToolArguments("gpio.pin.read", args, error), "valid read rejected");
    auto board = JsonValue::object({{"endpoint_id", JsonValue::string(std::string(32, 'a'))}, {"adapter", JsonValue::string("rk-gpio/v1")}});
    check(validateToolArguments("gpio.board.read", board, error), "valid board GPIO read rejected");
    board.objectItems()["level"] = JsonValue::integer(0);
    check(validateToolArguments("gpio.board.set", board, error), "valid board GPIO set rejected");
    board.objectItems()["level"] = JsonValue::integer(10);
    check(!validateToolArguments("gpio.board.set", board, error), "driver command bypassed board level whitelist");
    board.objectItems()["level"] = JsonValue::integer(1); board.objectItems()["path"] = JsonValue::string("/sys/class/rock_gpio/gpio1/value");
    check(!validateToolArguments("gpio.board.set", board, error), "arbitrary board GPIO path accepted");
    args.objectItems()["direction"] = JsonValue::string("input"); check(!validateToolArguments("gpio.pin.read", args, error), "read changed direction");
    args = readArgs(); args.objectItems()["chip"] = JsonValue::string("../gpiochip0"); check(!validateToolArguments("gpio.pin.read", args, error), "path traversal accepted");
    args = readArgs(); args.objectItems()["offset"] = JsonValue::integer(-1); check(!validateToolArguments("gpio.pin.read", args, error), "negative line accepted");
    check(!validIpv4("127.0.0.2") && !validIpv4("1.2.3.4.") && !validIpv4("1.02.3.4") &&
        !validIpv4("224.0.0.1") && !validIpv4("0.0.0.1") && !validIpv4("255.255.255.255"), "invalid IP accepted");
}
void parser()
{
    IperfJsonStream burst; std::string burstError;
    const std::string intervals = "{\"event\":\"interval\",\"data\":{\"sum\":{\"bits_per_second\":100,\"end\":0.5}}}\n"
        "{\"event\":\"interval\",\"data\":{\"sum\":{\"bits_per_second\":200,\"end\":1.0}}}\n"
        "{\"event\":\"interval\",\"data\":{\"sum\":{\"bits_per_second\":150,\"end\":1.5}}}\n";
    check(burst.push(intervals, burstError) && burst.samples().arrayItems().size() == 3 &&
        jsonInteger(burst.samples().arrayItems()[1], "bps") == 200, "batched intervals lost intermediate samples");
    IperfJsonStream parser; std::string error;
    const std::string record = "{\"event\":\"end\",\"data\":{\"sum\":{\"bits_per_second\":123.5,\"jitter_ms\":1.2345,\"lost_packets\":1,\"packets\":3}}}\n";
    for (const char c : record) check(parser.push(std::string(1, c), error), "split stream rejected");
    check(parser.finish(error), "terminal JSON rejected");
    check(jsonInteger(parser.metrics(), "aggregate_bps") == 124, "bps rounding incorrect");
    check(jsonInteger(parser.metrics(), "jitter_us") == 1235, "jitter rounding incorrect");
    check(jsonInteger(parser.metrics(), "loss_ppm") == 333333, "loss rounding incorrect");
    IperfJsonStream broken; check(!broken.push("not json\n", error), "malformed JSON accepted");
    IperfJsonStream missing; check(!missing.finish(error), "missing terminal accepted");
    IperfJsonStream cpu;
    check(cpu.push("{\"event\":\"end\",\"data\":{\"sum_sent\":{\"bits_per_second\":100},\"cpu_utilization_percent\":{\"host_total\":12.3,\"remote_total\":45.6}}}\n", error) && cpu.finish(error), "CPU source fixture rejected");
    check(jsonInteger(cpu.metrics(), "local_cpu_permille") == 123 && jsonInteger(cpu.metrics(), "remote_cpu_permille") == 456 &&
        !cpu.metrics().find("client_cpu_permille"), "CPU source was incorrectly tied to a client/server role");
}
void ownershipAndLease()
{
    Fixture fixture; const auto start = fixture.start("gpio.pin.write", holdArgs());
    const auto created = fixture.agent.handle("job.start", start); check(created.ok, "start failed"); fixture.waitStarted();
    auto params = fixture.jobParams(created.result);
    auto wrong = params; wrong.objectItems()["owner_id"] = JsonValue::string(makeToolToken());
    check(!fixture.agent.handle("job.cancel", wrong).ok, "foreign owner cancelled GPIO");
    const auto lease = fixture.platform->last->leaseUntil.load();
    check(fixture.agent.handle("job.progress", params).ok, "progress failed");
    auto futureCursor = params; futureCursor.objectItems()["cursor"] = JsonValue::integer(1);
    check(!fixture.agent.handle("job.progress", futureCursor).ok, "future event cursor accepted");
    check(fixture.platform->last->leaseUntil.load() == lease, "progress renewed lease");
    check(!fixture.jobs.start({"memory.capacity", 1}), "legacy job overlapped GPIO");
    check(fixture.agent.handle("job.start", start).ok && fixture.platform->calls == 1, "duplicate operation executed twice");
    auto changed = start; changed.find("arguments")->objectItems()["level"] = JsonValue::integer(0);
    check(!fixture.agent.handle("job.start", changed).ok, "duplicate with new payload accepted");
    auto stale = params; stale.objectItems()["instance_id"] = JsonValue::string(makeToolToken());
    check(!fixture.agent.handle("job.renew", stale).ok, "stale instance renewed GPIO");
    fixture.platform->last->leaseUntil = agent::toolClockMs() - 1;
    check(!fixture.agent.handle("job.renew", params).ok, "expired lease revived");
    for (int i = 0; i < 200 && fixture.platform->released == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    check(fixture.platform->released == 1, "lease did not release handle");
    fixture.jobs.stop();
}
void abandonedStart()
{
    Fixture fixture;
    const auto request = fixture.start("gpio.pin.write", holdArgs());
    auto abort = fixture.identity(); abort.objectItems()["operation_id"] = *request.find("operation_id");
    check(fixture.agent.handle("toolbox.abort", abort).ok, "prepare ticket abort failed");
    const auto late = fixture.agent.handle("job.start", request);
    check(!late.ok && late.code == "operation_aborted" && fixture.platform->calls == 0, "late start performed abandoned hardware operation");
    check(jsonString(fixture.agent.handle("job.progress", abort).result, "state") == "aborted", "abandoned ticket not queryable");
}
void serviceAndLostResponse()
{
    Fixture fixture; auto host = std::make_shared<Host>(fixture.platform); ToolboxService service(host);
    ToolboxRequest request; request.session.serial = "fixture"; request.session.generation = 7;
    request.session.openConnection = [&](std::string&) { return std::make_shared<Connection>(fixture, true); };
    request.capability = "gpio.pin.write"; request.arguments = holdArgs();
    std::string error;
    for (int trial = 0; trial < 8; ++trial) {
        check(service.prepare(request, false, error), "service prepare failed"); service.executePrepared();
        check(!service.snapshot().active && !service.snapshot().remotePending, "lost response did not reconcile safely");
        check(fixture.platform->calls == trial + 1 && fixture.platform->released == trial + 1, "lost response repeated hardware action");
    }
    request.session.openConnection = [&](std::string&) { return std::make_shared<Connection>(fixture); };
    request.networkTest = true; request.network.localInterface = 1;
    request.network.sourceIp = "192.0.2.1"; request.network.boardInterface = "eth0"; request.network.boardIp = "192.0.2.2";
    request.network.seconds = 1; request.network.direction = 2;
    check(service.prepare(request, false, error), "network service prepare failed"); service.executePrepared();
    const auto state = service.snapshot();
    check(state.state == "succeeded" && !state.remotePending, "simulated network task failed");
    check(host->calls == 2 && state.report.find("runs")->arrayItems().size() == 2, "sequential directions missing");
    host->calls = 0; host->failAt = 1;
    check(service.prepare(request, false, error), "failure-case prepare failed"); service.executePrepared();
    const auto failed = service.snapshot();
    check(failed.state == "failed" && host->calls == 1 &&
        jsonString(failed.report.find("runs")->arrayItems()[1], "state") == "not_run", "failed direction did not preserve unexecuted direction");
}
void boundedStop()
{
    Fixture fixture; fixture.platform->ignoreStop = true;
    ToolboxService service(std::make_shared<Host>(fixture.platform));
    ToolboxRequest request; request.session.serial = "fixture"; request.session.generation = 1;
    request.session.openConnection = [&](std::string&) { return std::make_shared<Connection>(fixture); };
    request.capability = "gpio.pin.write"; request.arguments = holdArgs(); request.arguments.objectItems()["hold_sec"] = JsonValue::integer(30);
    std::string error; check(service.prepare(request, false, error), "bounded-stop prepare failed");
    auto execution = std::async(std::launch::async, [&] { service.executePrepared(); }); fixture.waitStarted(); service.cancel();
    const bool bounded = execution.wait_for(std::chrono::seconds(7)) == std::future_status::ready;
    const auto state = service.snapshot();
    fixture.platform->releaseOverride = true; execution.wait(); fixture.jobs.stop();
    check(bounded && !state.active && state.remotePending && state.state == "unknown" && !state.complete &&
        jsonBoolean(state.report, "remote_pending") && state.report.find("finished_unix_ms"), "stop did not return bounded unknown state");
    check(!service.prepare(request, false, error), "unknown remote task did not block new output");
    check(service.reconcile(request.session, error), "finished original operation could not be reconciled");
    check(jsonString(service.snapshot().report, "state") == "failed" && !jsonBoolean(service.snapshot().report, "remote_pending"),
        "reconciled report retained an unknown active resource");
}
}
int main(int argc, char** argv)
{
    if (argc == 3 && std::string(argv[1]) == "--parse-iperf-output") {
        std::ifstream file(argv[2], std::ios::binary); std::string input((std::istreambuf_iterator<char>(file)), {}), error;
        IperfJsonStream parser;
        if (!file || !parser.push(input, error) || !parser.finish(error)) { std::cerr << error << '\n'; return 1; }
        std::cout << serializeJson(parser.metrics()) << '\n'; return 0;
    }
#if defined(TESTTOOLS_BOARD_GPIO_FIXTURES)
    try { runBoardGpioFixtures(); } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
#endif
    try { validation(); parser(); abandonedStart(); ownershipAndLease(); serviceAndLostResponse(); boundedStop(); std::cout << "Toolbox contracts passed (no hardware or network traffic).\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
