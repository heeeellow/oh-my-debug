#include "core/protocol/json_value.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace testtools {

namespace {

void setError(JsonError& error, JsonErrorCode code, std::size_t offset, const char* message)
{
    error.code = code;
    error.offset = offset;
    error.message = message;
}

bool validateLimits(const JsonLimits& limits, JsonError& error)
{
    if (limits.max_input_bytes > JsonLimits::HardMaxInputBytes) {
        setError(error,
                 JsonErrorCode::InputTooLarge,
                 0,
                 "JSON input limit exceeds the hard safety limit");
        return false;
    }
    if (limits.max_output_bytes > JsonLimits::HardMaxOutputBytes) {
        setError(error,
                 JsonErrorCode::OutputTooLarge,
                 0,
                 "JSON output limit exceeds the hard safety limit");
        return false;
    }
    if (limits.max_depth > JsonLimits::HardMaxDepth) {
        setError(
            error, JsonErrorCode::DepthLimit, 0, "JSON depth limit exceeds the hard safety limit");
        return false;
    }
    if (limits.max_string_bytes > JsonLimits::HardMaxStringBytes) {
        setError(error,
                 JsonErrorCode::StringLimit,
                 0,
                 "JSON string limit exceeds the hard safety limit");
        return false;
    }
    if (limits.max_array_items > JsonLimits::HardMaxArrayItems) {
        setError(
            error, JsonErrorCode::ArrayLimit, 0, "JSON array limit exceeds the hard safety limit");
        return false;
    }
    if (limits.max_object_members > JsonLimits::HardMaxObjectMembers) {
        setError(error,
                 JsonErrorCode::ObjectLimit,
                 0,
                 "JSON object member limit exceeds the hard safety limit");
        return false;
    }
    return true;
}

bool continuation(unsigned char byte)
{
    return (byte & 0xC0u) == 0x80u;
}

// 从 offset 处读取一个 UTF-8 标量字符。调用方持有字符串且可传入 ASCII 字节偏移；
// 拒绝过长编码（Overlong）、代理项码点（Surrogate）以及超过 U+10FFFF 的非法值。
bool readUtf8(const std::string& text,
              std::size_t offset,
              std::size_t& length,
              std::uint32_t& codepoint)
{
    if (offset >= text.size()) {
        return false;
    }
    const auto first = static_cast<unsigned char>(text[offset]);
    if (first <= 0x7F) {
        length = 1;
        codepoint = first;
        return true;
    }

    std::size_t expected = 0;
    std::uint32_t value = 0;
    std::uint32_t minimum = 0;
    if (first >= 0xC2 && first <= 0xDF) {
        expected = 2;
        value = first & 0x1Fu;
        minimum = 0x80;
    } else if (first >= 0xE0 && first <= 0xEF) {
        expected = 3;
        value = first & 0x0Fu;
        minimum = 0x800;
    } else if (first >= 0xF0 && first <= 0xF4) {
        expected = 4;
        value = first & 0x07u;
        minimum = 0x10000;
    } else {
        return false;
    }
    if (offset + expected > text.size()) {
        return false;
    }
    for (std::size_t i = 1; i < expected; ++i) {
        const auto byte = static_cast<unsigned char>(text[offset + i]);
        if (!continuation(byte)) {
            return false;
        }
        value = (value << 6) | (byte & 0x3Fu);
    }
    if (value < minimum || value > 0x10FFFFu || (value >= 0xD800u && value <= 0xDFFFu)) {
        return false;
    }
    length = expected;
    codepoint = value;
    return true;
}

bool appendUtf8(std::string& output, std::uint32_t codepoint, std::size_t maxBytes)
{
    char encoded[4] = {};
    std::size_t length = 0;
    if (codepoint <= 0x7Fu) {
        encoded[0] = static_cast<char>(codepoint);
        length = 1;
    } else if (codepoint <= 0x7FFu) {
        encoded[0] = static_cast<char>(0xC0u | (codepoint >> 6));
        encoded[1] = static_cast<char>(0x80u | (codepoint & 0x3Fu));
        length = 2;
    } else if (codepoint <= 0xFFFFu) {
        encoded[0] = static_cast<char>(0xE0u | (codepoint >> 12));
        encoded[1] = static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu));
        encoded[2] = static_cast<char>(0x80u | (codepoint & 0x3Fu));
        length = 3;
    } else if (codepoint <= 0x10FFFFu) {
        encoded[0] = static_cast<char>(0xF0u | (codepoint >> 18));
        encoded[1] = static_cast<char>(0x80u | ((codepoint >> 12) & 0x3Fu));
        encoded[2] = static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu));
        encoded[3] = static_cast<char>(0x80u | (codepoint & 0x3Fu));
        length = 4;
    } else {
        return false;
    }
    if (output.size() > maxBytes || length > maxBytes - output.size()) {
        return false;
    }
    output.append(encoded, length);
    return true;
}

