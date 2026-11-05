#include "application/toolbox/gpio_inventory.h"
#include <algorithm>
#include <set>

namespace testtools {
namespace {
void addUnique(JsonValue& array, const std::string& value)
{
    if (value.empty()) return;
    for (const auto& item : array.arrayItems()) if (item.stringValue() == value) return;
    array.arrayItems().push_back(JsonValue::string(value));
}
const JsonValue::Array& items(const JsonValue& object, const char* key)
{
    static const JsonValue::Array empty;
    const auto* value = object.find(key);
    return value && value->isArray() ? value->arrayItems() : empty;
}
}
std::string gpioPreferenceKey(const JsonValue& pin) { return jsonString(pin, "view_key"); }
std::string gpioRemark(const JsonValue& pin, const JsonValue& favorites)
{
    std::vector<std::string> remarks;
    for (const auto& key : items(pin, "preference_keys")) {
        const auto* value = favorites.find(key.stringValue());
        if (value && value->isString() && !value->stringValue().empty() &&
            std::find(remarks.begin(), remarks.end(), value->stringValue()) == remarks.end())
            remarks.push_back(value->stringValue());
    }
    std::string result;
    for (const auto& remark : remarks) { if (!result.empty()) result += " / "; result += remark; }
    return result;
}
void removeGpioFavorite(const JsonValue& pin, JsonValue& favorites)
{
    for (const auto& key : items(pin, "preference_keys")) favorites.objectItems().erase(key.stringValue());
}
std::vector<JsonValue> unifiedGpioInventory(const JsonValue& nativeInventory,
                                          const JsonValue& adaptedInventory,
                                          const std::vector<std::string>& capabilities)
{
    const auto supports = [&](const std::string& id) {
        return std::find(capabilities.begin(), capabilities.end(), id) != capabilities.end();
    };
    const auto& endpoints = items(adaptedInventory, "endpoints");
    std::set<std::size_t> linked;
    std::vector<JsonValue> result;
    auto normalize = [&](JsonValue pin, const std::vector<const JsonValue*>& matches, bool native) {
        auto& fields = pin.objectItems();
        for (const auto* action : {"read", "output", "pulse", "monitor"}) {
            fields.erase(std::string(action) + "_operation");
            fields.erase(std::string(action) + "_arguments");
        }
        fields.erase("output_persistent");
        const auto chip = jsonString(pin, "chip");
        if (native) {
            std::string controller = chip;
            for (const auto& c : items(nativeInventory, "chips")) if (jsonString(c, "id") == chip)
                controller = jsonString(c, "label") + "/" + jsonString(c, "name");
            fields["controller_identity"] = JsonValue::string(controller);
            fields["view_key"] = JsonValue::string(controller + ":" + std::to_string(jsonInteger(pin, "offset")));
        } else fields["view_key"] = JsonValue::string(jsonString(pin, "preference_key", "board:" + jsonString(pin, "endpoint_id")));
        auto& keys = fields["preference_keys"] = JsonValue::array(); addUnique(keys, gpioPreferenceKey(pin));
        auto& aliases = fields["aliases"] = JsonValue::array(); addUnique(aliases, jsonString(pin, "name"));
        JsonValue::Array details;
        for (const auto* endpoint : matches) {
            addUnique(keys, jsonString(*endpoint, "preference_key", "board:" + jsonString(*endpoint, "endpoint_id")));
            addUnique(aliases, jsonString(*endpoint, "name")); details.push_back(*endpoint);
        }
        fields["access_details"] = JsonValue::array(std::move(details));
        fields["reason"] = JsonValue::string(native && jsonBoolean(pin, "used") ? "引脚已被占用，未发现可用的控制接口" : "当前服务未提供此操作");
        const auto setOperation = [&](const char* action, const std::string& capability, const JsonValue& arguments) {
            if (!supports(capability)) return;
            fields[std::string(action) + "_operation"] = JsonValue::string(capability);
            fields[std::string(action) + "_arguments"] = arguments;
        };
        if (native && !jsonBoolean(pin, "used")) {
            const auto args = JsonValue::object({{"chip", JsonValue::string(chip)}, {"offset", JsonValue::integer(jsonInteger(pin, "offset"))}});
            setOperation("read", "gpio.pin.read", args); setOperation("output", "gpio.pin.write", args);
            setOperation("pulse", "gpio.pin.pulse", args); setOperation("monitor", "gpio.edge.monitor", args);
        } else if (matches.size() == 1) {
            const auto& endpoint = *matches.front();
            const auto args = JsonValue::object({{"endpoint_id", JsonValue::string(jsonString(endpoint, "endpoint_id"))}, {"adapter", JsonValue::string(jsonString(endpoint, "adapter"))}});
            if (validToolToken(jsonString(endpoint, "endpoint_id"))) {
                if (jsonBoolean(endpoint, "readable")) setOperation("read", "gpio.board.read", args);
                if (jsonBoolean(endpoint, "writable")) setOperation("output", "gpio.board.set", args);
            }
            auto reason = jsonString(endpoint, "reason");
            if (reason.empty()) reason = "当前服务或控制接口未提供该操作";
            fields["reason"] = JsonValue::string(reason);
            fields["output_persistent"] = JsonValue::boolean(true);
        } else if (matches.size() > 1) fields["reason"] = JsonValue::string("存在多个控制接口映射，尚未确认访问方式，不能操作");
        result.push_back(std::move(pin));
    };
    for (const auto& pin : items(nativeInventory, "pins")) {
        std::vector<const JsonValue*> matches;
        for (std::size_t i = 0; i < endpoints.size(); ++i) {
            const auto& endpoint = endpoints[i];
            if (!jsonString(endpoint, "chip").empty() && jsonString(endpoint, "chip") == jsonString(pin, "chip") &&
                jsonInteger(endpoint, "offset", -1) == jsonInteger(pin, "offset", -2)) {
                matches.push_back(&endpoint); linked.insert(i);
            }
        }
        normalize(pin, matches, true);
    }
    for (std::size_t i = 0; i < endpoints.size(); ++i) if (!linked.count(i)) normalize(endpoints[i], {&endpoints[i]}, false);
    return result;
}
} // namespace testtools
