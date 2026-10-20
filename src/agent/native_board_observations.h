#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace testtools::agent {

// Roots are injectable so the collection logic can be exercised against a
// host fixture.  They normally point at the Linux proc and sys mount points.
struct NativeObservationRoots {
    std::filesystem::path procRoot = "/proc";
    std::filesystem::path sysRoot = "/sys";
};

struct MemoryCapacityObservation {
    // MemTotal as reported by Linux, in KiB.  A missing or malformed value is
    // represented by nullopt and is never converted to zero.
    std::optional<std::uint64_t> memTotalKiB;
};

enum class StorageDeviceKind {
    eMMC,
    UFS,
};

struct StorageHealthObservation {
    // Values are the raw numeric values exposed by the kernel descriptor.
    // They are optional because health descriptors are kernel/device specific.
    std::optional<std::uint32_t> preEolInfo;
    std::optional<std::uint32_t> lifeTimeEstimationA;
    std::optional<std::uint32_t> lifeTimeEstimationB;
};

struct StorageIoObservation {
    // These are derived from /sys/block/<disk>/stat.  They are observations,
    // not a performance verdict and not a benchmark result.
    std::optional<std::uint64_t> readBytes;
    std::optional<std::uint64_t> writeBytes;
    std::optional<std::uint64_t> discardBytes;
};

struct StorageDeviceObservation {
    StorageDeviceKind kind = StorageDeviceKind::eMMC;
    std::string name;       // e.g. mmcblk0 or sda
    std::string deviceNode; // /dev/<name>, derived without touching the node
    std::optional<std::uint64_t> capacityBytes;

    // Identity fields are optional because their sysfs names and availability
    // differ between eMMC revisions, UFS implementations and kernel versions.
    std::optional<std::string> manufacturer;
    std::optional<std::string> model;
    std::optional<std::string> firmware;
    std::optional<std::string> serial;

    StorageHealthObservation health;
    StorageIoObservation io;
};

struct NativeBoardObservations {
    MemoryCapacityObservation memory;
    std::vector<StorageDeviceObservation> storage;

    // Errors describe unavailable roots or unreadable top-level directories.
    // Missing optional per-device files are intentionally not errors.
    std::vector<std::string> errors;
};

MemoryCapacityObservation readNativeMemoryCapacity(const NativeObservationRoots& roots = {});

std::vector<StorageDeviceObservation>
enumerateNativeStorage(const NativeObservationRoots& roots = {},
                       std::vector<std::string>* errors = nullptr);

NativeBoardObservations collectNativeBoardObservations(const NativeObservationRoots& roots = {});

} // namespace testtools::agent