class Parser {
  public:
    Parser(const std::string& input, const JsonLimits& limits, JsonError& error)
        : input_(input), limits_(limits), error_(error)
    {
    }

    bool parse(JsonValue& output)
    {
        if (input_.size() > limits_.max_input_bytes) {
            setError(error_,
                     JsonErrorCode::InputTooLarge,
                     limits_.max_input_bytes,
                     "JSON input exceeds the configured limit");
            return false;
        }
        if (!validateLimits(limits_, error_))
            return false;
        skipWhitespace();
        if (position_ == input_.size()) {
            setError(error_, JsonErrorCode::UnexpectedEnd, position_, "JSON input is empty");
            return false;
        }
        if (!parseValue(output, 0)) {
            return false;
        }
        skipWhitespace();
        if (position_ != input_.size()) {
            setError(error_,
                     JsonErrorCode::TrailingData,
                     position_,
                     "JSON input contains trailing data");
            return false;
        }
        return true;
    }

  private:
    void skipWhitespace()
    {
        while (position_ < input_.size()) {
            const char c = input_[position_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                break;
            }
            ++position_;
        }
    }

    bool parseValue(JsonValue& output, std::size_t depth)
    {
        skipWhitespace();
        if (position_ == input_.size()) {
            setError(error_, JsonErrorCode::UnexpectedEnd, position_, "JSON value is incomplete");
            return false;
        }
        const char c = input_[position_];
        switch (c) {
        case 'n':
            return parseLiteral(output, "null", JsonValue::Type::Null, false);
        case 't':
            return parseLiteral(output, "true", JsonValue::Type::Bool, true);
        case 'f':
            return parseLiteral(output, "false", JsonValue::Type::Bool, false);
        case '"': {
            std::string value;
            if (!parseString(value)) {
                return false;
            }
            output = JsonValue::string(std::move(value));
            return true;
        }
        case '[':
            if (depth >= limits_.max_depth) {
                setError(error_,
                         JsonErrorCode::DepthLimit,
                         position_,
                         "JSON nesting exceeds the configured limit");
                return false;
            }
            return parseArray(output, depth + 1);
        case '{':
            if (depth >= limits_.max_depth) {
                setError(error_,
                         JsonErrorCode::DepthLimit,
                         position_,
                         "JSON nesting exceeds the configured limit");
                return false;
            }
            return parseObject(output, depth + 1);
        case '-':
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
            return parseInteger(output);
        default:
            setError(error_,
                     JsonErrorCode::InvalidSyntax,
                     position_,
                     "JSON value has an invalid leading character");
            return false;
        }
    }

    bool parseLiteral(JsonValue& output, const char* literal, JsonValue::Type type, bool boolValue)
    {
        const std::size_t start = position_;
        for (const char* current = literal; *current != '\0'; ++current) {
            if (position_ == input_.size() || input_[position_] != *current) {
                setError(error_,
                         JsonErrorCode::InvalidSyntax,
                         start,
                         "JSON literal is invalid or incomplete");
                return false;
            }
            ++position_;
        }
        if (type == JsonValue::Type::Null) {
            output = JsonValue::null();
        } else {
            output = JsonValue::boolean(boolValue);
        }
        return true;
    }

