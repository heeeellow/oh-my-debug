#include "toolbox.h"
#include "board_gpio.h"
#include "core/toolbox/toolbox_contract.h"
#include "core/toolbox/iperf_json.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#if defined(__linux__)
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/gpio.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace testtools::agent {
namespace {
std::string readText(const std::string& path)
{
    std::ifstream input(path); std::string result; std::getline(input, result); return result;
}
BoardTestRunResult initialResult(const std::string& id)
{
    BoardTestRunResult result;
    result.provider = {id == "network.iperf3.server" ? "runner_pack" : "agent_native", "toolbox_native", 1};
    result.observationSchema = id + "/v1";
    return result;
}
#if defined(__linux__)
class Fd {
  public:
    explicit Fd(int value = -1) : value_(value) {}
    ~Fd() { reset(); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return value_; }
    bool reset() { if (value_ < 0) return true; const auto value = value_; value_ = -1; return ::close(value) == 0; }
  private: int value_;
};
std::runtime_error systemError(const std::string& operation)
{
    return std::runtime_error(operation + ": " + std::strerror(errno));
}
bool chipName(const std::string& name)
{
    std::string error;
    return validateToolArguments("gpio.pin.read", JsonValue::object({
        {"chip", JsonValue::string(name)}, {"offset", JsonValue::integer(0)}}), error);
}
std::vector<std::string> chips()
{
    std::vector<std::string> result;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator("/dev", error)) {
        const auto name = entry.path().filename().string();
        if (chipName(name)) result.push_back(name);
    }
    std::sort(result.begin(), result.end()); return result;
}
int openChip(const std::string& name)
{
    if (!chipName(name)) throw std::runtime_error("invalid GPIO controller");
    const auto path = "/dev/" + name;
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) throw systemError("open " + name);
    struct stat status{};
    if (::fstat(descriptor, &status) != 0 || !S_ISCHR(status.st_mode)) {
        ::close(descriptor); throw std::runtime_error("GPIO controller is not a character device");
    }
    return descriptor;
}
struct Line {
    bool v2 = false, used = false, output = false, activeLow = false;
    std::string name, consumer, drive = "push_pull";
};
Line lineInfo(int descriptor, unsigned int offset)
{
    Line result;
#if defined(GPIO_V2_GET_LINEINFO_IOCTL)
    gpio_v2_line_info info{}; info.offset = offset;
    if (::ioctl(descriptor, GPIO_V2_GET_LINEINFO_IOCTL, &info) == 0) {
        result.v2 = true; result.used = (info.flags & GPIO_V2_LINE_FLAG_USED) != 0;
        result.output = (info.flags & GPIO_V2_LINE_FLAG_OUTPUT) != 0;
        result.activeLow = (info.flags & GPIO_V2_LINE_FLAG_ACTIVE_LOW) != 0;
        result.name = std::string(info.name, strnlen(info.name, sizeof(info.name)));
        result.consumer = std::string(info.consumer, strnlen(info.consumer, sizeof(info.consumer)));
        if (info.flags & GPIO_V2_LINE_FLAG_OPEN_DRAIN) result.drive = "open_drain";
        if (info.flags & GPIO_V2_LINE_FLAG_OPEN_SOURCE) result.drive = "open_source";
        return result;
    }
    // 部分 Rockchip 4.19 内核对未知 v2 ioctl 返回 EINVAL；只在 v1 查询也成功时采用兼容结果。
    // 权限等其他错误保留原错误，不能降级绕过。
    if (errno != ENOTTY && errno != EINVAL) throw systemError("GPIO v2 line info");
#endif
    gpioline_info old{}; old.line_offset = offset;
    if (::ioctl(descriptor, GPIO_GET_LINEINFO_IOCTL, &old) != 0) throw systemError("GPIO v1 line info");
    result.used = (old.flags & GPIOLINE_FLAG_KERNEL) != 0;
    result.output = (old.flags & GPIOLINE_FLAG_IS_OUT) != 0;
    result.activeLow = (old.flags & GPIOLINE_FLAG_ACTIVE_LOW) != 0;
    result.name = std::string(old.name, strnlen(old.name, sizeof(old.name)));
    result.consumer = std::string(old.consumer, strnlen(old.consumer, sizeof(old.consumer)));
    if (old.flags & GPIOLINE_FLAG_OPEN_DRAIN) result.drive = "open_drain";
    if (old.flags & GPIOLINE_FLAG_OPEN_SOURCE) result.drive = "open_source";
    return result;
}
std::optional<std::int64_t> linuxGpioBase(const std::string& chip, const std::string& label, unsigned int lines)
{
    std::error_code error;
    const auto device = std::filesystem::canonical("/sys/bus/gpio/devices/" + chip, error).parent_path();
    if (error || device.empty()) return {};
    std::optional<std::int64_t> found;
    for (const auto& entry : std::filesystem::directory_iterator("/sys/class/gpio", error)) {
        if (!chipName(entry.path().filename().string())) continue;
        std::error_code linkError;
        if (std::filesystem::canonical(entry.path() / "device", linkError) != device || linkError ||
            readText((entry.path() / "label").string()) != label) continue;
        try {
            if (std::stoll(readText((entry.path() / "ngpio").string())) != lines) continue;
            const auto base = std::stoll(readText((entry.path() / "base").string()));
            if (base < 0 || base > 2147483647 - lines) continue;
            if (found) return {};
            found = base;
        } catch (...) {}
    }
    return found;
}
int requestLine(int descriptor, unsigned int offset, const Line& info,
                const std::string& operation, const JsonValue& arguments)
{
    const bool output = operation == "gpio.pin.write" || operation == "gpio.pin.pulse";
    const bool monitor = operation == "gpio.edge.monitor";
    const auto edge = jsonString(arguments, "edge");
    const auto drive = jsonString(arguments, "drive", "push_pull");
    const int initial = static_cast<int>(jsonInteger(arguments,
        operation == "gpio.pin.pulse" ? "idle_level" : "level")) ^ info.activeLow;
#if defined(GPIO_V2_GET_LINE_IOCTL)
    if (info.v2) {
        gpio_v2_line_request request{};
        request.offsets[0] = offset; request.num_lines = 1;
        std::strncpy(request.consumer, "ohmydebug", sizeof(request.consumer) - 1);
        auto& flags = request.config.flags;
        if (info.activeLow) flags |= GPIO_V2_LINE_FLAG_ACTIVE_LOW;
        if (output) {
            flags |= GPIO_V2_LINE_FLAG_OUTPUT;
            if (drive == "open_drain") flags |= GPIO_V2_LINE_FLAG_OPEN_DRAIN;
            if (drive == "open_source") flags |= GPIO_V2_LINE_FLAG_OPEN_SOURCE;
            request.config.num_attrs = 1;
            request.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
            request.config.attrs[0].attr.values = initial;
            request.config.attrs[0].mask = 1;
        } else if (monitor) {
            flags |= GPIO_V2_LINE_FLAG_INPUT;
            if (edge == "rising" || edge == "both") flags |= GPIO_V2_LINE_FLAG_EDGE_RISING;
            if (edge == "falling" || edge == "both") flags |= GPIO_V2_LINE_FLAG_EDGE_FALLING;
            request.event_buffer_size = 256;
        }
        // 读取时不设置方向/偏置/驱动标志，保持原配置（as-is）。
        if (::ioctl(descriptor, GPIO_V2_GET_LINE_IOCTL, &request) != 0) throw systemError("request GPIO v2 line");
        ::fcntl(request.fd, F_SETFD, FD_CLOEXEC);
        if (monitor) ::fcntl(request.fd, F_SETFL, O_NONBLOCK);
        return request.fd;
    }
#endif
    if (monitor) {
        gpioevent_request request{}; request.lineoffset = offset;
        request.handleflags = GPIOHANDLE_REQUEST_INPUT | (info.activeLow ? GPIOHANDLE_REQUEST_ACTIVE_LOW : 0);
        request.eventflags = edge == "rising" ? GPIOEVENT_REQUEST_RISING_EDGE :
            edge == "falling" ? GPIOEVENT_REQUEST_FALLING_EDGE : GPIOEVENT_REQUEST_BOTH_EDGES;
        std::strncpy(request.consumer_label, "ohmydebug", sizeof(request.consumer_label) - 1);
        if (::ioctl(descriptor, GPIO_GET_LINEEVENT_IOCTL, &request) != 0) throw systemError("request GPIO v1 events");
        ::fcntl(request.fd, F_SETFD, FD_CLOEXEC); ::fcntl(request.fd, F_SETFL, O_NONBLOCK);
        return request.fd;
    }
    gpiohandle_request request{}; request.lineoffsets[0] = offset; request.lines = 1;
    request.flags = info.activeLow ? GPIOHANDLE_REQUEST_ACTIVE_LOW : 0;
    if (output) {
        request.flags |= GPIOHANDLE_REQUEST_OUTPUT;
        if (drive == "open_drain") request.flags |= GPIOHANDLE_REQUEST_OPEN_DRAIN;
        if (drive == "open_source") request.flags |= GPIOHANDLE_REQUEST_OPEN_SOURCE;
        request.default_values[0] = static_cast<unsigned char>(initial);
    }
    std::strncpy(request.consumer_label, "ohmydebug", sizeof(request.consumer_label) - 1);
    if (::ioctl(descriptor, GPIO_GET_LINEHANDLE_IOCTL, &request) != 0) throw systemError("request GPIO v1 line");
    ::fcntl(request.fd, F_SETFD, FD_CLOEXEC); return request.fd;
}
int valueOf(int descriptor, const Line& info)
{
#if defined(GPIO_V2_LINE_GET_VALUES_IOCTL)
    if (info.v2) {
        gpio_v2_line_values values{}; values.mask = 1;
        if (::ioctl(descriptor, GPIO_V2_LINE_GET_VALUES_IOCTL, &values) != 0) throw systemError("read GPIO v2");
        return (values.bits & 1) != 0;
    }
#endif
    gpiohandle_data values{};
    if (::ioctl(descriptor, GPIOHANDLE_GET_LINE_VALUES_IOCTL, &values) != 0) throw systemError("read GPIO v1");
    return values.values[0] != 0;
}
void setValue(int descriptor, const Line& info, int physical)
{
    const int logical = physical ^ info.activeLow;
#if defined(GPIO_V2_LINE_SET_VALUES_IOCTL)
    if (info.v2) {
        gpio_v2_line_values values{}; values.mask = 1; values.bits = logical;
        if (::ioctl(descriptor, GPIO_V2_LINE_SET_VALUES_IOCTL, &values) != 0) throw systemError("set GPIO v2");
        return;
    }
#endif
    gpiohandle_data values{}; values.values[0] = static_cast<unsigned char>(logical);
    if (::ioctl(descriptor, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &values) != 0) throw systemError("set GPIO v1");
}
std::string runnerPath()
{
    char path[4096]{}; const auto length = ::readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (length <= 0) return {};
    return (std::filesystem::path(std::string(path, length)).parent_path() / "iperf3").string();
}
bool ownsListener(pid_t process, int port)
{
    std::set<std::string> inodes;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/" + std::to_string(process) + "/fd", error)) {
        const auto target = std::filesystem::read_symlink(entry.path(), error).string();
        if (!error && target.rfind("socket:[", 0) == 0 && target.back() == ']') inodes.insert(target.substr(8, target.size() - 9));
        error.clear();
    }
    std::ifstream tcp("/proc/net/tcp"); std::string line; std::getline(tcp, line);
    while (std::getline(tcp, line)) {
        std::istringstream row(line); std::vector<std::string> columns; std::string word;
        while (row >> word) columns.push_back(word);
        if (columns.size() < 10 || columns[3] != "0A" || !inodes.count(columns[9])) continue;
        const auto colon = columns[1].find(':');
        if (colon != std::string::npos && std::stoi(columns[1].substr(colon + 1), nullptr, 16) == port) return true;
    }
    return false;
}
bool boardRouteMatches(const std::string& interface, const std::string& source, const std::string& peer)
{
    if (source == peer || !validInterfaceName(interface) || !validIpv4(source) || !validIpv4(peer)) return false;
    const auto indexText = readText("/sys/class/net/" + interface + "/ifindex");
    int expected = 0;
    try { expected = std::stoi(indexText); } catch (...) { return false; }
    Fd socket(::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE));
    if (socket.get() < 0) return false;
    timeval timeout{1, 0};
    ::setsockopt(socket.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    struct Request { nlmsghdr header; rtmsg route; char attributes[64]; } request{};
    request.header.nlmsg_len = NLMSG_LENGTH(sizeof(rtmsg)); request.header.nlmsg_type = RTM_GETROUTE;
    request.header.nlmsg_flags = NLM_F_REQUEST; request.header.nlmsg_seq = 1;
    request.route.rtm_family = AF_INET; request.route.rtm_dst_len = 32; request.route.rtm_src_len = 32;
    const auto add = [&](unsigned short type, const std::string& ip) {
        in_addr address{}; if (::inet_pton(AF_INET, ip.c_str(), &address) != 1) return false;
        auto* attribute = reinterpret_cast<rtattr*>(reinterpret_cast<char*>(&request) + NLMSG_ALIGN(request.header.nlmsg_len));
        attribute->rta_type = type; attribute->rta_len = RTA_LENGTH(sizeof(address));
        std::memcpy(RTA_DATA(attribute), &address, sizeof(address));
        request.header.nlmsg_len = NLMSG_ALIGN(request.header.nlmsg_len) + RTA_ALIGN(attribute->rta_len); return true;
    };
    if (!add(RTA_SRC, source) || !add(RTA_DST, peer)) return false;
    sockaddr_nl kernel{}; kernel.nl_family = AF_NETLINK;
    // Netlink 查询仅向本机内核取路由，不向对端发送探测包。
    if (::sendto(socket.get(), &request, request.header.nlmsg_len, 0, reinterpret_cast<sockaddr*>(&kernel), sizeof(kernel)) < 0) return false;
    alignas(nlmsghdr) char buffer[8192];
    ssize_t length;
    do { length = ::recv(socket.get(), buffer, sizeof(buffer), 0); } while (length < 0 && errno == EINTR);
    if (length <= 0) return false;
    int remaining = static_cast<int>(length);
    for (auto* message = reinterpret_cast<nlmsghdr*>(buffer); NLMSG_OK(message, remaining); message = NLMSG_NEXT(message, remaining)) {
        if (message->nlmsg_seq != 1 || message->nlmsg_type != RTM_NEWROUTE) continue;
        auto* route = reinterpret_cast<rtmsg*>(NLMSG_DATA(message));
        if (route->rtm_family != AF_INET || route->rtm_type != RTN_UNICAST) return false;
        int attributes = RTM_PAYLOAD(message), outgoing = 0;
        for (auto* attribute = RTM_RTA(route); RTA_OK(attribute, attributes); attribute = RTA_NEXT(attribute, attributes))
            if (attribute->rta_type == RTA_OIF && RTA_PAYLOAD(attribute) >= sizeof(outgoing)) std::memcpy(&outgoing, RTA_DATA(attribute), sizeof(outgoing));
        return outgoing == expected;
    }
    return false;
}
#endif

