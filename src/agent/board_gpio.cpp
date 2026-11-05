#include "board_gpio.h"
#include "toolbox.h"
#include "core/toolbox/toolbox_contract.h"
#include <algorithm>
#include <array>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#if defined(__linux__)
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace testtools::agent {
namespace fs = std::filesystem;
namespace {
#if defined(__linux__)
struct Descriptor {
    int value;
    explicit Descriptor(int fd) : value(fd) {}
    ~Descriptor() { if (value >= 0) ::close(value); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
};
std::runtime_error ioError(const std::string& action) { return std::runtime_error(action + ": " + std::strerror(errno)); }
bool inside(const fs::path& path, const fs::path& root)
{
    auto a = path.begin(), b = root.begin();
    for (; b != root.end(); ++a, ++b) if (a == path.end() || *a != *b) return false;
    return true;
}
std::string fileIdentity(const fs::path& path)
{
    struct stat s{};
    if (::lstat(path.c_str(), &s) != 0 || (!S_ISREG(s.st_mode) && !S_ISDIR(s.st_mode))) return {};
    return std::to_string(s.st_dev) + ":" + std::to_string(s.st_ino);
}
std::string readBounded(const fs::path& path, std::size_t limit = 4096)
{
    Descriptor fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    struct stat s{};
    if (fd.value < 0 || ::fstat(fd.value, &s) != 0 || !S_ISREG(s.st_mode)) return {};
    std::string value(limit + 1, '\0');
    const auto n = ::read(fd.value, value.data(), value.size());
    if (n < 0 || static_cast<std::size_t>(n) > limit) return {};
    value.resize(static_cast<std::size_t>(n)); return value;
}
std::vector<fs::path> directories(const fs::path& parent, std::size_t limit)
{
    std::error_code ec; std::vector<fs::path> result;
    if (!fs::is_directory(parent, ec)) return result;
    for (const auto& item : fs::directory_iterator(parent, ec)) {
        if (ec) throw std::runtime_error("board GPIO directory is inaccessible");
        if (result.size() >= limit) throw std::runtime_error("board GPIO discovery limit exceeded");
        result.push_back(item.path());
    }
    if (ec) throw std::runtime_error("board GPIO directory is inaccessible");
    std::sort(result.begin(), result.end()); return result;
}
std::string dtString(const fs::path& path)
{
    auto value = readBounded(path, 256);
    if (!value.empty() && value.back() == '\0') value.pop_back();
    if (value.find('\0') != std::string::npos) return {};
    return value;
}
bool compatible(const fs::path& path, const std::string& expected)
{
    const auto value = readBounded(path);
    return (std::string(1, '\0') + value).find(std::string(1, '\0') + expected + '\0') != std::string::npos;
}
std::uint32_t cell(const std::string& value, std::size_t offset)
{
    std::uint32_t result = 0;
    for (unsigned i = 0; i < 4; ++i) result = (result << 8) | static_cast<unsigned char>(value.at(offset + i));
    return result;
}
bool ordinaryName(const std::string& label)
{
    return label.size() > 4 && label.size() <= 7 && label.rfind("gpio", 0) == 0 && label[4] >= '1' && label[4] <= '9' &&
        std::all_of(label.begin() + 4, label.end(), [](char c) { return c >= '0' && c <= '9'; });
}
bool safeName(const std::string& name)
{
    return !name.empty() && name.size() <= 80 && name != "." && name != ".." && std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    });
}
int valueAt(int fd)
{
    if (::lseek(fd, 0, SEEK_SET) < 0) throw ioError("seek board GPIO");
    char data[17]{}; const auto n = ::read(fd, data, sizeof(data));
    if (n < 0) throw ioError("read board GPIO");
    const std::string value(data, n);
    if (value == "0" || value == "0\n") return 0;
    if (value == "1" || value == "1\n") return 1;
    throw std::runtime_error("board GPIO returned an invalid digital value");
}
#endif
}
struct BoardGpioRegistry::Impl {
    explicit Impl(std::string root, AttributeStore store) : root(std::move(root)), store(std::move(store)) {}
    fs::path root;
    AttributeStore store;
    std::mutex mutex;
    struct Endpoint { JsonValue data; fs::path attribute; std::string key, nodeIdentity, parentIdentity; };
    std::map<std::string, Endpoint> entries;
#if defined(__linux__)
    std::vector<Endpoint> discover(const JsonValue& native)
    {
        std::vector<Endpoint> found;
        const auto* chips = native.find("chips"); const auto* pins = native.find("pins");
        const auto platformRoot = fs::weakly_canonical(root / "devices/platform");
        const auto treeRoot = fs::weakly_canonical(root / "firmware/devicetree/base");
        for (const auto& alias : directories(root / "bus/platform/devices", 1024)) {
            std::error_code ec; const auto device = fs::canonical(alias, ec);
            if (ec || !inside(device, platformRoot)) continue;
            const auto driver = fs::canonical(device / "driver", ec);
            if (ec || !inside(driver, fs::weakly_canonical(root / "bus/platform/drivers"))) continue;
            const auto driverName = driver.filename().string();
            const bool direct = driverName == "rk_gpio";
            if (!direct && driverName != "rocktech_gpio" && driverName != "gpio-control") continue;
            const auto dt = fs::canonical(device / "of_node", ec);
            if (ec || !inside(dt, treeRoot)) continue;
            if (!compatible(dt / "compatible", direct ? "rk_gpio" : driverName == "gpio-control" ? "rocktech,gpio-control" : "fsl,rocktech_gpio")) continue;
            std::vector<std::pair<std::string, fs::path>> nodes;
            if (direct) {
                for (int i = 1; i <= 6; ++i) nodes.emplace_back("gpio" + std::to_string(i), dt / ("gpio" + std::to_string(i)));
            } else {
                for (const auto& child : directories(dt, 256)) {
                    if (!fs::is_directory(child, ec)) continue;
                    const auto label = dtString(child / "label");
                    if (safeName(label)) nodes.emplace_back(label, child / "gpios");
                }
            }
            int directIndex = 0;
            for (const auto& node : nodes) {
                const auto label = node.first;
                const auto path = direct ? device / ("gpio" + std::to_string(directIndex++)) : root / "class/rock_gpio" / label / "value";
                const auto propertyParent = fs::canonical(node.second.parent_path(), ec);
                if (ec || !inside(propertyParent, treeRoot)) continue;
                const auto bytes = readBounded(propertyParent / node.second.filename(), 64);
                if (bytes.size() != 12) continue;
                const auto offset = cell(bytes, 4);
                const auto parent = fs::canonical(path.parent_path(), ec);
                if (ec || (direct ? parent != device : parent != root / "devices/virtual/rock_gpio" / label)) continue;
                const auto attribute = parent / path.filename();
                const auto nodeId = fileIdentity(attribute), parentId = fileIdentity(parent);
                if (nodeId.empty() || parentId.empty()) continue;
                JsonValue pin = JsonValue::object(); std::string chipId, controllerIdentity, rockchip, mappingIdentity;
                JsonValue number = JsonValue::null();
                int matches = 0;
                if (chips && chips->isArray()) for (const auto& chip : chips->arrayItems()) {
                    const auto id = jsonString(chip, "id");
                    std::string validationError;
                    if (!validateToolArguments("gpio.pin.read", JsonValue::object({{"chip", JsonValue::string(id)}, {"offset", JsonValue::integer(0)}}), validationError)) continue;
                    const auto chipDevice = fs::canonical(root / "bus/gpio/devices" / id, ec);
                    const auto controller = chipDevice.parent_path();
                    if (ec || !inside(controller, root / "devices")) continue;
                    const auto controllerDt = fs::canonical(controller / "of_node", ec);
                    if (ec || !inside(controllerDt, treeRoot)) continue;
                    const auto phandle = readBounded(controllerDt / "phandle", 4), cells = readBounded(controllerDt / "#gpio-cells", 4);
                    if (phandle.size() != 4 || cells.size() != 4 || cell(cells, 0) != 2 || cell(phandle, 0) != cell(bytes, 0) ||
                        offset >= static_cast<std::uint64_t>(jsonInteger(chip, "lines"))) continue;
                    ++matches; chipId = id; controllerIdentity = jsonString(chip, "label") + "/" + jsonString(chip, "name");
                    mappingIdentity = controller.string() + ":" + fileIdentity(controller) + ":" + fileIdentity(chipDevice) + ":" + fileIdentity(controllerDt);
                    if (const auto* base = chip.find("linux_base"); base && base->isInt64()) number = JsonValue::integer(base->int64Value() + offset);
                    RockchipGpioNumber converted; std::string conversionError;
                    if (convertRockchipGpio(jsonString(chip, "label") + "_" + static_cast<char>('A' + offset / 8) + std::to_string(offset % 8), converted, conversionError)) rockchip = converted.name;
                    if (pins && pins->isArray()) for (const auto& candidate : pins->arrayItems())
                        if (jsonString(candidate, "chip") == id && jsonInteger(candidate, "offset", -1) == offset) pin = candidate;
                }
                const bool mapped = matches == 1 && pin.find("used");
                const bool owned = mapped && jsonBoolean(pin, "used") && jsonString(pin, "consumer") == label;
                const bool readable = ::access(attribute.c_str(), R_OK) == 0;
                const bool writable = readable && owned && ordinaryName(label) && ::access(attribute.c_str(), W_OK) == 0;
                std::string reason = !mapped ? "实际管脚映射未确认" : !owned ? "当前管脚占用者与公司接口不符" :
                    !ordinaryName(label) ? "该用途节点当前仅提供读取" : !writable ? "接口访问权限不足" : "";
                const auto adapter = direct ? "rk-gpio/v1" : "rocktech-gpio/v1";
                Endpoint entry;
                entry.attribute = attribute; entry.nodeIdentity = nodeId; entry.parentIdentity = parentId;
                entry.key = attribute.string() + ":" + nodeId + ":" + parentId + ":" + fileIdentity(device) + ":" +
                    fileIdentity(dt) + ":" + bytes + ":" + adapter + ":" + driver.string() + ":" + fileIdentity(driver) + ":" +
                    readBounded(dt / "compatible") + ":" + (mapped ? chipId + ":" + mappingIdentity : "unmapped");
                entry.data = JsonValue::object({{"source", JsonValue::string("board")}, {"name", JsonValue::string(label)},
                    {"adapter", JsonValue::string(adapter)}, {"path", JsonValue::string(attribute.string())},
                    {"preference_key", JsonValue::string("board:" + fs::relative(attribute, root).generic_string())},
                    {"driver", JsonValue::string(driverName)}, {"chip", JsonValue::string(mapped ? chipId : "")},
                    {"offset", mapped ? JsonValue::integer(offset) : JsonValue::null()}, {"global_number", mapped ? number : JsonValue::null()},
                    {"rockchip_name", JsonValue::string(mapped ? rockchip : "")}, {"controller_identity", JsonValue::string(controllerIdentity)},
                    {"used", JsonValue::boolean(true)}, {"consumer", JsonValue::string(mapped ? jsonString(pin, "consumer") : driverName)},
                    {"direction", JsonValue::string(mapped ? jsonString(pin, "direction") : "unknown")},
                    {"dt_flags", JsonValue::integer(cell(bytes, 8))}, {"value_semantics", JsonValue::string("physical")},
                    {"readable", JsonValue::boolean(readable)}, {"writable", JsonValue::boolean(writable)}, {"reason", JsonValue::string(reason)}});
                found.push_back(std::move(entry));
                if (found.size() > 128) throw std::runtime_error("board GPIO endpoint limit exceeded");
            }
        }
        std::map<std::string, unsigned> counts, physicalCounts;
        for (const auto& entry : found) {
            ++counts[entry.attribute.string()];
            if (!jsonString(entry.data, "chip").empty()) ++physicalCounts[jsonString(entry.data, "chip") + ":" + std::to_string(jsonInteger(entry.data, "offset"))];
        }
        for (auto& entry : found) if (counts[entry.attribute.string()] != 1 ||
            (!jsonString(entry.data, "chip").empty() && physicalCounts[jsonString(entry.data, "chip") + ":" + std::to_string(jsonInteger(entry.data, "offset"))] != 1)) {
            entry.data.objectItems()["writable"] = JsonValue::boolean(false);
            entry.data.objectItems()["reason"] = JsonValue::string("接口关联了多个驱动节点，归属不唯一");
        }
        return found;
    }
#endif
    JsonValue refresh(const JsonValue& native)
    {
        JsonValue::Array endpoints; std::string unavailable;
        std::map<std::string, Endpoint> current;
#if defined(__linux__)
        try {
            root = fs::canonical(root);
            for (auto entry : discover(native)) {
                const auto old = entries.find(entry.key);
                entry.data.objectItems()["endpoint_id"] = old == entries.end() ? JsonValue::string(makeToolToken()) : *old->second.data.find("endpoint_id");
                endpoints.push_back(entry.data); current.emplace(entry.key, std::move(entry));
            }
        } catch (const std::exception& error) { unavailable = error.what(); endpoints.clear(); current.clear(); }
#else
        (void)native; unavailable = "board GPIO interfaces require Linux or Android";
#endif
        entries = std::move(current);
        return JsonValue::object({{"schema", JsonValue::string("gpio.board.inventory/v1")}, {"endpoints", JsonValue::array(std::move(endpoints))},
            {"complete", JsonValue::boolean(unavailable.empty())}, {"unavailable", JsonValue::string(unavailable)}});
    }
};
BoardGpioRegistry::BoardGpioRegistry(std::string root, AttributeStore store) : impl_(std::make_unique<Impl>(std::move(root), std::move(store))) {}
BoardGpioRegistry::~BoardGpioRegistry() = default;
JsonValue BoardGpioRegistry::inventory(const JsonValue& native)
{
    std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->refresh(native);
}
BoardTestRunResult BoardGpioRegistry::run(const BoardTestRunRequest& request, const std::atomic_bool& cancel,
                                        const std::function<JsonValue()>& nativeInventory)
{
    BoardTestRunResult result; result.provider = {"sdk_bsp", "board_gpio", 1}; result.observationSchema = request.capabilityId + "/v1";
    JsonValue values = JsonValue::object({{"write_attempted", JsonValue::boolean(false)}, {"write_completed", JsonValue::boolean(false)},
        {"persistent", JsonValue::boolean(request.capabilityId == "gpio.board.set")}});
    try {
#if defined(__linux__)
        if (!request.toolbox) throw std::runtime_error("missing board GPIO task context");
        const auto& args = request.toolbox->arguments; std::string error;
        if (!validateToolArguments(request.capabilityId, args, error)) throw std::runtime_error(error);
        std::lock_guard<std::mutex> lock(impl_->mutex);
        // 只接受先前发现过的端点；当前身份变化后不会沿用旧 ID。
        const auto id = jsonString(args, "endpoint_id");
        auto previous = std::find_if(impl_->entries.begin(), impl_->entries.end(), [&](const auto& e) { return jsonString(e.second.data, "endpoint_id") == id; });
        if (previous == impl_->entries.end()) throw std::runtime_error("board GPIO endpoint expired; refresh inventory");
        impl_->refresh(nativeInventory());
        const auto it = std::find_if(impl_->entries.begin(), impl_->entries.end(), [&](const auto& e) { return jsonString(e.second.data, "endpoint_id") == id; });
        if (it == impl_->entries.end() || jsonString(it->second.data, "adapter") != jsonString(args, "adapter")) throw std::runtime_error("board GPIO identity changed; refresh inventory");
        const auto entry = it->second; const bool write = request.capabilityId == "gpio.board.set";
        values.objectItems()["endpoint"] = entry.data;
        if (!jsonBoolean(entry.data, write ? "writable" : "readable")) throw std::runtime_error("board GPIO unavailable: " + jsonString(entry.data, "reason"));
        if (cancel || request.toolbox->expired()) throw std::runtime_error("board GPIO task expired before access");
        Descriptor parent(::open(entry.attribute.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        if (parent.value < 0) throw ioError("open board GPIO directory");
        struct stat parentStat{};
        if (::fstat(parent.value, &parentStat) != 0 || std::to_string(parentStat.st_dev) + ":" + std::to_string(parentStat.st_ino) != entry.parentIdentity)
            throw std::runtime_error("board GPIO directory changed");
        Descriptor fd(::openat(parent.value, entry.attribute.filename().c_str(), (write ? O_RDWR : O_RDONLY) | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
        if (fd.value < 0) throw ioError("open board GPIO attribute");
        struct stat status{};
        if (::fstat(fd.value, &status) != 0 || !S_ISREG(status.st_mode) ||
            std::to_string(status.st_dev) + ":" + std::to_string(status.st_ino) != entry.nodeIdentity) throw std::runtime_error("board GPIO attribute changed");
        const int before = valueAt(fd.value); values.objectItems()["before_value"] = JsonValue::integer(before);
        if (write) {
            if (cancel || request.toolbox->expired()) throw std::runtime_error("board GPIO task expired before write");
            const int level = static_cast<int>(jsonInteger(args, "level")); const auto command = std::to_string(10 + level) + "\n";
            values.objectItems()["target_level"] = JsonValue::integer(level);
            values.objectItems()["driver_command"] = JsonValue::integer(10 + level);
            if (::lseek(fd.value, 0, SEEK_SET) < 0) throw ioError("seek board GPIO write");
            values.objectItems()["write_attempted"] = JsonValue::boolean(true);
            const auto count = impl_->store ? impl_->store(fd.value, command) : ::write(fd.value, command.data(), command.size());
            if (count != static_cast<ssize_t>(command.size())) throw std::runtime_error("board GPIO write failed or incomplete; hardware state is unknown");
            values.objectItems()["write_completed"] = JsonValue::boolean(true);
        }
        const int after = write ? valueAt(fd.value) : before;
        values.objectItems()["physical_level"] = JsonValue::integer(after);
        if (write) {
            const auto afterInventory = impl_->refresh(nativeInventory());
            bool verified = false;
            for (const auto& endpoint : afterInventory.find("endpoints")->arrayItems()) if (jsonString(endpoint, "endpoint_id") == id)
                verified = jsonBoolean(endpoint, "writable") && jsonString(endpoint, "direction") == "output";
            values.objectItems()["direction_verified"] = JsonValue::boolean(verified);
            if (!verified || after != jsonInteger(args, "level")) throw std::runtime_error("board GPIO was written but readback or output direction does not match");
        }
        result.succeeded = true; result.exitCode = 0;
        result.summary = write ? "板载 GPIO 已设置并读回；设置持续生效，不自动恢复" : "已读取板载 GPIO 电平，未改变方向";
#else
        (void)request; (void)cancel; (void)nativeInventory;
        throw std::runtime_error("board GPIO interfaces require Linux or Android");
#endif
    } catch (const std::exception& error) { result.summary = error.what(); result.exitCode = -1; }
    result.observations = values;
    if (!result.succeeded) { result.cancelled = cancel.load(); result.timedOut = request.toolbox && request.toolbox->expired() && !result.cancelled; }
    if (request.toolbox) request.toolbox->update(result.succeeded ? "completed" : "failed", values, result.summary);
    return result;
}
} // namespace testtools::agent