    bool parseInteger(JsonValue& output)
    {
        const std::size_t start = position_;
        bool negative = false;
        if (input_[position_] == '-') {
            negative = true;
            ++position_;
            if (position_ == input_.size()) {
                setError(
                    error_, JsonErrorCode::UnexpectedEnd, position_, "JSON integer is incomplete");
                return false;
            }
        }

        if (input_[position_] == '0') {
            ++position_;
            if (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
                setError(error_,
                         JsonErrorCode::InvalidSyntax,
                         position_,
                         "JSON integer has a leading zero");
                return false;
            }
        } else if (input_[position_] >= '1' && input_[position_] <= '9') {
            while (position_ < input_.size() && input_[position_] >= '0' &&
                   input_[position_] <= '9') {
                ++position_;
            }
        } else {
            setError(error_, JsonErrorCode::InvalidSyntax, start, "JSON integer has no digits");
            return false;
        }

        if (position_ < input_.size() &&
            (input_[position_] == '.' || input_[position_] == 'e' || input_[position_] == 'E')) {
            setError(
                error_, JsonErrorCode::NonIntegerNumber, position_, "JSON number is not an int64");
            return false;
        }

        const std::uint64_t positiveLimit =
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        const std::uint64_t negativeLimit = positiveLimit + 1u;
        const std::uint64_t limit = negative ? negativeLimit : positiveLimit;
        std::uint64_t magnitude = 0;
        for (std::size_t i = start + (negative ? 1 : 0); i < position_; ++i) {
            const std::uint64_t digit = static_cast<unsigned char>(input_[i]) - '0';
            if (magnitude > (limit - digit) / 10u) {
                setError(error_,
                         JsonErrorCode::IntegerOverflow,
                         start,
                         "JSON integer does not fit in int64");
                return false;
            }
            magnitude = magnitude * 10u + digit;
        }
        const auto result =
            negative ? (magnitude == negativeLimit ? std::numeric_limits<std::int64_t>::min()
                                                   : -static_cast<std::int64_t>(magnitude))
                     : static_cast<std::int64_t>(magnitude);
        output = JsonValue::integer(result);
        return true;
    }

