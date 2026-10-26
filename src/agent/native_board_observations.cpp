#include "native_board_observations.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <fstream>
#include <limits>
#include <system_error>

namespace testtools::agent {
namespace {

using Path = std::filesystem::path;
constexpr std::size_t MaxObservationFileBytes = 64U * 1024U;

std::string trim(std::string value)
{
    const auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](char c) {
                    return !isSpace(static_cast<unsigned char>(c));
                }));
    value.erase(std::find_if(value.rbegin(),
                             value.rend(),
                             [&](char c) { return !isSpace(static_cast<unsigned char>(c)); })
                    .base(),
                value.end());
    return value;
}

std::optional<std::string> readText(const Path& path)
{
    std::ifstream input(path, std::ios::in | std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    std::string value;
    value.resize(MaxObservationFileBytes + 1U);
    input.read(value.data(), static_cast<std::streamsize>(value.size()));
    const auto count = input.gcount();
    if (input.bad() || count <= 0 ||
        count > static_cast<std::streamsize>(MaxObservationFileBytes)) {
        return std::nullopt;
    }
    value.resize(static_cast<std::size_t>(count));
    if (!input.eof()) {
        return std::nullopt;
    }
    value = trim(std::move(value));
    return value;
}

std::optional<std::uint64_t> parseUnsigned(std::string value)
{
    value = trim(std::move(value));
    if (value.empty()) {
        return std::nullopt;
    }
    int base = 10;
    if (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
        base = 16;
        value.erase(0, 2);
        if (value.empty()) {
            return std::nullopt;
        }
    }
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result, base);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    return result;
}

std::optional<std::uint32_t> readHexNumeric(const std::string& value)
{
    std::string token = trim(value);
    if (token.size() > 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X')) {
        token.erase(0, 2);
    }
    if (token.empty()) {
        return std::nullopt;
    }
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), result, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() ||
        result > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(result);
}

