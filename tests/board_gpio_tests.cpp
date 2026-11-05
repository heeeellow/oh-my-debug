#include "agent/board_gpio.h"
#include "agent/toolbox.h"
#include "core/toolbox/toolbox_contract.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

using namespace testtools;
namespace fs = std::filesystem;
namespace {
void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
void put(const fs::path& path, const std::string& value)
{
    fs::create_directories(path.parent_path()); std::ofstream file(path, std::ios::binary); file.write(value.data(), value.size());
}
std::string cells(std::initializer_list<unsigned> values)
{
    std::string result;
    for (auto n : values) for (int shift = 24; shift >= 0; shift -= 8) result.push_back(static_cast<char>(n >> shift));
    return result;
}
void link(const fs::path& from, const fs::path& to)
{
    fs::create_directories(from.parent_path()); fs::create_directories(to); fs::create_directory_symlink(to, from);
}
struct Fixture {
    fs::path root, attribute;
    JsonValue native;
    int writes = 0, mode = 0;
    std::unique_ptr<agent::BoardGpioRegistry> registry;
    explicit Fixture(bool direct = false) {
        char pattern[] = "/tmp/omd-board-gpio-XXXXXX";
        const auto* temp = ::mkdtemp(pattern); require(temp, "cannot create isolated board GPIO fixture"); root = temp;
        const auto dt = root / "firmware/devicetree/base";
        const auto device = root / "devices/platform/company-gpio";
        const auto controller = root / "devices/platform/controller3";
        fs::create_directories(controller / "gpiochip3");
        link(root / "bus/platform/devices/company-gpio", device);
        link(device / "driver", root / (direct ? "bus/platform/drivers/rk_gpio" : "bus/platform/drivers/rocktech_gpio"));
        link(device / "of_node", dt / "company-gpio");
        put(dt / "company-gpio/compatible", std::string(direct ? "rk_gpio" : "fsl,rocktech_gpio") + '\0');
        link(controller / "of_node", dt / "pinctrl/gpio3");
        link(root / "bus/gpio/devices/gpiochip3", controller / "gpiochip3");
        put(dt / "pinctrl/gpio3/phandle", cells({0xaf})); put(dt / "pinctrl/gpio3/#gpio-cells", cells({2}));
        if (direct) {
            for (int i = 1; i <= 6; ++i) {
                put(dt / ("company-gpio/gpio" + std::to_string(i)), cells({0xaf, static_cast<unsigned>(12 + i), 1}));
                put(device / ("gpio" + std::to_string(i - 1)), "0\n");
            }
            attribute = device / "gpio0";
        } else {
            put(dt / "company-gpio/gpio_01/label", std::string("gpio1\0", 6));
            put(dt / "company-gpio/gpio_01/gpios", cells({0xaf, 13, 1}));
            const auto virtualDevice = root / "devices/virtual/rock_gpio/gpio1";
            link(root / "class/rock_gpio/gpio1", virtualDevice); put(virtualDevice / "value", "0\n"); attribute = virtualDevice / "value";
        }
        JsonValue::Array pins;
        for (int i = 1; i <= 6; ++i) pins.push_back(JsonValue::object({{"chip", JsonValue::string("gpiochip3")}, {"offset", JsonValue::integer(12 + i)},
            {"consumer", JsonValue::string("gpio" + std::to_string(i))}, {"used", JsonValue::boolean(true)}, {"direction", JsonValue::string("output")}}));
        native = JsonValue::object({{"chips", JsonValue::array({JsonValue::object({{"id", JsonValue::string("gpiochip3")}, {"name", JsonValue::string("gpiochip3")},
            {"label", JsonValue::string("gpio3")}, {"linux_base", JsonValue::integer(96)}, {"lines", JsonValue::integer(32)}})})}, {"pins", JsonValue::array(std::move(pins))}});
        registry = std::make_unique<agent::BoardGpioRegistry>(root.string(), [this](int fd, const std::string& command) -> std::int64_t {
            ++writes; require(command == "10\n" || command == "11\n", "board GPIO sent an input/raw-level command");
            if (mode == 1) return 1;
            const auto value = mode == 2 ? "bad\n" : command == "10\n" ? "0\n" : "1\n";
            require(::ftruncate(fd, 0) == 0, "fixture attribute truncation failed"); const auto n = ::pwrite(fd, value, std::char_traits<char>::length(value), 0);
            require(n > 0, "fixture kernel store failed"); return command.size();
        });
    }
    ~Fixture() { registry.reset(); std::error_code ec; fs::remove_all(root, ec); }
    JsonValue endpoint() {
        const auto inventory = registry->inventory(native); require(jsonString(inventory, "unavailable").empty(), "discovery failed");
        const auto& items = inventory.find("endpoints")->arrayItems(); require(!items.empty(), "board GPIO endpoints missing"); return items.front();
    }
    agent::BoardTestRunResult run(const JsonValue& endpoint, bool write, int level = 1, bool cancel = false) {
        const auto context = std::make_shared<agent::ToolContext>(); context->leaseUntil = context->deadline = agent::toolClockMs() + 3000;
        context->arguments = JsonValue::object({{"endpoint_id", *endpoint.find("endpoint_id")}, {"adapter", *endpoint.find("adapter")}});
        if (write) context->arguments.objectItems()["level"] = JsonValue::integer(level);
        return registry->run({write ? "gpio.board.set" : "gpio.board.read", 1, context}, std::atomic_bool(cancel), [this] { return native; });
    }
};
}
void runBoardGpioFixtures()
{
    for (bool direct : {false, true}) {
        Fixture f(direct); const auto e = f.endpoint();
        require(jsonInteger(e, "global_number") == 109 && jsonString(e, "rockchip_name") == "GPIO3_B5", "company alias was mapped by ordinal rather than DT identity");
        require(jsonBoolean(e, "used") && jsonBoolean(e, "writable") && jsonString(e, "name") == "gpio1", "driver-owned board GPIO not controllable");
        require(jsonString(f.endpoint(), "endpoint_id") == jsonString(e, "endpoint_id"), "read-only refresh changed stable endpoint ID");
        auto read = f.run(e, false); require(read.succeeded && f.writes == 0 && jsonInteger(read.observations, "physical_level", -1) == 0, "read changed GPIO configuration");
        auto high = f.run(e, true); require(high.succeeded && jsonInteger(high.observations, "driver_command") == 11 && jsonInteger(high.observations, "physical_level") == 1, "high output encoding/readback failed");
        auto low = f.run(e, true, 0); require(low.succeeded && jsonInteger(low.observations, "driver_command") == 10 && jsonInteger(low.observations, "physical_level", -1) == 0, "low output was sent as input command zero");
        require(f.run(e, false).succeeded && f.writes == 2, "set completion automatically restored board GPIO");
        require(!f.run(e, true, 1, true).succeeded && f.writes == 2, "cancelled GPIO request wrote a value");
        f.mode = 1; auto partial = f.run(e, true);
        require(!partial.succeeded && jsonBoolean(partial.observations, "write_attempted") && !jsonBoolean(partial.observations, "write_completed"), "partial write reported success or retried");
        f.mode = 2; auto badRead = f.run(e, true);
        require(!badRead.succeeded && jsonBoolean(badRead.observations, "write_completed"), "write effect disappeared from failed readback report");
        put(f.attribute, "0\n"); f.mode = 0;
        f.native.objectItems()["pins"].arrayItems()[0].objectItems()["consumer"] = JsonValue::string("other-driver");
        const int writesBefore = f.writes;
        require(!f.run(e, true).succeeded && f.writes == writesBefore, "owner mismatch permitted company GPIO write");
        f.native.objectItems()["pins"].arrayItems()[0].objectItems()["consumer"] = JsonValue::string("gpio1");
        fs::rename(f.attribute, f.attribute.string() + ".old"); put(f.attribute, "0\n");
        require(!f.run(e, true).succeeded && f.writes == writesBefore, "replaced endpoint accepted stale ID");
        const auto replacement = f.endpoint();
        require(jsonString(replacement, "endpoint_id") != jsonString(e, "endpoint_id"), "replacement did not invalidate endpoint token");
        const auto outside = f.root / "outside"; put(outside, "0\n"); fs::remove(f.attribute); fs::create_symlink(outside, f.attribute);
        require(!f.run(replacement, true).succeeded && f.writes == writesBefore, "symlink replacement wrote an arbitrary attribute");
    }
    {
        Fixture f; auto e = f.endpoint();
        f.native.objectItems()["chips"] = JsonValue::array();
        require(!jsonBoolean(f.endpoint(), "writable") && !f.run(e, true).succeeded && f.writes == 0, "unknown physical GPIO mapping permitted write");
    }
    {
        Fixture f; const auto dt = f.root / "firmware/devicetree/base/company-gpio";
        put(dt / "gpio_01/label", std::string("otg_drv_vbus\0", 13));
        fs::rename(f.root / "devices/virtual/rock_gpio/gpio1", f.root / "devices/virtual/rock_gpio/otg_drv_vbus");
        fs::remove(f.root / "class/rock_gpio/gpio1"); link(f.root / "class/rock_gpio/otg_drv_vbus", f.root / "devices/virtual/rock_gpio/otg_drv_vbus");
        f.native.objectItems()["pins"].arrayItems()[0].objectItems()["consumer"] = JsonValue::string("otg_drv_vbus");
        const auto e = f.endpoint(); require(!jsonBoolean(e, "writable") && f.run(e, false).succeeded && !f.run(e, true).succeeded && f.writes == 0, "VBUS was treated as an ordinary test GPIO");
    }
    std::cout << "Board GPIO filesystem fixtures passed: both ABIs, commands 10/11, owner/identity, readback failures, persistent state and protected purpose.\n";
}
