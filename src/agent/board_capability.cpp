#include "board_capability.h"

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace testtools::agent {
namespace {

constexpr char MemoryCapacityCapability[] = "memory.capacity";
constexpr char StorageInfoCapability[] = "storage.device.info";
constexpr char StorageHealthCapability[] = "storage.device.health";

BoardCapabilityDefinition memoryDefinition()
{
    return {MemoryCapacityCapability,
            "Memory capacity",
            1,
            {"agent_native", "linux_proc_meminfo", 1},
            {"read_only"},
            {"memory.inventory"},
            "memory.capacity/v1",
            "none",
            2000,
            true};
}

BoardCapabilityDefinition storageInfoDefinition()
{
    return {StorageInfoCapability,
            "Storage device information",
            1,
            {"agent_native", "linux_sysfs_storage", 1},
            {"read_only"},
            {"storage.inventory"},
            "storage.device.info/v1",
            "none",
            3000,
            true};
}

BoardCapabilityDefinition storageHealthDefinition()
{
    return {StorageHealthCapability,
            "Storage device health",
            1,
            {"agent_native", "linux_sysfs_storage", 1},
            {"read_only"},
            {"storage.inventory"},
            "storage.device.health/v1",
            "none",
            3000,
            true};
}

JsonValue optionalString(const std::optional<std::string>& value)
{
    return value ? JsonValue::string(*value) : JsonValue::null();
}

JsonValue optionalInteger(const std::optional<std::uint64_t>& value)
{
    if (!value || *value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return JsonValue::null();
    }
    return JsonValue::integer(static_cast<std::int64_t>(*value));
}

JsonValue optionalInteger(const std::optional<std::uint32_t>& value)
{
    return value ? JsonValue::integer(*value) : JsonValue::null();
}

std::string storageKindName(StorageDeviceKind kind)
{
    return kind == StorageDeviceKind::eMMC ? "emmc" : "ufs";
}

JsonValue storageIdentity(const StorageDeviceObservation& device)
{
    return JsonValue::object({
        {"capacity_bytes", optionalInteger(device.capacityBytes)},
        {"device_key", JsonValue::string(storageKindName(device.kind) + ":" + device.name)},
        {"firmware", optionalString(device.firmware)},
        {"kernel_name", JsonValue::string(device.name)},
        {"manufacturer", optionalString(device.manufacturer)},
        {"model", optionalString(device.model)},
        {"serial", optionalString(device.serial)},
        {"type", JsonValue::string(storageKindName(device.kind))},
    });
}

JsonValue storageHealth(const StorageDeviceObservation& device)
{
    return JsonValue::object({
        {"device_key", JsonValue::string(storageKindName(device.kind) + ":" + device.name)},
        {"discard_bytes", optionalInteger(device.io.discardBytes)},
        {"life_time_a", optionalInteger(device.health.lifeTimeEstimationA)},
        {"life_time_b", optionalInteger(device.health.lifeTimeEstimationB)},
        {"pre_eol", optionalInteger(device.health.preEolInfo)},
        {"read_bytes", optionalInteger(device.io.readBytes)},
        {"type", JsonValue::string(storageKindName(device.kind))},
        {"write_bytes", optionalInteger(device.io.writeBytes)},
    });
}

BoardTestRunResult baseResult(const BoardCapabilityDefinition& definition)
{
    BoardTestRunResult result;
    result.provider = definition.provider;
    result.observationSchema = definition.observationSchema;
    result.exitCode = -1;
    return result;
}

BoardTestRunResult cancelledResult(const BoardCapabilityDefinition& definition)
{
    auto result = baseResult(definition);
    result.cancelled = true;
    result.exitCode = 130;
    result.summary = "read-only observation cancelled";
    return result;
}

bool memoryAvailable(const NativeObservationRoots& roots)
{
    return readNativeMemoryCapacity(roots).memTotalKiB.has_value();
}

bool storageAvailable(const NativeObservationRoots& roots)
{
    return !enumerateNativeStorage(roots).empty();
}

BoardTestRunResult runMemoryCapacity(const BoardCapabilityDefinition& definition,
                                     const std::atomic_bool& cancelRequested,
                                     const NativeObservationRoots& roots)
{
    if (cancelRequested.load())
        return cancelledResult(definition);
    auto result = baseResult(definition);
    const auto memory = readNativeMemoryCapacity(roots);
    if (!memory.memTotalKiB ||
        *memory.memTotalKiB >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        result.summary = "Linux MemTotal is unavailable";
        return result;
    }
    result.observations = JsonValue::object({
        {"memtotal_kib", JsonValue::integer(static_cast<std::int64_t>(*memory.memTotalKiB))},
    });
    result.summary = "Observed Linux MemTotal";
    if (cancelRequested.load())
        return cancelledResult(definition);
    result.succeeded = true;
    result.exitCode = 0;
    return result;
}

BoardTestRunResult runStorage(const BoardCapabilityDefinition& definition,
                              const std::atomic_bool& cancelRequested,
                              const NativeObservationRoots& roots)
{
    if (cancelRequested.load())
        return cancelledResult(definition);
    auto result = baseResult(definition);
    const auto devices = enumerateNativeStorage(roots);
    if (devices.empty()) {
        result.summary = "No supported non-removable eMMC or UFS device was found";
        return result;
    }
    const bool information = definition.id == StorageInfoCapability;
    JsonValue::Array values;
    values.reserve(devices.size());
    for (const auto& device : devices) {
        values.push_back(information ? storageIdentity(device) : storageHealth(device));
    }
    result.observations = JsonValue::object({{"devices", JsonValue::array(std::move(values))}});
    result.summary = information ? "Observed eMMC/UFS device information"
                                 : "Observed eMMC/UFS health information";
    if (cancelRequested.load())
        return cancelledResult(definition);
    result.succeeded = true;
    result.exitCode = 0;
    return result;
}

using ProbeFunction = bool (*)(const NativeObservationRoots&);
using RunFunction = BoardTestRunResult (*)(const BoardCapabilityDefinition&,
                                           const std::atomic_bool&,
                                           const NativeObservationRoots&);

struct ProviderBinding {
    BoardCapabilityDefinition definition;
    ProbeFunction probe = nullptr;
    RunFunction run = nullptr;
};

const std::array<ProviderBinding, 3>& providerBindings()
{
    static const std::array<ProviderBinding, 3> bindings{{
        {memoryDefinition(), memoryAvailable, runMemoryCapacity},
        {storageInfoDefinition(), storageAvailable, runStorage},
        {storageHealthDefinition(), storageAvailable, runStorage},
    }};
    return bindings;
}

} // namespace

std::vector<BoardCapabilityDefinition>
availableBoardCapabilities(const NativeObservationRoots& roots)
{
    std::vector<BoardCapabilityDefinition> definitions;
    for (const auto& binding : providerBindings()) {
        if (binding.probe && binding.probe(roots))
            definitions.push_back(binding.definition);
    }
    return definitions;
}

BoardTestRunResult runBoardCapability(const BoardTestRunRequest& request,
                                      const std::atomic_bool& cancelRequested,
                                      const NativeObservationRoots& roots)
{
    const ProviderBinding* selected = nullptr;
    for (const auto& binding : providerBindings()) {
        if (binding.definition.id == request.capabilityId &&
            binding.definition.version == request.capabilityVersion && binding.probe &&
            binding.probe(roots)) {
            selected = &binding;
            break;
        }
    }
    if (!selected || !selected->run) {
        BoardTestRunResult result;
        result.summary = "capability is unavailable";
        return result;
    }
    return selected->run(selected->definition, cancelRequested, roots);
}

} // namespace testtools::agent