class NativeToolbox final : public IToolboxPlatform {
  public:
    JsonValue gpioInventory(const std::string& selected, std::int64_t cursor, int limit) override;
    JsonValue boardGpioInventory() override { return boardGpio_.inventory(allGpio()); }
    JsonValue networkInventory() override;
    bool hasRunner() const override;
    BoardTestRunResult run(const BoardTestRunRequest&, const std::atomic_bool&) override;
  private:
    BoardGpioRegistry boardGpio_;
    JsonValue allGpio() {
        auto result = gpioInventory({}, 0, 256);
        JsonValue::Array pins;
        for (auto page = result;;) {
            if (const auto* batch = page.find("pins"); batch && batch->isArray()) for (const auto& pin : batch->arrayItems()) pins.push_back(pin);
            const auto cursor = jsonInteger(page, "next_cursor");
            if (cursor >= jsonInteger(page, "total") || cursor <= 0) break;
            if (pins.size() >= 65536) throw std::runtime_error("GPIO inventory exceeds limit");
            page = gpioInventory({}, cursor, 256);
            if (jsonInteger(page, "next_cursor") <= cursor) throw std::runtime_error("GPIO inventory cursor did not advance");
        }
        result.objectItems()["pins"] = JsonValue::array(std::move(pins)); return result;
    }
#if defined(__linux__)
    BoardTestRunResult gpio(const BoardTestRunRequest&, const std::atomic_bool&);
    BoardTestRunResult network(const BoardTestRunRequest&, const std::atomic_bool&);
#endif
};
JsonValue NativeToolbox::gpioInventory(const std::string& selected, std::int64_t cursor, int limit)
{
    JsonValue::Array controllers, pins;
    std::int64_t total = 0;
    std::string unavailable;
#if defined(__linux__)
    if (!selected.empty() && !chipName(selected)) throw std::runtime_error("invalid GPIO controller");
    for (const auto& name : chips()) {
        if (!selected.empty() && name != selected) continue;
        JsonValue::Object controller{{"id", JsonValue::string(name)}};
        try {
            Fd descriptor(openChip(name)); gpiochip_info info{};
            if (::ioctl(descriptor.get(), GPIO_GET_CHIPINFO_IOCTL, &info) != 0) throw systemError("GPIO chip info");
            if (info.lines > 4096) throw std::runtime_error("GPIO controller exceeds line limit");
            controller["name"] = JsonValue::string(std::string(info.name, strnlen(info.name, sizeof(info.name))));
            controller["label"] = JsonValue::string(std::string(info.label, strnlen(info.label, sizeof(info.label))));
            const auto base = linuxGpioBase(name, jsonString(JsonValue::object(controller), "label"), info.lines);
            controller["linux_base"] = base ? JsonValue::integer(*base) : JsonValue::null();
            controller["lines"] = JsonValue::integer(info.lines);
            controller["accessible"] = JsonValue::boolean(true);
            for (unsigned int offset = 0; offset < info.lines; ++offset, ++total) {
                if (total < cursor || pins.size() >= static_cast<std::size_t>(limit)) continue;
                const auto pin = lineInfo(descriptor.get(), offset);
                pins.push_back(JsonValue::object({{"chip", JsonValue::string(name)}, {"offset", JsonValue::integer(offset)},
                    {"name", JsonValue::string(pin.name)}, {"consumer", JsonValue::string(pin.consumer)},
                    {"direction", JsonValue::string(pin.output ? "output" : "input")}, {"used", JsonValue::boolean(pin.used)},
                    {"active_low", JsonValue::boolean(pin.activeLow)}, {"drive", JsonValue::string(pin.drive)},
                    {"abi", JsonValue::integer(pin.v2 ? 2 : 1)},
                    {"global_number", base ? JsonValue::integer(*base + offset) : JsonValue::null()}}));
            }
        } catch (const std::exception& error) {
            controller["accessible"] = JsonValue::boolean(false); controller["error"] = JsonValue::string(error.what());
            unavailable = error.what();
        }
        controllers.push_back(JsonValue::object(std::move(controller)));
    }
    if (controllers.empty()) unavailable = "GPIO character devices are unavailable";
#else
    (void)selected; (void)cursor; (void)limit; unavailable = "GPIO requires a Linux or Android Agent";
#endif
    const auto next = std::min<std::int64_t>(total, cursor + static_cast<std::int64_t>(pins.size()));
    return JsonValue::object({{"schema", JsonValue::string("gpio.inventory/v1")},
        {"chips", JsonValue::array(std::move(controllers))}, {"pins", JsonValue::array(std::move(pins))},
        {"total", JsonValue::integer(total)}, {"next_cursor", JsonValue::integer(next)},
        {"complete", JsonValue::boolean(unavailable.empty())}, {"unavailable", JsonValue::string(unavailable)}});
}
JsonValue NativeToolbox::networkInventory()
{
    JsonValue::Array result;
#if defined(__linux__)
    std::map<std::string, JsonValue::Array> ipv4;
    std::map<std::string, unsigned int> flags;
    // getifaddrs 在 Bionic API 24 才可链接；保持现有 API 23 目标，使用固定 ioctl 枚举。
    Fd socket(::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0));
    if (socket.get() < 0) throw systemError("network inventory socket");
    std::array<char, 65536> buffer{};
    ifconf configuration{}; configuration.ifc_len = static_cast<int>(buffer.size()); configuration.ifc_buf = buffer.data();
    if (::ioctl(socket.get(), SIOCGIFCONF, &configuration) != 0) throw systemError("network IPv4 inventory");
    if (configuration.ifc_len >= static_cast<int>(buffer.size() - sizeof(ifreq))) throw std::runtime_error("network inventory exceeds limit");
    for (int offset = 0; offset + static_cast<int>(sizeof(ifreq)) <= configuration.ifc_len; offset += sizeof(ifreq)) {
        ifreq item{}; std::memcpy(&item, buffer.data() + offset, sizeof(item));
        if (item.ifr_addr.sa_family != AF_INET) continue;
        std::string name(item.ifr_name, strnlen(item.ifr_name, IFNAMSIZ));
        const auto alias = name.find(':'); if (alias != std::string::npos) name.resize(alias);
        if (!validInterfaceName(name)) continue;
        char text[INET_ADDRSTRLEN]{};
        if (::inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(&item.ifr_addr)->sin_addr, text, sizeof(text)))
            ipv4[name].push_back(JsonValue::string(text));
    }
    std::error_code directoryError;
    for (const auto& entry : std::filesystem::directory_iterator("/sys/class/net", directoryError)) {
        const auto name = entry.path().filename().string(); if (!validInterfaceName(name)) continue;
        ifreq item{}; std::strncpy(item.ifr_name, name.c_str(), IFNAMSIZ - 1);
        if (::ioctl(socket.get(), SIOCGIFFLAGS, &item) == 0) flags[name] = static_cast<unsigned short>(item.ifr_flags);
    }
    for (const auto& item : flags) {
        const auto base = "/sys/class/net/" + item.first;
        std::error_code error;
        const bool physical = std::filesystem::exists(base + "/device", error) &&
            !std::filesystem::exists(base + "/wireless", error) && readText(base + "/type") == "1";
        auto integerFile = [&](const std::string& suffix) {
            try { const auto text = readText(base + suffix); const auto value = std::stoll(text);
                return value >= 0 ? JsonValue::integer(value) : JsonValue::null(); }
            catch (...) { return JsonValue::null(); }
        };
        result.push_back(JsonValue::object({{"id", JsonValue::string(item.first)}, {"ipv4", JsonValue::array(ipv4[item.first])},
            {"up", JsonValue::boolean((item.second & IFF_UP) != 0)}, {"physical", JsonValue::boolean(physical)},
            {"loopback", JsonValue::boolean((item.second & IFF_LOOPBACK) != 0)},
            {"operstate", JsonValue::string(readText(base + "/operstate"))}, {"mac", JsonValue::string(readText(base + "/address"))},
            {"speed_mbps", integerFile("/speed")}, {"duplex", JsonValue::string(readText(base + "/duplex"))},
            {"rx_bytes", integerFile("/statistics/rx_bytes")}, {"tx_bytes", integerFile("/statistics/tx_bytes")},
            {"rx_errors", integerFile("/statistics/rx_errors")}, {"tx_errors", integerFile("/statistics/tx_errors")}}));
    }
