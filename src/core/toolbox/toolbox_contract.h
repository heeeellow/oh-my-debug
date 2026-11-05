#pragma once

#include "core/protocol/json_value.h"
#include <cstdint>
#include <string>
#include <vector>

namespace testtools {
struct RockchipGpioNumber { int bank = 0, offset = 0, number = 0; std::string name; };
bool convertRockchipGpio(const std::string& input, RockchipGpioNumber& result, std::string& error);
bool isToolCapability(const std::string& id);
bool validateToolArguments(const std::string& id, const JsonValue& arguments, std::string& error);
bool onlyFields(const JsonValue& value, const std::vector<std::string>& fields);
std::string jsonString(const JsonValue& value, const std::string& key, const std::string& fallback = {});
std::int64_t jsonInteger(const JsonValue& value, const std::string& key, std::int64_t fallback = 0);
bool jsonBoolean(const JsonValue& value, const std::string& key, bool fallback = false);
bool validToolToken(const std::string& value);
bool validInterfaceName(const std::string& value);
bool validIpv4(const std::string& value);
std::string makeToolToken();
} // namespace testtools
