#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <type_traits>
#include <vector>

namespace testtools {

// 控制面 JSON 解析与序列化的安全边界配置。调用方可根据具体报文场景收紧上限，
// 但解析器绝不允许超出硬性上限或退化为无界解析。
struct JsonLimits {
    static constexpr std::size_t HardMaxInputBytes = 8 * 1024 * 1024;
    static constexpr std::size_t HardMaxOutputBytes = 8 * 1024 * 1024;
    static constexpr std::size_t HardMaxDepth = 128;
    static constexpr std::size_t HardMaxStringBytes = 4 * 1024 * 1024;
    static constexpr std::size_t HardMaxArrayItems = 65536;
    static constexpr std::size_t HardMaxObjectMembers = 65536;

    static constexpr std::size_t DefaultMaxInputBytes = 1024 * 1024;
    static constexpr std::size_t DefaultMaxOutputBytes = 1024 * 1024;
    static constexpr std::size_t DefaultMaxDepth = 32;
    static constexpr std::size_t DefaultMaxStringBytes = 256 * 1024;
    static constexpr std::size_t DefaultMaxArrayItems = 4096;
    static constexpr std::size_t DefaultMaxObjectMembers = 4096;

    std::size_t max_input_bytes = DefaultMaxInputBytes;
    std::size_t max_output_bytes = DefaultMaxOutputBytes;
    std::size_t max_depth = DefaultMaxDepth;
    std::size_t max_string_bytes = DefaultMaxStringBytes;
    std::size_t max_array_items = DefaultMaxArrayItems;
    std::size_t max_object_members = DefaultMaxObjectMembers;
};

enum class JsonErrorCode {
    None,
    InputTooLarge,
    UnexpectedEnd,
    InvalidSyntax,
    InvalidUtf8,
    InvalidEscape,
    DuplicateObjectKey,
    DepthLimit,
    StringLimit,
    ArrayLimit,
    ObjectLimit,
    IntegerOverflow,
    NonIntegerNumber,
    TrailingData,
    OutputTooLarge,
    InvalidValue
};

struct JsonError {
    JsonErrorCode code = JsonErrorCode::None;
    std::size_t offset = 0;
    std::string message;

    void clear();
};

class JsonValue {
  public:
    enum class Type { Null, Bool, Int64, String, Array, Object };

    using Array = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue>;

    JsonValue();
    explicit JsonValue(std::nullptr_t);
    explicit JsonValue(bool value);
    explicit JsonValue(std::int64_t value);

    template <typename Integer,
              typename std::enable_if<std::is_integral<Integer>::value &&
                                          !std::is_same<Integer, bool>::value,
                                      int>::type = 0>
    explicit JsonValue(Integer value) : JsonValue(static_cast<std::int64_t>(value))
    {
    }

    explicit JsonValue(std::string value);
    explicit JsonValue(const char* value);
    explicit JsonValue(Array value);
    explicit JsonValue(Object value);

    static JsonValue null();
    static JsonValue boolean(bool value);
    static JsonValue integer(std::int64_t value);
    static JsonValue string(std::string value);
    static JsonValue array(Array value = {});
    static JsonValue object(Object value = {});

    Type type() const noexcept;
    bool isNull() const noexcept;
    bool isBool() const noexcept;
    bool isInt64() const noexcept;
    bool isString() const noexcept;
    bool isArray() const noexcept;
    bool isObject() const noexcept;

    bool boolValue() const noexcept;
    std::int64_t int64Value() const noexcept;
    const std::string& stringValue() const noexcept;
    const Array& arrayItems() const noexcept;
    Array& arrayItems() noexcept;
    const Object& objectItems() const noexcept;
    Object& objectItems() noexcept;

    const JsonValue* find(const std::string& key) const noexcept;
    JsonValue* find(const std::string& key) noexcept;

  private:
    Type type_ = Type::Null;
    bool bool_value_ = false;
    std::int64_t int64_value_ = 0;
    std::string string_value_;
    Array array_items_;
    Object object_items_;
};

// 带 JsonError 的重载保留机器可读的错误码与出错字节偏移量。
bool parseJson(const std::string& input,
               JsonValue& value,
               JsonError& error,
               const JsonLimits& limits = {});

// 仅需诊断字符串时的便捷重载。
bool parseJson(const std::string& input,
               JsonValue& value,
               std::string& error,
               const JsonLimits& limits = {});

bool serializeJson(const JsonValue& value,
                   std::string& output,
                   JsonError& error,
                   const JsonLimits& limits = {});

// 序列化失败时返回空字符串；由于合法 JSON 文档不可能为空字符串，调用方可直接判空或使用带 JsonError 的重载。
std::string serializeJson(const JsonValue& value, const JsonLimits& limits = {});

} // namespace testtools