std::optional<std::uint32_t> readNumeric(const Path& path)
{
    const auto text = readText(path);
    if (!text) {
        return std::nullopt;
    }
    const auto value = parseUnsigned(*text);
    if (!value || *value > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(*value);
}

std::optional<std::string> readOptionalText(const Path& path)
{
    const auto text = readText(path);
    if (!text || text->empty()) {
        return std::nullopt;
    }
    return text;
}

bool isDecimalDigits(const std::string& value, std::size_t offset)
{
    if (offset >= value.size()) {
        return false;
    }
    return std::all_of(value.begin() + static_cast<std::ptrdiff_t>(offset),
                       value.end(),
                       [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
}

bool isWholeEmmcName(const std::string& name)
{
    // 严格匹配 mmcblk<数字>，从而排除 mmcblk0p1 分区、mmcblk0boot0 和 mmcblk0rpmb。
    return name.rfind("mmcblk", 0) == 0 && isDecimalDigits(name, 6);
}

bool isWholeUfsLunName(const std::string& name)
{
    // Linux 将 UFS 用户 LUN 呈现为 sd[a-z]+；仅允许纯字母后缀以排除 sda1 分区或 sda_boot0 等特殊节点。
    if (name.size() < 3 || name.rfind("sd", 0) != 0) {
        return false;
    }
    return std::all_of(name.begin() + 2, name.end(), [](char c) { return c >= 'a' && c <= 'z'; });
}

bool isExplicitlyExcludedName(const std::string& name)
{
    return name.rfind("dm-", 0) == 0 || name.rfind("loop", 0) == 0 || name.rfind("ram", 0) == 0 ||
           name.rfind("zram", 0) == 0 || name.find("boot") != std::string::npos ||
           name.find("rpmb") != std::string::npos;
}

std::string lowerCopy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return value;
}

bool hasUfsDeviceAncestry(const Path& devicePath)
{
    const auto type = readOptionalText(devicePath / "type");
    if (type && lowerCopy(*type).find("ufs") != std::string::npos) {
        return true;
    }
    std::error_code error;
    const Path resolved = std::filesystem::weakly_canonical(devicePath, error);
    const std::string text = lowerCopy((error ? devicePath : resolved).generic_string());
    return text.find("ufs") != std::string::npos || text.find("ufshc") != std::string::npos;
}

bool isNonRemovable(const Path& blockPath)
{
    const auto removable = readNumeric(blockPath / "removable");
    return removable && *removable == 0;
}

std::optional<std::uint64_t> readCapacityBytes(const Path& blockPath)
{
    const auto text = readText(blockPath / "size");
    const auto sectors = text ? parseUnsigned(*text) : std::nullopt;
    if (!sectors || *sectors == 0 ||
        *sectors > std::numeric_limits<std::uint64_t>::max() / std::uint64_t{512}) {
        return std::nullopt;
    }
    return *sectors * std::uint64_t{512};
}

std::optional<Path> findUfsDescriptorRoot(const Path& devicePath)
{
    std::error_code error;
    Path current = std::filesystem::weakly_canonical(devicePath, error);
    if (error) {
        current = devicePath;
    }
    for (int depth = 0; depth < 16 && !current.empty(); ++depth) {
        if (std::filesystem::exists(current / "health_descriptor", error) ||
            std::filesystem::exists(current / "device_descriptor", error)) {
            return current;
        }
        const Path parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
    }
    return std::nullopt;
}

void readEmmcIdentity(StorageDeviceObservation& observation, const Path& devicePath)
{
    observation.manufacturer = readOptionalText(devicePath / "manfid");
    observation.model = readOptionalText(devicePath / "name");
    observation.firmware = readOptionalText(devicePath / "fwrev");
    observation.serial = readOptionalText(devicePath / "serial");
}

void readEmmcHealth(StorageDeviceObservation& observation, const Path& devicePath)
{
    observation.health.preEolInfo = readNumeric(devicePath / "pre_eol_info");
    const auto lifetime = readText(devicePath / "life_time");
    if (!lifetime) {
        return;
    }
    std::string first;
    std::string second;
    const auto separator = lifetime->find_first_of(" \t");
    if (separator == std::string::npos) {
        return;
    }
    first = lifetime->substr(0, separator);
    second = trim(lifetime->substr(separator));
    const std::size_t secondEnd = second.find_first_of(" \t");
    if (secondEnd != std::string::npos) {
        second.resize(secondEnd);
    }
    // eMMC life_time 通常为两个十六进制字节（带或不带 0x 前缀）；独立解析两个字段并保留 0 值。
    observation.health.lifeTimeEstimationA = readHexNumeric(first);
    observation.health.lifeTimeEstimationB = readHexNumeric(second);
}

void readUfsDescriptors(StorageDeviceObservation& observation, const Path& devicePath)
{
    const auto root = findUfsDescriptorRoot(devicePath);
    if (!root) {
        return;
    }
    const Path identity = *root / "device_descriptor";
    observation.manufacturer = readOptionalText(identity / "manufacturer_id");
    if (!observation.manufacturer) {
        observation.manufacturer = readOptionalText(identity / "manufacturer_name");
    }
    observation.model = readOptionalText(identity / "product_name");
    observation.firmware = readOptionalText(identity / "product_revision");
    if (!observation.firmware) {
        observation.firmware = readOptionalText(identity / "firmware_version");
    }
    observation.serial = readOptionalText(identity / "serial_number");

    const Path health = *root / "health_descriptor";
    observation.health.preEolInfo = readNumeric(health / "pre_eol_info");
    if (!observation.health.preEolInfo) {
        observation.health.preEolInfo = readNumeric(health / "eol_info");
    }
    observation.health.lifeTimeEstimationA = readNumeric(health / "life_time_estimation_a");
    observation.health.lifeTimeEstimationB = readNumeric(health / "life_time_estimation_b");
}

void readIoStats(StorageDeviceObservation& observation, const Path& blockPath)
{
    const auto text = readText(blockPath / "stat");
    if (!text) {
        return;
    }
    std::vector<std::uint64_t> fields;
    std::size_t cursor = 0;
    while (cursor < text->size() && fields.size() < 20) {
        while (cursor < text->size() && std::isspace(static_cast<unsigned char>((*text)[cursor]))) {
            ++cursor;
        }
        if (cursor == text->size()) {
            break;
        }
        const std::size_t end = text->find_first_of(" \t\r\n", cursor);
        const std::string token =
            text->substr(cursor, end == std::string::npos ? end : end - cursor);
        const auto value = parseUnsigned(token);
        if (!value) {
            break;
        }
        fields.push_back(*value);
        cursor = end == std::string::npos ? text->size() : end;
    }
    if (fields.size() >= 7) {
        if (fields[2] <= std::numeric_limits<std::uint64_t>::max() / 512) {
            observation.io.readBytes = fields[2] * 512;
        }
        if (fields[6] <= std::numeric_limits<std::uint64_t>::max() / 512) {
            observation.io.writeBytes = fields[6] * 512;
        }
    }
    if (fields.size() >= 13 && fields[12] <= std::numeric_limits<std::uint64_t>::max() / 512) {
        observation.io.discardBytes = fields[12] * 512;
    }
}

} // namespace

MemoryCapacityObservation readNativeMemoryCapacity(const NativeObservationRoots& roots)
{
    MemoryCapacityObservation observation;
    const auto meminfo = readText(roots.procRoot / "meminfo");
    if (!meminfo) {
        return observation;
    }
    std::size_t lineStart = 0;
    while (lineStart <= meminfo->size()) {
        const std::size_t lineEnd = meminfo->find('\n', lineStart);
        const std::string line = meminfo->substr(
            lineStart, lineEnd == std::string::npos ? lineEnd : lineEnd - lineStart);
        if (line.rfind("MemTotal", 0) == 0) {
            const std::size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string fields = trim(line.substr(colon + 1));
                const std::size_t separator = fields.find_first_of(" \t");
                const std::string number = fields.substr(0, separator);
                const std::string unit =
                    separator == std::string::npos ? std::string{} : trim(fields.substr(separator));
                const auto value = parseUnsigned(number);
                if (value &&
                    (unit.empty() || lowerCopy(unit) == "kb" || lowerCopy(unit) == "kib")) {
                    observation.memTotalKiB = *value;
                }
            }
            break;
        }
        if (lineEnd == std::string::npos) {
            break;
        }
        lineStart = lineEnd + 1;
    }
    return observation;
}