    bool parseHexQuad(std::uint32_t& value)
    {
        if (input_.size() - position_ < 4) {
            setError(error_,
                     JsonErrorCode::UnexpectedEnd,
                     position_,
                     "JSON unicode escape is incomplete");
            return false;
        }
        value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = input_[position_++];
            std::uint32_t digit = 0;
            if (c >= '0' && c <= '9') {
                digit = static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                digit = static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                digit = static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                setError(error_,
                         JsonErrorCode::InvalidEscape,
                         position_ - 1,
                         "JSON unicode escape contains a non-hex digit");
                return false;
            }
            value = (value << 4) | digit;
        }
        return true;
    }

    bool parseString(std::string& output)
    {
        const std::size_t start = position_;
        ++position_; // 跳过起始双引号
        output.clear();
        while (position_ < input_.size()) {
            const unsigned char byte = static_cast<unsigned char>(input_[position_++]);
            if (byte == '"') {
                return true;
            }
            if (byte < 0x20u) {
                setError(error_,
                         JsonErrorCode::InvalidSyntax,
                         position_ - 1,
                         "JSON string contains an unescaped control character");
                return false;
            }
            if (byte == '\\') {
                if (position_ == input_.size()) {
                    setError(error_,
                             JsonErrorCode::UnexpectedEnd,
                             position_,
                             "JSON escape is incomplete");
                    return false;
                }
                const char escape = input_[position_++];
                switch (escape) {
                case '"':
                    if (!appendUtf8(output, '"', limits_.max_string_bytes))
                        return stringLimit();
                    break;
                case '\\':
                    if (!appendUtf8(output, '\\', limits_.max_string_bytes))
                        return stringLimit();
                    break;
                case '/':
                    if (!appendUtf8(output, '/', limits_.max_string_bytes))
                        return stringLimit();
                    break;
                case 'b':
                    if (!appendUtf8(output, '\b', limits_.max_string_bytes))
                        return stringLimit();
                    break;
                case 'f':
                    if (!appendUtf8(output, '\f', limits_.max_string_bytes))
                        return stringLimit();
                    break;
                case 'n':
                    if (!appendUtf8(output, '\n', limits_.max_string_bytes))
                        return stringLimit();
                    break;
                case 'r':
                    if (!appendUtf8(output, '\r', limits_.max_string_bytes))
                        return stringLimit();
                    break;
                case 't':
                    if (!appendUtf8(output, '\t', limits_.max_string_bytes))
                        return stringLimit();
                    break;
                case 'u': {
                    std::uint32_t unit = 0;
                    if (!parseHexQuad(unit))
                        return false;
                    std::uint32_t codepoint = unit;
                    if (unit >= 0xD800u && unit <= 0xDBFFu) {
                        if (input_.size() - position_ < 6 || input_[position_] != '\\' ||
                            input_[position_ + 1] != 'u') {
                            setError(error_,
                                     JsonErrorCode::InvalidEscape,
                                     position_,
                                     "JSON high surrogate is not followed by a low surrogate");
                            return false;
                        }
                        position_ += 2;
                        std::uint32_t low = 0;
                        if (!parseHexQuad(low) || low < 0xDC00u || low > 0xDFFFu) {
                            if (error_.code == JsonErrorCode::None) {
                                setError(error_,
                                         JsonErrorCode::InvalidEscape,
                                         position_,
                                         "JSON unicode surrogate pair is invalid");
                            }
                            return false;
                        }
                        codepoint = 0x10000u + ((unit - 0xD800u) << 10) + (low - 0xDC00u);
                    } else if (unit >= 0xDC00u && unit <= 0xDFFFu) {
                        setError(error_,
                                 JsonErrorCode::InvalidEscape,
                                 position_ - 4,
                                 "JSON low surrogate has no high surrogate");
                        return false;
                    }
                    if (!appendUtf8(output, codepoint, limits_.max_string_bytes)) {
                        return stringLimit();
                    }
                    break;
                }
                default:
                    setError(error_,
                             JsonErrorCode::InvalidEscape,
                             position_ - 1,
                             "JSON string uses an unsupported escape");
                    return false;
                }
                continue;
            }
            if (byte < 0x80u) {
                if (output.size() >= limits_.max_string_bytes)
                    return stringLimit();
                output.push_back(static_cast<char>(byte));
                continue;
            }

            const std::size_t sequenceStart = position_ - 1;
            std::size_t sequenceLength = 0;
            std::uint32_t codepoint = 0;
            if (!readUtf8(input_, sequenceStart, sequenceLength, codepoint)) {
                setError(error_,
                         JsonErrorCode::InvalidUtf8,
                         sequenceStart,
                         "JSON string contains invalid UTF-8");
                return false;
            }
            if (output.size() > limits_.max_string_bytes ||
                sequenceLength > limits_.max_string_bytes - output.size()) {
                return stringLimit();
            }
            output.append(input_, sequenceStart, sequenceLength);
            position_ = sequenceStart + sequenceLength;
        }
        setError(error_, JsonErrorCode::UnexpectedEnd, start, "JSON string is unterminated");
        return false;
    }

    bool stringLimit()
    {
        setError(error_,
                 JsonErrorCode::StringLimit,
                 position_,
                 "JSON string exceeds the configured limit");
        return false;
    }

    bool parseArray(JsonValue& output, std::size_t depth)
    {
        ++position_; // 跳过起始左方括号
        JsonValue::Array items;
        skipWhitespace();
        if (position_ < input_.size() && input_[position_] == ']') {
            ++position_;
            output = JsonValue::array(std::move(items));
            return true;
        }
        while (true) {
            if (items.size() >= limits_.max_array_items) {
                setError(error_,
                         JsonErrorCode::ArrayLimit,
                         position_,
                         "JSON array exceeds the configured limit");
                return false;
            }
            JsonValue item;
            if (!parseValue(item, depth))
                return false;
            items.push_back(std::move(item));
            skipWhitespace();
            if (position_ == input_.size()) {
                setError(
                    error_, JsonErrorCode::UnexpectedEnd, position_, "JSON array is unterminated");
                return false;
            }
            if (input_[position_] == ']') {
                ++position_;
                output = JsonValue::array(std::move(items));
                return true;
            }
            if (input_[position_] != ',') {
                setError(error_,
                         JsonErrorCode::InvalidSyntax,
                         position_,
                         "JSON array separator expected");
                return false;
            }
            ++position_;
            skipWhitespace();
            if (position_ < input_.size() && input_[position_] == ']') {
                setError(error_,
                         JsonErrorCode::InvalidSyntax,
                         position_,
                         "JSON array has a trailing comma");
                return false;
            }
        }
    }

    bool parseObject(JsonValue& output, std::size_t depth)
    {
        ++position_; // 跳过起始左花括号
        JsonValue::Object members;
        skipWhitespace();
        if (position_ < input_.size() && input_[position_] == '}') {
            ++position_;
            output = JsonValue::object(std::move(members));
            return true;
        }
        while (true) {
            if (members.size() >= limits_.max_object_members) {
                setError(error_,
                         JsonErrorCode::ObjectLimit,
                         position_,
                         "JSON object exceeds the configured member limit");
                return false;
            }
            if (position_ == input_.size() || input_[position_] != '"') {
                setError(error_,
                         JsonErrorCode::InvalidSyntax,
                         position_,
                         "JSON object member key expected");
                return false;
            }
            std::string key;
            if (!parseString(key))
                return false;
            if (members.find(key) != members.end()) {
                setError(error_,
                         JsonErrorCode::DuplicateObjectKey,
                         position_,
                         "JSON object contains a duplicate key");
                return false;
            }
            skipWhitespace();
            if (position_ == input_.size() || input_[position_] != ':') {
                setError(
                    error_, JsonErrorCode::InvalidSyntax, position_, "JSON object colon expected");
                return false;
            }
            ++position_;
            JsonValue member;
            if (!parseValue(member, depth))
                return false;
            members.emplace(std::move(key), std::move(member));
            skipWhitespace();
            if (position_ == input_.size()) {
                setError(
                    error_, JsonErrorCode::UnexpectedEnd, position_, "JSON object is unterminated");
                return false;
            }
            if (input_[position_] == '}') {
                ++position_;
                output = JsonValue::object(std::move(members));
                return true;
            }
            if (input_[position_] != ',') {
                setError(error_,
                         JsonErrorCode::InvalidSyntax,
                         position_,
                         "JSON object separator expected");
                return false;
            }
            ++position_;
            skipWhitespace();
            if (position_ < input_.size() && input_[position_] == '}') {
                setError(error_,
                         JsonErrorCode::InvalidSyntax,
                         position_,
                         "JSON object has a trailing comma");
                return false;
            }
        }
    }

    const std::string& input_;
    const JsonLimits& limits_;
    JsonError& error_;
    std::size_t position_ = 0;
};

