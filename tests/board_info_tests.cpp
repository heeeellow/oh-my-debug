#include "board_info_fixture.h"
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace testtools;
namespace {
void check(bool value, const char* message)
{
    if (!value)
        throw std::runtime_error(message);
}
BoardInfoSnapshot run(const std::shared_ptr<boardfixture::Connection>& connection)
{
    BoardInfoService service;
    std::string error;
    check(service.prepare(boardfixture::request(connection), false, error), error.c_str());
    service.executePrepared();
    return service.snapshot();
}
void waitStart(const std::shared_ptr<boardfixture::Connection>& connection)
{
    while (connection->starts().empty())
        std::this_thread::yield();
}
} // namespace
int main()
{
    try {
        auto connection = std::make_shared<boardfixture::Connection>();
        BoardInfoService service;
        std::string error;
        auto request = boardfixture::request(connection);
        check(!service.prepare(request, true, error) && connection->calls == 0,
              "maintenance rejects before RPC");
        auto invalid = request;
        invalid.capabilities = {"arbitrary.shell"};
        check(!service.prepare(invalid, false, error), "only fixed read-only capabilities allowed");
        invalid = request;
        invalid.session.capabilities.clear();
        check(!service.prepare(invalid, false, error), "undeclared single/batch unavailable");
        invalid = request;
        *invalid.session.alive = false;
        check(!service.prepare(invalid, false, error), "stale session rejected");
        request = boardfixture::request(connection);
        check(service.prepare(request, false, error), "batch prepared");
        const auto firstId = service.snapshot().id;
        check(!service.prepare(request, false, error), "single board batch gate");
        service.executePrepared();
        auto state = service.snapshot();
        check(!state.active && !state.remotePending &&
                  connection->starts() == BoardInfoService::capabilities(),
              "three reads execute in exact order");
        for (const auto& row : state.records)
            check(row.state == BoardInfoState::Completed && !row.fields.empty() && row.job &&
                      row.job->result,
                  "terminal observations retained before next start");
        check(state.records[0].fields[0].value.find("3987456") != std::string::npos,
              "MemTotal exact KiB retained");
        check(state.records[1].fields.size() == 14, "all two storage candidates retained");
        check(state.records[2].fields[0].value == "0" &&
                  state.records[2].fields[2].value == "未知" &&
                  state.records[2].fields[3].value == "0 字节",
              "reported zero and unknown are distinct");
        request.capabilities = {"memory.capacity"};
        connection->missing = true;
        check(service.prepare(request, false, error), "single read prepared");
        service.executePrepared();
        state = service.snapshot();
        check(state.records[0].state == BoardInfoState::Completed &&
                  state.records[0].fields[0].value == "未知" &&
                  state.records[1].fields.size() == 14,
              "missing data remains unknown and other item results stay");
        connection->mode = boardfixture::Mode::Hold;
        connection->missing = false;
        check(service.prepare(request, false, error), "held task prepared");
        service.cancel(firstId);
        check(!service.snapshot().cancelling, "stale stop ID cannot stop later read");
        const auto before = connection->starts().size();
        std::thread worker([&] { service.executePrepared(); });
        while (connection->starts().size() == before)
            std::this_thread::yield();
        service.executePrepared();
        service.cancel(service.snapshot().id);
        worker.join();
        check(connection->starts().size() == before + 1 &&
                  service.snapshot().records[0].state == BoardInfoState::Cancelled &&
                  !service.snapshot().remotePending,
              "duplicate worker and cancellation drain");
        connection = std::make_shared<boardfixture::Connection>();
        request = boardfixture::request(connection);
        request.session.capabilities = {"memory.capacity"};
        BoardInfoService partial;
        check(partial.prepare(request, false, error), "partial capabilities prepared");
        partial.executePrepared();
        state = partial.snapshot();
        check(connection->starts().size() == 1 &&
                  state.records[1].state == BoardInfoState::Unavailable,
              "only Agent-declared capabilities execute");
        for (const auto mode : {boardfixture::Mode::BadSchema,
                                boardfixture::Mode::WrongJob,
                                boardfixture::Mode::WrongCapability,
                                boardfixture::Mode::Lost}) {
            connection = std::make_shared<boardfixture::Connection>();
            connection->mode = mode;
            state = run(connection);
            check(state.records[0].state == BoardInfoState::Failed,
                  "invalid schema, correlation and transport failures are not read complete");
            if (mode != boardfixture::Mode::BadSchema)
                check(connection->starts().size() == 1 && state.remotePending,
                      "uncertain response stops subsequent jobs");
        }
        connection = std::make_shared<boardfixture::Connection>();
        connection->mode = boardfixture::Mode::FailMemory;
        state = run(connection);
        check(state.records[0].state == BoardInfoState::Failed &&
                  state.records[1].state == BoardInfoState::Completed,
              "item failure does not erase later observations");
        connection = std::make_shared<boardfixture::Connection>();
        connection->mode = boardfixture::Mode::Hold;
        request = boardfixture::request(connection);
        request.timeout = std::chrono::milliseconds(8);
        BoardInfoService timeout;
        check(timeout.prepare(request, false, error), "timeout prepared");
        timeout.executePrepared();
        state = timeout.snapshot();
        check(state.records[0].state == BoardInfoState::TimedOut &&
                  state.records[1].state == BoardInfoState::Cancelled && !state.remotePending,
              "timeout cancels and drains before skipping next read");
        connection = std::make_shared<boardfixture::Connection>();
        connection->mode = boardfixture::Mode::IgnoreCancel;
        state = run(connection);
        check(state.records[0].state == BoardInfoState::TimedOut && state.remotePending,
              "unacknowledged remote stop is explicit");
        connection = std::make_shared<boardfixture::Connection>();
        connection->mode = boardfixture::Mode::Hold;
        request = boardfixture::request(connection);
        BoardInfoService changed;
        check(changed.prepare(request, false, error), "generation fixture prepared");
        std::thread old([&] { changed.executePrepared(); });
        waitStart(connection);
        *request.session.alive = false;
        old.join();
        check(connection->starts().size() == 1 &&
                  changed.snapshot().records[0].state == BoardInfoState::Cancelled &&
                  changed.snapshot().serial == "device-A",
              "generation change never starts next item or retargets request");
        auto primary = std::make_shared<boardfixture::Connection>();
        auto independent = std::make_shared<boardfixture::Connection>();
        request = boardfixture::request(primary);
        request.session.openConnection = [independent](std::string&) { return independent; };
        BoardInfoService isolated;
        check(isolated.prepare(request, false, error), "independent connection prepared");
        isolated.executePrepared();
        check(primary->calls == 0 && !primary->closed && independent->calls > 0 &&
                  independent->closed,
              "owned reading connection closes without touching primary session");
        request = boardfixture::request(primary);
        request.session.openConnection =
            [](std::string& error) -> std::shared_ptr<IAgentConnection> {
            error = "fixture open failure";
            return {};
        };
        check(isolated.prepare(request, false, error), "open failure fixture prepared");
        isolated.executePrepared();
        check(primary->calls == 0 && isolated.snapshot().records[0].error.find(
                                         "fixture open failure") != std::string::npos,
              "failed read connection never falls back to shared primary socket");
        std::cout
            << "PASS board information order, capabilities, raw units, unknown fields, "
               "cancellation, timeout, correlation, session changes and remote-stop uncertainty\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
