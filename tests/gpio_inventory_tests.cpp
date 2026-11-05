#include "application/toolbox/gpio_inventory.h"
#include <iostream>
#include <stdexcept>
using namespace testtools;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main()
{
    try {
        auto pin = JsonValue::object({{"chip", JsonValue::string("gpiochip3")}, {"offset", JsonValue::integer(21)},
            {"used", JsonValue::boolean(true)}, {"consumer", JsonValue::string("signal")}, {"direction", JsonValue::string("output")}});
        auto endpoint = JsonValue::object({{"chip", JsonValue::string("gpiochip3")}, {"offset", JsonValue::integer(21)},
            {"endpoint_id", JsonValue::string(std::string(32, 'a'))}, {"adapter", JsonValue::string("rocktech-gpio/v1")},
            {"name", JsonValue::string("gpio1")}, {"readable", JsonValue::boolean(true)}, {"writable", JsonValue::boolean(true)},
            {"preference_key", JsonValue::string("board:virtual/gpio1/value")}});
        const std::vector<std::string> caps{"gpio.pin.read", "gpio.pin.write", "gpio.pin.pulse", "gpio.edge.monitor", "gpio.board.read", "gpio.board.set"};
        auto native = JsonValue::object({{"pins", JsonValue::array({pin})}, {"chips", JsonValue::array({JsonValue::object({
            {"id", JsonValue::string("gpiochip3")}, {"label", JsonValue::string("gpio3")}, {"name", JsonValue::string("gpiochip3")}})})}});
        auto adapted = JsonValue::object({{"endpoints", JsonValue::array({endpoint})}});
        auto rows = unifiedGpioInventory(native, adapted, caps);
        check(rows.size() == 1 && jsonBoolean(rows[0], "used") && jsonString(rows[0], "consumer") == "signal", "one resource preserves kernel occupancy");
        check(jsonString(rows[0], "output_operation") == "gpio.board.set" && jsonBoolean(rows[0], "output_persistent"), "known occupied interface has explicit persistent effect");
        check(jsonString(rows[0], "pulse_operation").empty() && jsonString(rows[0], "monitor_operation").empty(), "adapter does not invent timing or input capabilities");
        auto favorites = JsonValue::object({{"gpio3/gpiochip3:21", JsonValue::string("signal A")}, {"board:virtual/gpio1/value", JsonValue::string("signal B")}, {"unrelated", JsonValue::string("keep")}});
        check(gpioRemark(rows[0], favorites) == "signal A / signal B", "both old remarks survive mapping changes");
        removeGpioFavorite(rows[0], favorites);
        check(gpioRemark(rows[0], favorites).empty() && favorites.find("unrelated"), "removal clears aliases without touching other favorites");
        auto legacy = unifiedGpioInventory(native, JsonValue::object(), caps);
        check(gpioPreferenceKey(legacy[0]) == gpioPreferenceKey(rows[0]) && jsonString(legacy[0], "read_operation").empty(), "stable physical identity and no occupied native fallback");
        adapted.objectItems()["endpoints"] = JsonValue::array({endpoint, endpoint});
        rows = unifiedGpioInventory(native, adapted, caps);
        check(rows.size() == 1 && jsonString(rows[0], "output_operation").empty(), "ambiguous access remains blocked without duplicate physical rows");
        native.find("pins")->arrayItems()[0].objectItems()["used"] = JsonValue::boolean(false);
        native.find("pins")->arrayItems()[0].objectItems()["output_persistent"] = JsonValue::boolean(true);
        rows = unifiedGpioInventory(native, adapted, caps);
        check(jsonString(rows[0], "output_operation") == "gpio.pin.write" && !jsonBoolean(rows[0], "output_persistent"), "free pin retains timed native lifecycle");
        rows = unifiedGpioInventory(native, adapted, {"gpio.pin.read"});
        check(jsonString(rows[0], "read_operation") == "gpio.pin.read" && jsonString(rows[0], "output_operation").empty(), "advertised capabilities gate each operation");
        native.find("pins")->arrayItems()[0].objectItems()["used"] = JsonValue::boolean(true);
        native.find("pins")->arrayItems()[0].objectItems()["read_operation"] = JsonValue::string("gpio.pin.read");
        rows = unifiedGpioInventory(native, JsonValue::object(), caps);
        check(jsonString(rows[0], "read_operation").empty(), "raw metadata cannot grant derived operations");
        std::cout << "PASS unified GPIO identity, occupancy, access, effects and legacy remarks\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