class Serializer {
  public:
    Serializer(const JsonLimits& limits, JsonError& error) : limits_(limits), error_(error) {}

    bool run(const JsonValue& value, std::string& output)
    {
        if (!validateLimits(limits_, error_))
            return false;
        output.clear();
        if (!writeValue(value, 0, output)) {
            output.clear();
            return false;
        }
        return true;
    }

  private:
    bool add(std::string& output, const char* text, std::size_t length)
    {
        if (output.size() > limits_.max_output_bytes ||
            length > limits_.max_output_bytes - output.size()) {
            setError(error_,
                     JsonErrorCode::OutputTooLarge,
                     output.size(),
                     "JSON output exceeds the configured limit");
            return false;
        }
        output.append(text, length);
        return true;
    }

    bool add(std::string& output, char c)
    {
        return add(output, &c, 1);
    }

    bool writeValue(const JsonValue& value, std::size_t depth, std::string& output)
    {
        switch (value.type()) {
        case JsonValue::Type::Null:
            return add(output, "null", 4);
        case JsonValue::Type::Bool:
            return value.boolValue() ? add(output, "true", 4) : add(output, "false", 5);
        case JsonValue::Type::Int64: {
            const std::string number = std::to_string(value.int64Value());
            return add(output, number.data(), number.size());
        }
        case JsonValue::Type::String:
            return writeString(value.stringValue(), output);
        case JsonValue::Type::Array:
            if (depth >= limits_.max_depth) {
                setError(error_,
                         JsonErrorCode::DepthLimit,
                         output.size(),
                         "JSON nesting exceeds the configured limit");
                return false;
            }
            return writeArray(value.arrayItems(), depth + 1, output);
        case JsonValue::Type::Object:
            if (depth >= limits_.max_depth) {
                setError(error_,
                         JsonErrorCode::DepthLimit,
                         output.size(),
                         "JSON nesting exceeds the configured limit");
                return false;
            }
            return writeObject(value.objectItems(), depth + 1, output);
        }
        setError(
            error_, JsonErrorCode::InvalidValue, output.size(), "JSON value has an unknown type");
        return false;
    }

