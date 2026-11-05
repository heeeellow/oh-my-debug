#pragma once
#include "core/toolbox/toolbox_contract.h"

namespace testtools {
// One physical resource, with access operations selected from verified inventory.
std::vector<JsonValue> unifiedGpioInventory(const JsonValue& nativeInventory,
                                          const JsonValue& adaptedInventory,
                                          const std::vector<std::string>& capabilities);
std::string gpioPreferenceKey(const JsonValue& pin);
std::string gpioRemark(const JsonValue& pin, const JsonValue& favorites);
void removeGpioFavorite(const JsonValue& pin, JsonValue& favorites);
} // namespace testtools