std::vector<StorageDeviceObservation> enumerateNativeStorage(const NativeObservationRoots& roots,
                                                             std::vector<std::string>* errors)
{
    std::vector<StorageDeviceObservation> observations;
    const Path blockRoot = roots.sysRoot / "block";
    std::error_code error;
    if (!std::filesystem::is_directory(blockRoot, error)) {
        if (errors) {
            errors->push_back("sysfs block root is unavailable: " + blockRoot.generic_string());
        }
        return observations;
    }

    std::filesystem::directory_iterator iterator(blockRoot, error);
    const std::filesystem::directory_iterator end;
    for (; !error && iterator != end; iterator.increment(error)) {
        const auto& entry = *iterator;
        const std::string name = entry.path().filename().string();
        if (isExplicitlyExcludedName(name)) {
            continue;
        }
        const bool emmc = isWholeEmmcName(name);
        const bool ufs = isWholeUfsLunName(name) && hasUfsDeviceAncestry(entry.path() / "device");
        if ((!emmc && !ufs) || !isNonRemovable(entry.path())) {
            continue;
        }
        const auto capacity = readCapacityBytes(entry.path());
        // UFS boot/RPMB LUN 也可能暴露为 sd* 节点；按瑞芯微参考分区布局，将小于 1 GiB 的 LUN 视为辅助 LUN 排除。
        if (!capacity || (ufs && *capacity < std::uint64_t{1} * 1024 * 1024 * 1024)) {
            continue;
        }

        StorageDeviceObservation observation;
        observation.kind = emmc ? StorageDeviceKind::eMMC : StorageDeviceKind::UFS;
        observation.name = name;
        observation.deviceNode = "/dev/" + name;
        observation.capacityBytes = capacity;
        const Path devicePath = entry.path() / "device";
        if (emmc) {
            readEmmcIdentity(observation, devicePath);
            readEmmcHealth(observation, devicePath);
        } else {
            readUfsDescriptors(observation, devicePath);
        }
        readIoStats(observation, entry.path());
        observations.push_back(std::move(observation));
    }
    if (error && errors) {
        errors->push_back("sysfs block enumeration failed: " + error.message());
    }
    std::sort(observations.begin(), observations.end(), [](const auto& left, const auto& right) {
        return left.name < right.name;
    });
    return observations;
}

NativeBoardObservations collectNativeBoardObservations(const NativeObservationRoots& roots)
{
    NativeBoardObservations observations;
    observations.memory = readNativeMemoryCapacity(roots);
    observations.storage = enumerateNativeStorage(roots, &observations.errors);
    return observations;
}

} // namespace testtools::agent
