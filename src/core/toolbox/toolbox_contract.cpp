#include "toolbox_contract.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <random>
#include <sstream>
#include <regex>

namespace testtools {
bool convertRockchipGpio(const std::string& input, RockchipGpioNumber& result, std::string& error)
{
    std::string value;
    for (const unsigned char c : input) if (!std::isspace(c)) value.push_back(static_cast<char>(std::toupper(c)));
    if (value.size() > 16) { error = "请输入 GPIO3_B4 或 108 这样的管脚号"; return false; }
    std::smatch match;
    int bank = 0, offset = 0;
    if (std::regex_match(value, match, std::regex("(?:GPIO)?([0-9]{1,3})_([A-D])([0-7])"))) {
        bank = std::stoi(match[1].str()); offset = (match[2].str()[0] - 'A') * 8 + std::stoi(match[3].str());
    } else if (!value.empty() && value.size() <= 4 && std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        const auto number = std::stoi(value); bank = number / 32; offset = number % 32;
    } else { error = "请输入 GPIO3_B4 或 108 这样的管脚号"; return false; }
    if (bank > 127) { error = "换算范围为 0–4095（bank 0–127）"; return false; }
    result.bank = bank; result.offset = offset; result.number = bank * 32 + offset;
    result.name = "GPIO" + std::to_string(bank) + "_" + static_cast<char>('A' + offset / 8) + std::to_string(offset % 8);
    error.clear(); return true;
}
bool onlyFields(const JsonValue& value, const std::vector<std::string>& fields)
{
    if (!value.isObject()) return false;
    for (const auto& item : value.objectItems())
        if (std::find(fields.begin(), fields.end(), item.first) == fields.end()) return false;
    return true;
}
std::string jsonString(const JsonValue& value, const std::string& key, const std::string& fallback)
{
    const auto* v = value.find(key);
    return v && v->isString() ? v->stringValue() : fallback;
}
std::int64_t jsonInteger(const JsonValue& value, const std::string& key, std::int64_t fallback)
{
    const auto* v = value.find(key);
    return v && v->isInt64() ? v->int64Value() : fallback;
}
bool jsonBoolean(const JsonValue& value, const std::string& key, bool fallback)
{
    const auto* v = value.find(key);
    return v && v->isBool() ? v->boolValue() : fallback;
}
bool validToolToken(const std::string& value)
{
    return value.size() == 32 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
std::string makeToolToken()
{
    std::random_device random;
    static const char hex[] = "0123456789abcdef";
    std::string result(32, '0');
    for (auto& c : result) c = hex[random() & 15];
    return result;
}
bool validInterfaceName(const std::string& value)
{
    return !value.empty() && value.size() < 16 && value != "." && value != ".." &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '_' || c == '-' || c == '.' || c == ':';
        });
}
bool validIpv4(const std::string& value)
{
    std::istringstream stream(value);
    std::string part;
    int count = 0;
    int first = -1;
    while (std::getline(stream, part, '.')) {
        if (part.empty() || part.size() > 3 || (part.size() > 1 && part[0] == '0') ||
            !std::all_of(part.begin(), part.end(), [](unsigned char c) { return c >= '0' && c <= '9'; })) return false;
        const int octet = std::stoi(part);
        if (octet > 255) return false;
        if (count == 0) first = octet;
        ++count;
    }
    return count == 4 && value.back() != '.' && first > 0 && first < 224 && first != 127;
}
bool isToolCapability(const std::string& id)
{
    static const std::vector<std::string> ids{
        "gpio.pin.read", "gpio.pin.write", "gpio.pin.pulse", "gpio.edge.monitor", "gpio.board.read", "gpio.board.set", "network.iperf3.server"};
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}
bool validateToolArguments(const std::string& id, const JsonValue& args, std::string& error)
{
    const auto fail = [&](const std::string& text) { error = text; return false; };
    const auto integer = [&](const char* key, std::int64_t low, std::int64_t high) {
        const auto* v = args.find(key);
        return v && v->isInt64() && v->int64Value() >= low && v->int64Value() <= high;
    };
    const auto enumeration = [&](const char* key, const std::vector<std::string>& allowed) {
        const auto* v = args.find(key);
        return v && v->isString() && std::find(allowed.begin(), allowed.end(), v->stringValue()) != allowed.end();
    };
    if (!args.isObject() || !isToolCapability(id)) return fail("unsupported tool capability");
    if (id == "gpio.board.read" || id == "gpio.board.set") {
        const bool write = id == "gpio.board.set";
        if (!validToolToken(jsonString(args, "endpoint_id")) ||
            !enumeration("adapter", {"rocktech-gpio/v1", "rk-gpio/v1"}) ||
            !onlyFields(args, write ? std::vector<std::string>{"endpoint_id", "adapter", "level"} :
                std::vector<std::string>{"endpoint_id", "adapter"}) || (write && !integer("level", 0, 1)))
            return fail("invalid board GPIO endpoint or level");
        return true;
    }
    if (id == "network.iperf3.server") {
        if (!onlyFields(args, {"interface", "bind_ip", "peer_ip", "port", "duration_sec", "protocol", "parallel", "udp_bps"}) ||
            !validInterfaceName(jsonString(args, "interface")) || !validIpv4(jsonString(args, "bind_ip")) ||
            !validIpv4(jsonString(args, "peer_ip")) || !integer("port", 5201, 5210) ||
            !integer("duration_sec", 1, 60) || !enumeration("protocol", {"tcp", "udp"}) ||
            !integer("parallel", 1, 8) || !integer("udp_bps", 1000000, 1000000000))
            return fail("invalid network parameters");
        if (jsonString(args, "protocol") == "udp" && jsonInteger(args, "parallel") != 1)
            return fail("UDP requires one stream");
        return true;
    }
    const auto chip = jsonString(args, "chip");
    if (chip.size() < 9 || chip.size() > 13 || chip.rfind("gpiochip", 0) != 0 ||
        !std::all_of(chip.begin() + 8, chip.end(), [](unsigned char c) { return c >= '0' && c <= '9'; }) ||
        !integer("offset", 0, 4095)) return fail("invalid GPIO identity");
    std::vector<std::string> fields{"chip", "offset"};
    if (id == "gpio.pin.write") {
        fields.insert(fields.end(), {"level", "hold_sec", "drive"});
        if (!integer("level", 0, 1) || !integer("hold_sec", 1, 300)) return fail("invalid output parameters");
    } else if (id == "gpio.pin.pulse") {
        fields.insert(fields.end(), {"idle_level", "pulse_level", "pulse_ms", "drive"});
        if (!integer("idle_level", 0, 1) || !integer("pulse_level", 0, 1) || !integer("pulse_ms", 10, 10000) ||
            jsonInteger(args, "idle_level") == jsonInteger(args, "pulse_level")) return fail("invalid pulse parameters");
    } else if (id == "gpio.edge.monitor") {
        fields.insert(fields.end(), {"edge", "duration_sec"});
        if (!enumeration("edge", {"rising", "falling", "both"}) || !integer("duration_sec", 1, 300))
            return fail("invalid monitor parameters");
    }
    if ((id == "gpio.pin.write" || id == "gpio.pin.pulse") &&
        !enumeration("drive", {"push_pull", "open_drain", "open_source"})) return fail("invalid drive mode");
    if (!onlyFields(args, fields)) return fail("unexpected GPIO parameter");
    return true;
}
} // namespace testtools
