#include "application/serial/at_command_library.h"

#include "core/protocol/json_value.h"
#include "core/serial/codec.h"

#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace testtools {
namespace {

constexpr std::size_t MaximumCommands = 64;
constexpr std::size_t MaximumNameBytes = 80;
constexpr std::size_t MaximumCommandBytes = 1024;
constexpr std::size_t MaximumFileBytes = 128 * 1024;

std::vector<AtCommand> defaultCommands()
{
    return {{"\u68c0\u67e5\u54cd\u5e94", "AT"},
            {"\u6a21\u5757\u4fe1\u606f", "ATI"},
            {"\u4fe1\u53f7\u8d28\u91cf", "AT+CSQ"},
            {"SIM \u5361\u72b6\u6001", "AT+CPIN?"},
            {"\u7f51\u7edc\u6ce8\u518c", "AT+CREG?"}};
}

const char* lineEndingName(AtCommandLineEnding ending)
{
    switch (ending) {
    case AtCommandLineEnding::Lf:
        return "lf";
    case AtCommandLineEnding::Cr:
        return "cr";
    case AtCommandLineEnding::CrLf:
        return "crlf";
    default:
        return "none";
    }
}

bool parseLineEnding(const std::string& value, AtCommandLineEnding& ending)
{
    if (value == "none")
        ending = AtCommandLineEnding::None;
    else if (value == "lf")
        ending = AtCommandLineEnding::Lf;
    else if (value == "cr")
        ending = AtCommandLineEnding::Cr;
    else if (value == "crlf")
        ending = AtCommandLineEnding::CrLf;
    else
        return false;
    return true;
}

bool exactFields(const JsonValue& value,
                 std::initializer_list<const char*> fields)
{
    if (!value.isObject() || value.objectItems().size() != fields.size())
        return false;
    std::set<std::string> allowed;
    for (const auto* field : fields)
        allowed.emplace(field);
    for (const auto& [name, ignored] : value.objectItems()) {
        (void)ignored;
        if (allowed.find(name) == allowed.end())
            return false;
    }
    return true;
}

bool replaceFile(const std::filesystem::path& temporary,
                 const std::filesystem::path& destination,
                 std::string& error)
{
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(),
                     destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "\u65e0\u6cd5\u66ff\u6362 AT \u6307\u4ee4\u6587\u4ef6";
        return false;
    }
#else
    std::error_code ec;
    std::filesystem::rename(temporary, destination, ec);
    if (ec) {
        error = "\u65e0\u6cd5\u66ff\u6362 AT \u6307\u4ee4\u6587\u4ef6: " + ec.message();
        return false;
    }
#endif
    return true;
}

} // namespace

AtCommandLibrary::AtCommandLibrary() : commands_(defaultCommands()) {}

const std::vector<AtCommand>& AtCommandLibrary::commands() const noexcept
{
    return commands_;
}

AtCommandLineEnding AtCommandLibrary::lineEnding() const noexcept
{
    return lineEnding_;
}

void AtCommandLibrary::setLineEnding(AtCommandLineEnding ending) noexcept
{
    lineEnding_ = ending;
}

bool AtCommandLibrary::valid(const AtCommand& command,
                             std::size_t ignoredIndex,
                             std::string& error) const
{
    if (command.name.empty() || command.name.size() > MaximumNameBytes) {
        error = "\u6307\u4ee4\u540d\u79f0\u5e94\u4e3a 1-80 \u5b57\u8282";
        return false;
    }
    if (command.command.empty() || command.command.size() > MaximumCommandBytes) {
        error = "\u6307\u4ee4\u5185\u5bb9\u5e94\u4e3a 1-1024 \u5b57\u8282";
        return false;
    }
    if (command.name.find('\r') != std::string::npos ||
        command.name.find('\n') != std::string::npos ||
        command.command.find('\r') != std::string::npos ||
        command.command.find('\n') != std::string::npos) {
        error = "\u540d\u79f0\u548c\u6307\u4ee4\u5185\u5bb9\u4e0d\u80fd\u5305\u542b\u6362\u884c\uff0c\u8bf7\u5728\u4e3b\u9875\u9009\u62e9\u884c\u5c3e";
        return false;
    }
    if (command.hex) {
        serialdemo::ByteBuffer parsed;
        std::string hexError;
        if (!serialdemo::parseHex(command.command, parsed, hexError) || parsed.empty()) {
            error = hexError.empty() ? "HEX \u6307\u4ee4\u4e0d\u80fd\u4e3a\u7a7a" : hexError;
            return false;
        }
    }
    for (std::size_t index = 0; index < commands_.size(); ++index) {
        if (index != ignoredIndex && commands_[index].name == command.name) {
            error = "\u5df2\u6709\u540c\u540d\u5feb\u6377\u6307\u4ee4";
            return false;
        }
    }
    return true;
}

