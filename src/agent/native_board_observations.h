#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace testtools::agent {

// 挂载点根目录支持注入，以便在宿主测试夹具中验证采集逻辑；生产环境默认指向 Linux /proc 与 /sys。
struct NativeObservationRoots {
    std::filesystem::path procRoot = "/proc";
    std::filesystem::path sysRoot = "/sys";
};

struct MemoryCapacityObservation {
    // Linux /proc/meminfo 报告的 MemTotal（单位 KiB）；缺失或格式错误时记为 nullopt，绝不转换为 0。
    std::optional<std::uint64_t> memTotalKiB;
};

enum class StorageDeviceKind {
    eMMC,
    UFS,
};

struct StorageHealthObservation {
    // 内核描述符暴露的原始数值；由于健康度描述符与内核/器件实现相关，各字段均为可选项。
    std::optional<std::uint32_t> preEolInfo;
    std::optional<std::uint32_t> lifeTimeEstimationA;
    std::optional<std::uint32_t> lifeTimeEstimationB;
};

struct StorageIoObservation {
    // 源自 /sys/block/<disk>/stat 的累计读写/丢弃字节观测值，仅作统计观测而非性能基准结论。
    std::optional<std::uint64_t> readBytes;
    std::optional<std::uint64_t> writeBytes;
    std::optional<std::uint64_t> discardBytes;
};

struct StorageDeviceObservation {
    StorageDeviceKind kind = StorageDeviceKind::eMMC;
    std::string name;       // 例如 mmcblk0 或 sda
    std::string deviceNode; // /dev/<name>，仅按名称推导而不直接打开设备节点
    std::optional<std::uint64_t> capacityBytes;

    // 器件身份字段为可选项，因为不同 eMMC 版本、UFS 实现与内核版本的 sysfs 属性可用性存在差异。
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

    // 错误列表仅记录不可用的挂载根目录或不可读的顶层目录；单个设备缺失可选属性文件不视为错误。
    std::vector<std::string> errors;
};

MemoryCapacityObservation readNativeMemoryCapacity(const NativeObservationRoots& roots = {});

std::vector<StorageDeviceObservation>
enumerateNativeStorage(const NativeObservationRoots& roots = {},
                       std::vector<std::string>* errors = nullptr);

NativeBoardObservations collectNativeBoardObservations(const NativeObservationRoots& roots = {});

} // namespace testtools::agent