#endif
    return JsonValue::object({{"schema", JsonValue::string("network.inventory/v1")}, {"interfaces", JsonValue::array(std::move(result))}});
}
bool NativeToolbox::hasRunner() const
{
#if defined(__linux__)
    const auto path = runnerPath(); struct stat status{};
    return !path.empty() && ::lstat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode) && ::access(path.c_str(), X_OK) == 0;
#else
    return false;
#endif
}
BoardTestRunResult NativeToolbox::run(const BoardTestRunRequest& request, const std::atomic_bool& cancel)
{
    auto result = initialResult(request.capabilityId);
    try {
        if (!request.toolbox) throw std::runtime_error("missing tool context");
        if (request.capabilityId == "gpio.board.read" || request.capabilityId == "gpio.board.set")
            return boardGpio_.run(request, cancel, [this] { return allGpio(); });
#if defined(__linux__)
        return request.capabilityId == "network.iperf3.server" ? network(request, cancel) : gpio(request, cancel);
#else
        (void)cancel; throw std::runtime_error("native board tools require Linux or Android");
#endif
    } catch (const std::exception& error) {
        result.summary = error.what();
        if (request.toolbox) request.toolbox->update("failed", JsonValue::object(), result.summary);
        return result;
    }
}
#if defined(__linux__)
BoardTestRunResult NativeToolbox::gpio(const BoardTestRunRequest& request, const std::atomic_bool& cancel)
{
    auto result = initialResult(request.capabilityId);
    const auto control = request.toolbox;
    const auto& arguments = control->arguments;
    Fd chip(openChip(jsonString(arguments, "chip")));
    const auto offset = static_cast<unsigned int>(jsonInteger(arguments, "offset"));
    const auto info = lineInfo(chip.get(), offset);
    if (info.used) throw std::runtime_error("GPIO is occupied by " + (info.consumer.empty() ? std::string("another consumer") : info.consumer));
    if (cancel.load() || control->expired()) throw std::runtime_error("GPIO task expired before acquisition");
    Fd handle(requestLine(chip.get(), offset, info, request.capabilityId, arguments));
    JsonValue values = JsonValue::object({{"chip", JsonValue::string(jsonString(arguments, "chip"))},
        {"offset", JsonValue::integer(offset)}, {"abi", JsonValue::integer(info.v2 ? 2 : 1)},
        {"original_direction", JsonValue::string(info.output ? "output" : "input")},
        {"active_low", JsonValue::boolean(info.activeLow)}});
    auto sample = [&] {
        const int logical = valueOf(handle.get(), info);
        values.objectItems()["logical_value"] = JsonValue::integer(logical);
        values.objectItems()["physical_level"] = JsonValue::integer(logical ^ info.activeLow);
    };
    bool overflow = false, sequenceGap = false;
    std::uint64_t count = 0, previousSequence = 0;
    const auto start = toolClockMs();
    if (request.capabilityId == "gpio.pin.read") sample();
    else if (request.capabilityId == "gpio.pin.pulse") {
        setValue(handle.get(), info, static_cast<int>(jsonInteger(arguments, "pulse_level")));
        control->update("pulsing", values);
        const auto pulseEnd = toolClockMs() + jsonInteger(arguments, "pulse_ms");
        while (!cancel.load() && !control->expired() && toolClockMs() < pulseEnd) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        values.objectItems()["software_pulse_ms"] = JsonValue::integer(toolClockMs() - (pulseEnd - jsonInteger(arguments, "pulse_ms")));
        setValue(handle.get(), info, static_cast<int>(jsonInteger(arguments, "idle_level"))); sample();
    } else if (request.capabilityId == "gpio.pin.write") {
        sample(); control->update("holding", values);
        while (!cancel.load() && !control->expired()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        sample();
    } else if (request.capabilityId == "gpio.edge.monitor") {
        values.objectItems()["clock"] = JsonValue::string(info.v2 ? "monotonic" : "kernel-v1-clock-unverified");
        values.objectItems()["kernel_loss_detectable"] = JsonValue::boolean(info.v2);
        control->update("monitoring", values);
        while (!cancel.load() && !control->expired()) {
            pollfd eventPoll{handle.get(), POLLIN, 0}; const int ready = ::poll(&eventPoll, 1, 25);
            if (ready < 0 && errno != EINTR) throw systemError("poll GPIO events");
            if (ready <= 0) continue;
            std::uint64_t timestamp = 0, sequence = 0; unsigned int eventId = 0;
#if defined(GPIO_V2_LINE_EVENT_READ)
            // Some UAPI versions do not define a read ioctl; event structures are read from the fd.
#endif
#if defined(GPIO_V2_GET_LINE_IOCTL)
            if (info.v2) {
                gpio_v2_line_event event{}; const auto size = ::read(handle.get(), &event, sizeof(event));
                if (size < 0 && (errno == EAGAIN || errno == EINTR)) continue;
                if (size != sizeof(event)) throw std::runtime_error("incomplete GPIO v2 event");
                timestamp = event.timestamp_ns; sequence = event.seqno; eventId = event.id;
                if (previousSequence && sequence != previousSequence + 1) sequenceGap = true;
                previousSequence = sequence;
            } else
#endif
            {
                gpioevent_data event{}; const auto size = ::read(handle.get(), &event, sizeof(event));
                if (size < 0 && (errno == EAGAIN || errno == EINTR)) continue;
                if (size != sizeof(event)) throw std::runtime_error("incomplete GPIO v1 event");
                timestamp = event.timestamp; eventId = event.id;
            }
            ++count;
            if (!control->appendEvent(JsonValue::object({{"timestamp_ns", JsonValue::integer(static_cast<std::int64_t>(timestamp))},
                {"edge", JsonValue::string(eventId == 1 ? "rising" : "falling")},
                {"kernel_sequence", info.v2 ? JsonValue::integer(static_cast<std::int64_t>(sequence)) : JsonValue::null()}}))) { overflow = true; break; }
            values.objectItems()["event_count"] = JsonValue::integer(static_cast<std::int64_t>(count));
            control->update("monitoring", values);
        }
    }
    const bool leaseExpired = toolClockMs() >= control->leaseUntil.load();
    const bool released = handle.reset();
    values.objectItems()["released"] = JsonValue::boolean(released);
    values.objectItems()["release_state"] = JsonValue::string("uncontrolled");
    values.objectItems()["elapsed_ms"] = JsonValue::integer(toolClockMs() - start);
    values.objectItems()["event_count"] = JsonValue::integer(static_cast<std::int64_t>(count));
    values.objectItems()["kernel_gap"] = JsonValue::boolean(sequenceGap);
    if (overflow || sequenceGap) { std::lock_guard<std::mutex> lock(control->mutex); control->complete = false; }
    result.succeeded = released && !cancel.load() && !leaseExpired && !overflow && !sequenceGap;
    result.cancelled = cancel.load(); result.timedOut = leaseExpired;
    result.exitCode = result.succeeded ? 0 : -1; result.observations = values;
    result.summary = leaseExpired ? "tool owner lease expired; GPIO handle released" : overflow ? "GPIO event queue overflow; result incomplete" :
        sequenceGap ? "kernel GPIO event sequence gap; result incomplete" : cancel.load() ? "GPIO operation stopped; handle released" : "GPIO operation completed; handle released";
    control->update("releasing", values, result.summary); return result;
}
BoardTestRunResult NativeToolbox::network(const BoardTestRunRequest& request, const std::atomic_bool& cancel)
{
    const auto context = request.toolbox; const auto& args = context->arguments;
    const auto inventory = networkInventory(); const auto* interfaces = inventory.find("interfaces");
    bool valid = false;
    if (interfaces) for (const auto& interface : interfaces->arrayItems()) {
        if (jsonString(interface, "id") != jsonString(args, "interface") || !jsonBoolean(interface, "physical") ||
            !jsonBoolean(interface, "up") || jsonString(interface, "operstate") != "up") continue;
        const auto* addresses = interface.find("ipv4");
        if (addresses) for (const auto& address : addresses->arrayItems())
            if (address.isString() && address.stringValue() == jsonString(args, "bind_ip")) valid = true;
    }
    if (!valid) throw std::runtime_error("selected Ethernet interface or address is not active");
    if (!boardRouteMatches(jsonString(args, "interface"), jsonString(args, "bind_ip"), jsonString(args, "peer_ip")))
        throw std::runtime_error("board route does not use the selected Ethernet interface");
    const auto executable = runnerPath(); if (!hasRunner()) throw std::runtime_error("iperf3 runner is missing");
    std::vector<std::string> arguments{executable, "-s", "-1", "-B", jsonString(args, "bind_ip"), "-p", std::to_string(jsonInteger(args, "port")),
        "--json-stream", "--forceflush", "--idle-timeout", "10", "--server-max-duration", std::to_string(jsonInteger(args, "duration_sec"))};
    if (jsonString(args, "protocol") == "udp") { arguments.push_back("--server-bitrate-limit"); arguments.push_back(std::to_string(jsonInteger(args, "udp_bps") * 2)); }
    std::vector<char*> argv; for (auto& argument : arguments) argv.push_back(argument.data()); argv.push_back(nullptr);
    int outPipe[2], errPipe[2];
    if (::pipe2(outPipe, O_CLOEXEC) != 0) throw systemError("iperf stdout pipe");
    Fd output(outPipe[0]), outputWriter(outPipe[1]);
    if (::pipe2(errPipe, O_CLOEXEC) != 0) throw systemError("iperf stderr pipe");
    Fd errors(errPipe[0]), errorWriter(errPipe[1]);
    const auto process = ::fork();
    if (process < 0) throw systemError("fork iperf3");
    if (process == 0) {
        ::setsid(); ::dup2(outputWriter.get(), STDOUT_FILENO); ::dup2(errorWriter.get(), STDERR_FILENO);
        ::close(output.get()); ::close(errors.get()); ::execv(executable.c_str(), argv.data()); ::_exit(127);
    }
    outputWriter.reset(); errorWriter.reset();
    ::fcntl(output.get(), F_SETFL, O_NONBLOCK); ::fcntl(errors.get(), F_SETFL, O_NONBLOCK);
    struct Child {
        pid_t pid; bool reaped = false;
        ~Child() { if (!reaped) { ::kill(-pid, SIGKILL); ::kill(pid, SIGKILL); int status; while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {} } }
    } child{process};
    IperfJsonStream parser; std::string stderrText, parseError;
    bool ready = false, processEnded = false; int status = 0;
    const auto readyDeadline = toolClockMs() + 5000;
    context->update("starting_server");
    const auto drain = [&](int descriptor, bool standardOutput) {
        char buffer[8192];
        while (true) {
            const auto size = ::read(descriptor, buffer, sizeof(buffer));
            if (size > 0) {
                if (standardOutput) {
                    if (!parser.push(std::string(buffer, size), parseError)) return false;
                } else {
                    if (stderrText.size() + size > 8U * 1024 * 1024) { parseError = "iperf stderr exceeds limit"; return false; }
                    stderrText.append(buffer, size);
                }
            } else if (size == 0 || errno == EAGAIN || errno == EWOULDBLOCK) break;
            else if (errno != EINTR) { parseError = "iperf output read failed"; return false; }
        }
        return true;
    };
    while (!cancel.load() && !context->expired()) {
        if (!drain(output.get(), true) || !drain(errors.get(), false)) break;
        const auto ended = ::waitpid(process, &status, WNOHANG);
        if (ended == process) { child.reaped = true; processEnded = true; break; }
        if (ended < 0 && errno != EINTR) throw systemError("wait iperf3");
        if (!ready && ownsListener(process, static_cast<int>(jsonInteger(args, "port")))) {
            ready = true; context->update("ready", JsonValue::object({{"pid", JsonValue::integer(process)}}));
        }
        if (!ready && toolClockMs() >= readyDeadline) { parseError = "iperf3 listener readiness timed out"; break; }
        if (!parser.peer().empty() && parser.peer() != jsonString(args, "peer_ip")) { parseError = "iperf3 peer does not match the requested PC"; break; }
        if (!parser.raw().empty()) context->update("running", parser.metrics());
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (processEnded) { drain(output.get(), true); drain(errors.get(), false); }
    if (!child.reaped) {
        ::kill(-process, SIGTERM); ::kill(process, SIGTERM);
        for (int attempt = 0; attempt < 20; ++attempt) {
            if (::waitpid(process, &status, WNOHANG) == process) { child.reaped = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!child.reaped) { ::kill(-process, SIGKILL); ::kill(process, SIGKILL); while (::waitpid(process, &status, 0) < 0 && errno == EINTR) {} child.reaped = true; }
    }
    const bool parsed = parseError.empty() && processEnded && parser.finish(parseError);
    const bool peerMatches = parser.peer() == jsonString(args, "peer_ip");
    const bool clean = !ownsListener(process, static_cast<int>(jsonInteger(args, "port")));
    auto result = initialResult(request.capabilityId);
    result.cancelled = cancel.load(); result.timedOut = context->expired() && !result.cancelled;
    result.succeeded = !result.cancelled && !result.timedOut && parsed && peerMatches && clean && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    result.summary = result.succeeded ? "iperf3 server completed and exited" :
        !parseError.empty() ? parseError : result.cancelled ? "network test stopped" : result.timedOut ? "network test deadline or owner lease expired" : "iperf3 process or peer validation failed";
    result.observations = JsonValue::object({{"metrics", parser.metrics()}, {"peer_ip", JsonValue::string(parser.peer())},
        {"local_ip", JsonValue::string(parser.local())}, {"cleaned", JsonValue::boolean(clean)},
        {"server_log_truncated", JsonValue::boolean(parser.raw().size() + stderrText.size() > 60000)}});
    result.log = (parser.raw() + stderrText).substr(0, 60000);
    context->update("cleaning", result.observations, result.summary); return result;
}
#endif
} // namespace
std::shared_ptr<IToolboxPlatform> createToolboxPlatform() { return std::make_shared<NativeToolbox>(); }
} // namespace testtools::agent