bool AtCommandLibrary::replace(std::vector<AtCommand> commands, std::string& error)
{
    if (commands.size() > MaximumCommands) {
        error = "AT \u6307\u4ee4\u6700\u591a\u4fdd\u5b58 64 \u6761";
        return false;
    }
    AtCommandLibrary candidate;
    candidate.commands_.clear();
    for (auto& command : commands) {
        if (!candidate.add(std::move(command), error))
            return false;
    }
    commands_ = std::move(candidate.commands_);
    return true;
}

bool AtCommandLibrary::add(AtCommand command, std::string& error)
{
    if (commands_.size() >= MaximumCommands) {
        error = "AT \u6307\u4ee4\u6700\u591a\u4fdd\u5b58 64 \u6761";
        return false;
    }
    if (!valid(command, std::numeric_limits<std::size_t>::max(), error))
        return false;
    if (command.hex) {
        serialdemo::ByteBuffer parsed;
        std::string ignored;
        if (serialdemo::parseHex(command.command, parsed, ignored)) {
            command.command = serialdemo::toHex(parsed);
        }
    }
    commands_.push_back(std::move(command));
    return true;
}

bool AtCommandLibrary::update(std::size_t index, AtCommand command, std::string& error)
{
    if (index >= commands_.size()) {
        error = "\u8981\u4fee\u6539\u7684 AT \u6307\u4ee4\u4e0d\u5b58\u5728";
        return false;
    }
    if (!valid(command, index, error))
        return false;
    if (command.hex) {
        serialdemo::ByteBuffer parsed;
        std::string ignored;
        if (serialdemo::parseHex(command.command, parsed, ignored)) {
            command.command = serialdemo::toHex(parsed);
        }
    }
    commands_[index] = std::move(command);
    return true;
}