    bool writeString(const std::string& value, std::string& output)
    {
        if (value.size() > limits_.max_string_bytes) {
            setError(error_,
                     JsonErrorCode::StringLimit,
                     output.size(),
                     "JSON string exceeds the configured limit");
            return false;
        }
        if (!add(output, '"'))
            return false;
        for (std::size_t position = 0; position < value.size();) {
            const unsigned char byte = static_cast<unsigned char>(value[position]);
            const char* escape = nullptr;
            std::size_t escapeLength = 0;
            switch (byte) {
            case '"':
                escape = "\\\"";
                escapeLength = 2;
                break;
            case '\\':
                escape = "\\\\";
                escapeLength = 2;
                break;
            case '\b':
                escape = "\\b";
                escapeLength = 2;
                break;
            case '\f':
                escape = "\\f";
                escapeLength = 2;
                break;
            case '\n':
                escape = "\\n";
                escapeLength = 2;
                break;
            case '\r':
                escape = "\\r";
                escapeLength = 2;
                break;
            case '\t':
                escape = "\\t";
                escapeLength = 2;
                break;
            default:
                break;
            }
            if (escape != nullptr) {
                if (!add(output, escape, escapeLength))
                    return false;
                ++position;
                continue;
            }
            if (byte < 0x20u) {
                setError(error_,
                         JsonErrorCode::InvalidValue,
                         position,
                         "JSON string contains an unescaped control character");
                return false;
            }
            if (byte < 0x80u) {
                if (!add(output, static_cast<char>(byte)))
                    return false;
                ++position;
                continue;
            }
            std::size_t length = 0;
            std::uint32_t codepoint = 0;
            if (!readUtf8(value, position, length, codepoint)) {
                setError(error_,
                         JsonErrorCode::InvalidUtf8,
                         position,
                         "JSON string contains invalid UTF-8");
                return false;
            }
            if (!add(output, value.data() + position, length))
                return false;
            position += length;
        }
        return add(output, '"');
    }

    bool writeArray(const JsonValue::Array& items, std::size_t depth, std::string& output)
    {
        if (items.size() > limits_.max_array_items) {
            setError(error_,
                     JsonErrorCode::ArrayLimit,
                     output.size(),
                     "JSON array exceeds the configured limit");
            return false;
        }
        if (!add(output, '['))
            return false;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (i != 0 && !add(output, ','))
                return false;
            if (!writeValue(items[i], depth, output))
                return false;
        }
        return add(output, ']');
    }

    bool writeObject(const JsonValue::Object& members, std::size_t depth, std::string& output)
    {
        if (members.size() > limits_.max_object_members) {
            setError(error_,
                     JsonErrorCode::ObjectLimit,
                     output.size(),
                     "JSON object exceeds the configured member limit");
            return false;
        }
        if (!add(output, '{'))
            return false;
        std::size_t index = 0;
        for (const auto& member : members) {
            if (index++ != 0 && !add(output, ','))
                return false;
            if (!writeString(member.first, output) || !add(output, ':') ||
                !writeValue(member.second, depth, output)) {
                return false;
            }
        }
        return add(output, '}');
    }

    const JsonLimits& limits_;
    JsonError& error_;
};

} // namespace

