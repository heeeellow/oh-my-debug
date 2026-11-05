#include "application/device/device_maintenance_service.h"
#include <atomic>
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
class Backend final : public IDeviceMaintenanceBackend {
  public:
    std::atomic<int> calls{0};
    std::atomic<bool> entered{false};
    bool hold = false, fail = false, loader = false;
    MaintenanceRequest captured;
    MaintenanceResult answer{MaintenanceOutcome::Completed, "fixture complete"};
    std::atomic<bool> probeHold{false}, probeEntered{false};
    LoaderSupport inspectLoaderSupport(const AdbDevice&, const std::atomic<bool>& cancel) override
    {
        probeEntered = true;
        while (probeHold && !cancel) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return {loader ? LoaderSupportState::Supported : LoaderSupportState::Unsupported, "fixture BSP evidence", {}};
    }
    MaintenanceResult
    execute(const MaintenanceRequest& request,
            const std::atomic<bool>& cancel,
            const std::function<void(const MaintenanceProgress&)>& publish) override
    {
        ++calls;
        captured = request;
        publish({MaintenanceStage::Exporting, "fixture", 42, request.output, false});
        entered = true;
        if (fail)
            throw std::runtime_error("fixture backend failure");
        while (hold && !cancel)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (cancel)
            return {MaintenanceOutcome::Cancelled, "fixture cancelled"};
        return answer;
    }
};
MaintenanceRequest request(MaintenanceAction action = MaintenanceAction::Dmesg)
{
    MaintenanceRequest value;
    value.device = {"device-A", "device", "fixture"};
    value.action = action;
    value.output = std::filesystem::current_path() / "fixture.log";
    return value;
}
} // namespace
int main()
{
    try {
        auto backend = std::make_shared<Backend>();
        DeviceMaintenanceService service(backend);
        std::string error;
        auto invalid = request();
        invalid.device.state = "unauthorized";
        check(!service.prepare(invalid, {}, error), "offline device rejected");
        invalid = request();
        invalid.device.serial = "bad\nserial";
        check(!service.prepare(invalid, {}, error), "control bytes rejected");
        invalid = request();
        invalid.output = "relative.log";
        check(!service.prepare(invalid, {}, error), "relative export path rejected without I/O");
        invalid = request();
        invalid.action = MaintenanceAction(99);
        check(!service.prepare(invalid, {}, error), "unknown action rejected");
        invalid = request();
        invalid.commandTimeout = std::chrono::milliseconds(0);
        check(!service.prepare(invalid, {}, error), "unbounded timeout rejected");
        check(!service.prepare(request(MaintenanceAction::Loader), {}, error),
              "unconfirmed Loader rejected");
        const std::string proof = "OMD_LOADER_V1\nPLATFORM\nrockchip,rk3568\nENDPLATFORM\nNODE\nsyscon-reboot-mode\nMAGIC\n 52 42 c3 01\nENDNODE\nEND\n";
        check(parseLoaderProbe(proof).state == LoaderSupportState::Supported, "bound BSP loader magic accepted");
        check(parseLoaderProbe(proof.substr(0, proof.size()-5)).state == LoaderSupportState::Unknown, "incomplete probe rejected");
        auto wrongMagic = proof; wrongMagic.replace(wrongMagic.find("c3 01"), 5, "c3 09");
        check(parseLoaderProbe(wrongMagic).state == LoaderSupportState::Unsupported, "fastboot magic is not Loader");
        check(parseLoaderProbe("OMD_LOADER_V1\nPLATFORM\nrockchip,rk3568\nENDPLATFORM\nEND\n").state == LoaderSupportState::Unsupported, "platform name alone insufficient");
        check(parseLoaderProbe(proof + "noise").state == LoaderSupportState::Unknown, "extra data rejected");
        check(parseLoaderProbe(std::string(17000, 'a')).state == LoaderSupportState::Unknown, "oversize probe rejected");
        service.setLoaderTarget(request().device);
        backend->loader = true; backend->probeHold = true;
        auto probe = service.prepareLoaderProbe(); check(probe.id != 0, "probe prepared without I/O");
        std::thread probeWorker([&] { service.executeLoaderProbe(probe); });
        while (!backend->probeEntered) std::this_thread::yield();
        check(service.loaderSupport(request().device).state == LoaderSupportState::Checking, "nonblocking checking state");
        auto other = request().device; other.serial = "device-B"; service.setLoaderTarget(other); service.setLoaderTarget(request().device);
        probeWorker.join();
        check(!service.loaderSupported(request().device), "stale A-B-A probe cannot qualify current target");
        backend->probeHold = false;
        probe = service.prepareLoaderProbe(); service.executeLoaderProbe(probe);
        check(service.loaderSupported(request().device), "current probe enables Loader");
        service.setLoaderTarget(other);
        check(!service.loaderSupported(request().device), "support cannot cross devices");
        service.setLoaderTarget(request().device);
        probe = service.prepareLoaderProbe(); service.cancelLoaderProbe(); service.executeLoaderProbe(probe);
        check(!service.loaderSupported(request().device), "cancelled probe cannot enable Loader");
        for (int field = 0; field < 5; ++field) {
            MaintenanceConflicts conflict;
            if (field == 0)
                conflict.files = true;
            if (field == 1)
                conflict.serial = true;
            if (field == 2)
                conflict.board = true;
            if (field == 3)
                conflict.mirror = true;
            if (field == 4)
                conflict.preparingService = true;
            check(!service.prepare(request(MaintenanceAction::RootRemount), conflict, error),
                  "same-device task blocks disruptive action");
        }
        auto value = request(MaintenanceAction::Logcat);
        check(service.prepare(value, {true, true, true, true, true}, error),
              "read-only export independent of maintenance conflicts");
        const auto firstId = service.snapshot().id;
        value.device.serial = "device-B";
        check(service.snapshot().request.device.serial == "device-A",
              "prepared target is immutable");
        check(!service.prepare(value, {}, error), "single active task gate");
        service.executePrepared();
        check(backend->captured.device.serial == "device-A" && backend->calls == 1,
              "only original target executes");
        check(service.snapshot().result.bytes == 42 && !service.snapshot().active,
              "progress retained in final snapshot");
        service.executePrepared();
        check(backend->calls == 1, "finished task cannot execute twice");

        backend->hold = true;
        backend->entered = false;
        check(service.prepare(request(), {}, error), "next task starts");
        const auto id = service.snapshot().id;
        service.cancel(firstId);
        check(!service.snapshot().cancelling, "stale stop does not cancel newer task");
        std::thread worker([&] { service.executePrepared(); });
        while (!backend->entered)
            std::this_thread::yield();
        service.executePrepared();
        check(backend->calls == 2, "duplicate worker cannot execute same task");
        service.cancel(id);
        check(service.snapshot().cancelling, "cancel remains active until worker settles");
        worker.join();
        check(service.snapshot().result.outcome == MaintenanceOutcome::Cancelled &&
                  !service.snapshot().active,
              "cancelled task settles");
        backend->hold = false;
        check(service.prepare(request(MaintenanceAction::Reboot), {}, error),
              "local COM is not a device maintenance conflict");
        service.cancel();
        service.executePrepared();
        check(backend->calls == 2, "cancel before execution performs no backend action");
        backend->answer = {MaintenanceOutcome::Partial, "root only"};
        backend->answer.rootVerified = true;
        check(service.prepare(request(MaintenanceAction::RootRemount), {}, error),
              "root task prepared");
        service.executePrepared();
        check(service.snapshot().result.rootVerified &&
                  !service.snapshot().result.remountAccepted &&
                  service.snapshot().result.outcome == MaintenanceOutcome::Partial,
              "root success is separate from remount result");
        backend->fail = true;
        check(service.prepare(request(), {}, error), "failure fixture prepared");
        service.executePrepared();
        check(service.snapshot().result.outcome == MaintenanceOutcome::Failed &&
                  service.snapshot().result.bytes == 42 &&
                  !service.snapshot().result.artifact.empty(),
              "exception keeps artifact and partial progress");
        std::cout << "PASS maintenance targets, validation, task conflicts, fixed IDs, "
                     "cancellation, partial results and exception boundaries\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