bool AtCommandLibrary::remove(std::size_t index)
{
    if (index >= commands_.size())
        return false;
    commands_.erase(commands_.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

bool AtCommandStore::load(const std::filesystem::path& path,
                          AtCommandLibrary& library,
                          std::string& error)
{
    error.clear();
    std::error_code fileError;
    if (!std::filesystem::exists(path, fileError))
        return !fileError;
    const auto size = std::filesystem::file_size(path, fileError);
    if (fileError || size > MaximumFileBytes) {
        error = "AT \u6307\u4ee4\u6587\u4ef6\u8fc7\u5927\u6216\u65e0\u6cd5\u8bfb\u53d6";
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "\u65e0\u6cd5\u6253\u5f00 AT \u6307\u4ee4\u6587\u4ef6";
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    JsonValue root;
    JsonLimits limits;
    limits.max_input_bytes = MaximumFileBytes;
    limits.max_array_items = MaximumCommands;
    std::string parseError;
    if (!parseJson(text, root, parseError, limits) || !root.isObject()) {
        error = "AT \u6307\u4ee4\u6587\u4ef6\u683c\u5f0f\u9519\u8bef";
        return false;
    }
    const auto* schema = root.find("schema_version");
    const auto* commandsValue = root.find("commands");
    if (!schema || !schema->isInt64() || !commandsValue || !commandsValue->isArray()) {
        error = "AT \u6307\u4ee4\u6587\u4ef6\u7248\u672c\u6216\u7ed3\u6784\u4e0d\u53d7\u652f\u6301";
        return false;
    }
    const auto schemaVersion = schema->int64Value();
    AtCommandLineEnding globalEnding = AtCommandLineEnding::CrLf;
    if (schemaVersion == 1) {
        if (!exactFields(root, {"schema_version", "commands"})) {
            error = "AT \u6307\u4ee4\u6587\u4ef6\u683c\u5f0f\u9519\u8bef";
            return false;
        }
    } else if (schemaVersion == 2) {
        const auto* ending = root.find("line_ending");
        if (!exactFields(root, {"schema_version", "line_ending", "commands"}) ||
            !ending || !ending->isString() ||
            !parseLineEnding(ending->stringValue(), globalEnding)) {
            error = "AT \u6307\u4ee4\u6587\u4ef6\u884c\u5c3e\u8bbe\u7f6e\u9519\u8bef";
            return false;
        }
    } else {
        error = "AT \u6307\u4ee4\u6587\u4ef6\u7248\u672c\u6216\u7ed3\u6784\u4e0d\u53d7\u652f\u6301";
        return false;
    }
    std::vector<AtCommand> commands;
    commands.reserve(commandsValue->arrayItems().size());
    for (const auto& value : commandsValue->arrayItems()) {
        const bool validFields =
            schemaVersion == 1
                ? exactFields(value, {"name", "command", "line_ending"})
                : (exactFields(value, {"name", "command"}) ||
                   exactFields(value, {"name", "command", "hex"}));
        if (!validFields) {
            error = "AT \u6307\u4ee4\u9879\u5b57\u6bb5\u9519\u8bef";
            return false;
        }
        const auto* name = value.find("name");
        const auto* command = value.find("command");
        const auto* hexValue = value.find("hex");
        AtCommand current;
        if (!name || !name->isString() || !command || !command->isString() ||
            (hexValue && !hexValue->isBool())) {
            error = "AT \u6307\u4ee4\u9879\u5185\u5bb9\u9519\u8bef";
            return false;
        }
        if (schemaVersion == 1) {
            const auto* ending = value.find("line_ending");
            AtCommandLineEnding ignoredEnding = AtCommandLineEnding::CrLf;
            if (!ending || !ending->isString() ||
                !parseLineEnding(ending->stringValue(), ignoredEnding)) {
                error = "AT \u6307\u4ee4\u9879\u5185\u5bb9\u9519\u8bef";
                return false;
            }
        }
        current.name = name->stringValue();
        current.command = command->stringValue();
        current.hex = hexValue && hexValue->boolValue();
        commands.push_back(std::move(current));
    }
    if (!library.replace(std::move(commands), error))
        return false;
    library.setLineEnding(globalEnding);
    return true;
}

bool AtCommandStore::importCommands(const std::filesystem::path& path,
                                    AtCommandLibrary& library, std::string& error)
{
    std::error_code fileError;
    if (!std::filesystem::is_regular_file(path, fileError)) {
        error = "指令文件不存在或无法读取";
        return false;
    }
    AtCommandLibrary imported;
    if (!load(path, imported, error)) {
        return false;
    }
    // Sharing commands must not change the active serial channel's send semantics.
    imported.setLineEnding(library.lineEnding());
    library = std::move(imported);
    return true;
}

bool AtCommandStore::save(const std::filesystem::path& path,
                          const AtCommandLibrary& library,
                          std::string& error)
{
    error.clear();
    JsonValue::Array values;
    values.reserve(library.commands().size());
    for (const auto& command : library.commands()) {
        if (command.hex) {
            values.emplace_back(JsonValue::object({
                {"command", JsonValue::string(command.command)},
                {"hex", JsonValue::boolean(true)},
                {"name", JsonValue::string(command.name)},
            }));
        } else {
            values.emplace_back(JsonValue::object({
                {"command", JsonValue::string(command.command)},
                {"name", JsonValue::string(command.name)},
            }));
        }
    }
    const auto root = JsonValue::object({
        {"commands", JsonValue::array(std::move(values))},
        {"line_ending", JsonValue::string(lineEndingName(library.lineEnding()))},
        {"schema_version", JsonValue::integer(2)},
    });
    JsonLimits limits;
    limits.max_output_bytes = MaximumFileBytes;
    JsonError jsonError;
    std::string text;
    if (!serializeJson(root, text, jsonError, limits)) {
        error = "\u65e0\u6cd5\u751f\u6210 AT \u6307\u4ee4\u6587\u4ef6";
        return false;
    }
    std::error_code directoryError;
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path(), directoryError);
    if (directoryError) {
        error = "\u65e0\u6cd5\u521b\u5efa AT \u6307\u4ee4\u5b58\u50a8\u76ee\u5f55";
        return false;
    }
    const auto temporary = path.wstring() + L".tmp";
    {
        std::ofstream output(std::filesystem::path(temporary),
                             std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "\u65e0\u6cd5\u5199\u5165 AT \u6307\u4ee4\u6587\u4ef6";
            return false;
        }
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.flush();
        if (!output) {
            error = "\u5199\u5165 AT \u6307\u4ee4\u6587\u4ef6\u5931\u8d25";
            return false;
        }
    }
    if (!replaceFile(std::filesystem::path(temporary), path, error)) {
        std::error_code cleanupError;
        std::filesystem::remove(std::filesystem::path(temporary), cleanupError);
        return false;
    }
    return true;
}

SerialWorkspacePreferences::SerialWorkspacePreferences()
{
    channels[0].useAgent = false;
    channels[0].lineEnding = 0;
    channels[1].useAgent = false;
    channels[1].lineEnding = 3;
}

bool SerialPreferencesStore::load(const std::filesystem::path& path,
                                  SerialWorkspacePreferences& preferences,
                                  std::string& error)
{
    error.clear();
    std::error_code fileError;
    if (!std::filesystem::exists(path, fileError)) {
        return !fileError;
    }
    const auto size = std::filesystem::file_size(path, fileError);
    if (fileError || size > MaximumFileBytes) {
        error = "\u4e32\u53e3\u914d\u7f6e\u6587\u4ef6\u8fc7\u5927\u6216\u65e0\u6cd5\u8bfb\u53d6";
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "\u65e0\u6cd5\u6253\u5f00\u4e32\u53e3\u914d\u7f6e\u6587\u4ef6";
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    JsonValue root;
    JsonLimits limits;
    limits.max_input_bytes = MaximumFileBytes;
    std::string parseError;
    if (!parseJson(text, root, parseError, limits) || !root.isObject()) {
        error = "\u4e32\u53e3\u914d\u7f6e\u6587\u4ef6\u683c\u5f0f\u9519\u8bef";
        return false;
    }
    const auto* schema = root.find("schema_version");
    const auto* channelsValue = root.find("channels");
    const auto* shortcutVisible = root.find("shortcut_bar_visible");
    if (!schema || !schema->isInt64() || schema->int64Value() != 1 ||
        !channelsValue || !channelsValue->isArray() ||
        channelsValue->arrayItems().size() != 2 ||
        (shortcutVisible && !shortcutVisible->isBool())) {
        error = "\u4e32\u53e3\u914d\u7f6e\u6587\u4ef6\u7248\u672c\u6216\u7ed3\u6784\u4e0d\u53d7\u652f\u6301";
        return false;
    }

    SerialWorkspacePreferences candidate;
    if (const auto* ending = root.find("terminal_line_ending"); ending && ending->isInt64()
        && ending->int64Value() >= 1 && ending->int64Value() <= 3) {
        candidate.terminalLineEnding = static_cast<int>(ending->int64Value());
    }
    if (shortcutVisible) {
        candidate.shortcutBarVisible = shortcutVisible->boolValue();
    }
    if (const auto* key = root.find("terminal_backspace"); key && key->isInt64()
        && (key->int64Value() == 8 || key->int64Value() == 127)) {
        candidate.terminalBackspace = static_cast<int>(key->int64Value());
    }
    for (std::size_t i = 0; i < 2; ++i) {
        const auto& item = channelsValue->arrayItems()[i];
        if (!item.isObject()) {
            error = "\u4e32\u53e3\u914d\u7f6e\u9879\u683c\u5f0f\u9519\u8bef";
            return false;
        }
        auto& dst = candidate.channels[i];
        if (const auto* v = item.find("use_agent"); v && v->isBool())
            dst.useAgent = v->boolValue();
        if (const auto* v = item.find("endpoint"); v && v->isString() && v->stringValue().size() <= 256)
            dst.endpoint = v->stringValue();
        if (const auto* v = item.find("baud_rate"); v && v->isInt64() && v->int64Value() > 0 && v->int64Value() <= 10000000)
            dst.baudRate = static_cast<int>(v->int64Value());
        if (const auto* v = item.find("data_bits"); v && v->isInt64() && v->int64Value() >= 5 && v->int64Value() <= 8)
            dst.dataBits = static_cast<int>(v->int64Value());
        if (const auto* v = item.find("parity"); v && v->isInt64() && v->int64Value() >= 0 && v->int64Value() <= 2)
            dst.parity = static_cast<int>(v->int64Value());
        if (const auto* v = item.find("stop_bits"); v && v->isInt64() && v->int64Value() >= 0 && v->int64Value() <= 1)
            dst.stopBits = static_cast<int>(v->int64Value());
        if (const auto* v = item.find("flow_control"); v && v->isInt64() && v->int64Value() >= 0 && v->int64Value() <= 2)
            dst.flowControl = static_cast<int>(v->int64Value());
        if (const auto* v = item.find("line_ending"); v && v->isInt64() && v->int64Value() >= 0 && v->int64Value() <= 3)
            dst.lineEnding = static_cast<int>(v->int64Value());
        if (const auto* v = item.find("receive_hex"); v && v->isBool())
            dst.receiveHex = v->boolValue();
        if (const auto* v = item.find("timestamp"); v && v->isBool())
            dst.timestamp = v->boolValue();
        if (const auto* v = item.find("auto_scroll"); v && v->isBool())
            dst.autoScroll = v->boolValue();
        if (const auto* v = item.find("send_hex"); v && v->isBool())
            dst.sendHex = v->boolValue();
    }
    preferences = std::move(candidate);
    return true;
}

bool SerialPreferencesStore::save(const std::filesystem::path& path,
                                  const SerialWorkspacePreferences& preferences,
                                  std::string& error)
{
    error.clear();
    JsonValue::Array channelsArray;
    channelsArray.reserve(2);
    for (const auto& ch : preferences.channels) {
        channelsArray.emplace_back(JsonValue::object({
            {"auto_scroll", JsonValue::boolean(ch.autoScroll)},
            {"baud_rate", JsonValue::integer(ch.baudRate)},
            {"data_bits", JsonValue::integer(ch.dataBits)},
            {"endpoint", JsonValue::string(ch.endpoint)},
            {"flow_control", JsonValue::integer(ch.flowControl)},
            {"line_ending", JsonValue::integer(ch.lineEnding)},
            {"parity", JsonValue::integer(ch.parity)},
            {"receive_hex", JsonValue::boolean(ch.receiveHex)},
            {"send_hex", JsonValue::boolean(ch.sendHex)},
            {"stop_bits", JsonValue::integer(ch.stopBits)},
            {"timestamp", JsonValue::boolean(ch.timestamp)},
            {"use_agent", JsonValue::boolean(ch.useAgent)},
        }));
    }
    const auto root = JsonValue::object({
        {"channels", JsonValue::array(std::move(channelsArray))},
        {"schema_version", JsonValue::integer(1)},
        {"shortcut_bar_visible", JsonValue::boolean(preferences.shortcutBarVisible)},
        {"terminal_line_ending", JsonValue::integer(preferences.terminalLineEnding)},
        {"terminal_backspace", JsonValue::integer(preferences.terminalBackspace)},
    });
    JsonLimits limits;
    limits.max_output_bytes = MaximumFileBytes;
    JsonError jsonError;
    std::string text;
    if (!serializeJson(root, text, jsonError, limits)) {
        error = "\u65e0\u6cd5\u751f\u6210\u4e32\u53e3\u914d\u7f6e\u6587\u4ef6";
        return false;
    }
    std::error_code directoryError;
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path(), directoryError);
    if (directoryError) {
        error = "\u65e0\u6cd5\u521b\u5efa\u4e32\u53e3\u914d\u7f6e\u76ee\u5f55";
        return false;
    }
    const auto temporary = path.wstring() + L".tmp";
    {
        std::ofstream output(std::filesystem::path(temporary),
                             std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "\u65e0\u6cd5\u5199\u5165\u4e32\u53e3\u914d\u7f6e\u6587\u4ef6";
            return false;
        }
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.flush();
        if (!output) {
            error = "\u5199\u5165\u4e32\u53e3\u914d\u7f6e\u6587\u4ef6\u5931\u8d25";
            return false;
        }
    }
    if (!replaceFile(std::filesystem::path(temporary), path, error)) {
        std::error_code cleanupError;
        std::filesystem::remove(std::filesystem::path(temporary), cleanupError);
        return false;
    }
    return true;
}

} // namespace testtools
