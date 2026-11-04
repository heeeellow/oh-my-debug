#include "application/serial/serial_preferences.h"

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
constexpr std::size_t MaximumFileBytes = 128 * 1024;

bool replaceFile(const std::filesystem::path& temporary,
                 const std::filesystem::path& destination,
                 std::string& error)
{
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(),
                     destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "\u65e0\u6cd5\u66ff\u6362 \u4e32\u53e3\u8bbe\u7f6e\u6587\u4ef6";
        return false;
    }
#else
    std::error_code ec;
    std::filesystem::rename(temporary, destination, ec);
    if (ec) {
        error = "\u65e0\u6cd5\u66ff\u6362 \u4e32\u53e3\u8bbe\u7f6e\u6587\u4ef6: " + ec.message();
        return false;
    }
#endif
    return true;
}

} // namespace

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
    if (!schema || !schema->isInt64() || schema->int64Value() != 1 || !channelsValue ||
        !channelsValue->isArray() || channelsValue->arrayItems().size() != 2 ||
        (shortcutVisible && !shortcutVisible->isBool())) {
        error = "\u4e32\u53e3\u914d\u7f6e\u6587\u4ef6\u7248\u672c\u6216\u7ed3\u6784\u4e0d\u53d7"
                "\u652f\u6301";
        return false;
    }

    SerialWorkspacePreferences candidate;
    if (const auto* ending = root.find("terminal_line_ending");
        ending && ending->isInt64() && ending->int64Value() >= 1 && ending->int64Value() <= 3) {
        candidate.terminalLineEnding = static_cast<int>(ending->int64Value());
    }
    if (shortcutVisible) {
        candidate.shortcutBarVisible = shortcutVisible->boolValue();
    }
    if (const auto* width = root.find("shortcut_sidebar_width");
        width && width->isInt64() && width->int64Value() >= 220 && width->int64Value() <= 480) {
        candidate.shortcutSidebarWidth = static_cast<int>(width->int64Value());
    }
    if (const auto* key = root.find("terminal_backspace");
        key && key->isInt64() && (key->int64Value() == 8 || key->int64Value() == 127)) {
        candidate.terminalBackspace = static_cast<int>(key->int64Value());
    }
    if (const auto* v = root.find("terminal_smart_keys"); v && v->isBool())
        candidate.terminalSmartKeys = v->boolValue();
    if (const auto* v = root.find("terminal_copy_on_select"); v && v->isBool())
        candidate.terminalCopyOnSelect = v->boolValue();
    if (const auto* v = root.find("terminal_right_paste"); v && v->isBool())
        candidate.terminalRightPaste = v->boolValue();
    if (const auto* v = root.find("terminal_theme");
        v && v->isInt64() && v->int64Value() >= 0 && v->int64Value() <= 2)
        candidate.terminalTheme = int(v->int64Value());
    if (const auto* v = root.find("terminal_font_size");
        v && v->isInt64() && v->int64Value() >= 11 && v->int64Value() <= 18)
        candidate.terminalFontSize = int(v->int64Value());
    if (const auto* v = root.find("terminal_line_spacing");
        v && v->isInt64() && v->int64Value() >= 0 && v->int64Value() <= 6)
        candidate.terminalLineSpacing = int(v->int64Value());
    if (const auto* v = root.find("terminal_cursor_shape");
        v && v->isInt64() && v->int64Value() >= 0 && v->int64Value() <= 3)
        candidate.terminalCursorShape = int(v->int64Value());
    if (const auto* v = root.find("terminal_font");
        v && v->isString() && v->stringValue().size() <= 128)
        candidate.terminalFont = v->stringValue();
    // Upgrade only the exact original typography defaults, once. Explicit custom values remain.
    const auto* appearanceRevision = root.find("terminal_appearance_revision");
    if ((!appearanceRevision || !appearanceRevision->isInt64() ||
         appearanceRevision->int64Value() < 1) &&
        candidate.terminalFont == "Cascadia Mono" && candidate.terminalFontSize == 14 &&
        candidate.terminalLineSpacing == 2) {
        candidate.terminalFontSize = 15;
        candidate.terminalLineSpacing = 3;
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
        if (const auto* v = item.find("manual_endpoint"); v && v->isBool())
            dst.manualEndpoint = v->boolValue();
        if (const auto* v = item.find("endpoint");
            v && v->isString() && v->stringValue().size() <= 256)
            dst.endpoint = v->stringValue();
        if (const auto* v = item.find("baud_rate");
            v && v->isInt64() && v->int64Value() > 0 && v->int64Value() <= 10000000)
            dst.baudRate = static_cast<int>(v->int64Value());
        if (const auto* v = item.find("data_bits");
            v && v->isInt64() && v->int64Value() >= 5 && v->int64Value() <= 8)
            dst.dataBits = static_cast<int>(v->int64Value());
        if (const auto* v = item.find("parity");
            v && v->isInt64() && v->int64Value() >= 0 && v->int64Value() <= 2)
            dst.parity = static_cast<int>(v->int64Value());
        if (const auto* v = item.find("stop_bits");
            v && v->isInt64() && v->int64Value() >= 0 && v->int64Value() <= 1)
            dst.stopBits = static_cast<int>(v->int64Value());
        if (const auto* v = item.find("flow_control");
            v && v->isInt64() && v->int64Value() >= 0 && v->int64Value() <= 2)
            dst.flowControl = static_cast<int>(v->int64Value());
        if (const auto* v = item.find("line_ending");
            v && v->isInt64() && v->int64Value() >= 0 && v->int64Value() <= 3)
            dst.lineEnding = static_cast<int>(v->int64Value());
        if (const auto* v = item.find("receive_hex"); v && v->isBool())
            dst.receiveHex = v->boolValue();
        if (const auto* v = item.find("timestamp"); v && v->isBool())
            dst.timestamp = v->boolValue();
        if (const auto* v = item.find("auto_scroll"); v && v->isBool())
            dst.autoScroll = v->boolValue();
        if (const auto* v = item.find("send_hex"); v && v->isBool())
            dst.sendHex = v->boolValue();
        if (const auto* v = item.find("highlight_keywords"); v && v->isBool())
            dst.highlightKeywords = v->boolValue();
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
            {"manual_endpoint", JsonValue::boolean(ch.manualEndpoint)},
            {"flow_control", JsonValue::integer(ch.flowControl)},
            {"line_ending", JsonValue::integer(ch.lineEnding)},
            {"parity", JsonValue::integer(ch.parity)},
            {"receive_hex", JsonValue::boolean(ch.receiveHex)},
            {"send_hex", JsonValue::boolean(ch.sendHex)},
            {"highlight_keywords", JsonValue::boolean(ch.highlightKeywords)},
            {"stop_bits", JsonValue::integer(ch.stopBits)},
            {"timestamp", JsonValue::boolean(ch.timestamp)},
            {"use_agent", JsonValue::boolean(ch.useAgent)},
        }));
    }
    const auto root = JsonValue::object({
        {"channels", JsonValue::array(std::move(channelsArray))},
        {"schema_version", JsonValue::integer(1)},
        {"shortcut_bar_visible", JsonValue::boolean(preferences.shortcutBarVisible)},
        {"shortcut_sidebar_width", JsonValue::integer(preferences.shortcutSidebarWidth)},
        {"terminal_line_ending", JsonValue::integer(preferences.terminalLineEnding)},
        {"terminal_backspace", JsonValue::integer(preferences.terminalBackspace)},
        {"terminal_smart_keys", JsonValue::boolean(preferences.terminalSmartKeys)},
        {"terminal_copy_on_select", JsonValue::boolean(preferences.terminalCopyOnSelect)},
        {"terminal_right_paste", JsonValue::boolean(preferences.terminalRightPaste)},
        {"terminal_theme", JsonValue::integer(preferences.terminalTheme)},
        {"terminal_appearance_revision", JsonValue::integer(1)},
        {"terminal_font_size", JsonValue::integer(preferences.terminalFontSize)},
        {"terminal_line_spacing", JsonValue::integer(preferences.terminalLineSpacing)},
        {"terminal_cursor_shape", JsonValue::integer(preferences.terminalCursorShape)},
        {"terminal_font", JsonValue::string(preferences.terminalFont)},
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
        std::ofstream output(std::filesystem::path(temporary), std::ios::binary | std::ios::trunc);
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