void JsonError::clear()
{
    code = JsonErrorCode::None;
    offset = 0;
    message.clear();
}

JsonValue::JsonValue() = default;
JsonValue::JsonValue(std::nullptr_t) : JsonValue() {}
JsonValue::JsonValue(bool value) : type_(Type::Bool), bool_value_(value) {}
JsonValue::JsonValue(std::int64_t value) : type_(Type::Int64), int64_value_(value) {}
JsonValue::JsonValue(std::string value) : type_(Type::String), string_value_(std::move(value)) {}
JsonValue::JsonValue(const char* value)
    : type_(Type::String), string_value_(value == nullptr ? "" : value)
{
}
JsonValue::JsonValue(Array value) : type_(Type::Array), array_items_(std::move(value)) {}
JsonValue::JsonValue(Object value) : type_(Type::Object), object_items_(std::move(value)) {}

JsonValue JsonValue::null()
{
    return JsonValue();
}
JsonValue JsonValue::boolean(bool value)
{
    return JsonValue(value);
}
JsonValue JsonValue::integer(std::int64_t value)
{
    return JsonValue(value);
}
JsonValue JsonValue::string(std::string value)
{
    return JsonValue(std::move(value));
}
JsonValue JsonValue::array(Array value)
{
    return JsonValue(std::move(value));
}
JsonValue JsonValue::object(Object value)
{
    return JsonValue(std::move(value));
}

JsonValue::Type JsonValue::type() const noexcept
{
    return type_;
}
bool JsonValue::isNull() const noexcept
{
    return type_ == Type::Null;
}
bool JsonValue::isBool() const noexcept
{
    return type_ == Type::Bool;
}
bool JsonValue::isInt64() const noexcept
{
    return type_ == Type::Int64;
}
bool JsonValue::isString() const noexcept
{
    return type_ == Type::String;
}
bool JsonValue::isArray() const noexcept
{
    return type_ == Type::Array;
}
bool JsonValue::isObject() const noexcept
{
    return type_ == Type::Object;
}
bool JsonValue::boolValue() const noexcept
{
    return bool_value_;
}
std::int64_t JsonValue::int64Value() const noexcept
{
    return int64_value_;
}

const std::string& JsonValue::stringValue() const noexcept
{
    return string_value_;
}
const JsonValue::Array& JsonValue::arrayItems() const noexcept
{
    return array_items_;
}
JsonValue::Array& JsonValue::arrayItems() noexcept
{
    return array_items_;
}
const JsonValue::Object& JsonValue::objectItems() const noexcept
{
    return object_items_;
}
JsonValue::Object& JsonValue::objectItems() noexcept
{
    return object_items_;
}

const JsonValue* JsonValue::find(const std::string& key) const noexcept
{
    if (!isObject())
        return nullptr;
    const auto iterator = object_items_.find(key);
    return iterator == object_items_.end() ? nullptr : &iterator->second;
}

JsonValue* JsonValue::find(const std::string& key) noexcept
{
    if (!isObject())
        return nullptr;
    const auto iterator = object_items_.find(key);
    return iterator == object_items_.end() ? nullptr : &iterator->second;
}

bool parseJson(const std::string& input,
               JsonValue& value,
               JsonError& error,
               const JsonLimits& limits)
{
    error.clear();
    value = JsonValue::null();
    return Parser(input, limits, error).parse(value);
}

bool parseJson(const std::string& input,
               JsonValue& value,
               std::string& error,
               const JsonLimits& limits)
{
    JsonError detail;
    const bool success = parseJson(input, value, detail, limits);
    error = detail.message;
    return success;
}

bool serializeJson(const JsonValue& value,
                   std::string& output,
                   JsonError& error,
                   const JsonLimits& limits)
{
    error.clear();
    output.clear();
    return Serializer(limits, error).run(value, output);
}

std::string serializeJson(const JsonValue& value, const JsonLimits& limits)
{
    std::string output;
    JsonError error;
    if (!serializeJson(value, output, error, limits))
        return {};
    return output;
}

} // namespace testtools
