#include "application/agent/agent_bundle.h"

#include "core/protocol/agent_version.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <vector>

namespace testtools {
namespace {

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Object, Array } type = Type::Null;
    std::string scalar;
    std::map<std::string, JsonValue> object;
    std::vector<JsonValue> array;
};

class JsonParser {
  public:
    explicit JsonParser(std::string text) : text_(std::move(text)) {}

    bool parse(JsonValue& value, std::string& error)
    {
        skip();
        if (!parseValue(value, error)) {
            return false;
        }
        skip();
        if (position_ != text_.size()) {
            error = "manifest JSON contains trailing data";
            return false;
        }
        return true;
    }

  private:
    void skip()
    {
        while (position_ < text_.size() &&
               std::isspace(static_cast<unsigned char>(text_[position_]))) {
            ++position_;
        }
    }

    bool consume(char expected)
    {
        skip();
        if (position_ >= text_.size() || text_[position_] != expected) {
            return false;
        }
        ++position_;
        return true;
    }

    bool parseValue(JsonValue& value, std::string& error)
    {
        skip();
        if (position_ >= text_.size()) {
            error = "manifest JSON ended unexpectedly";
            return false;
        }
        const char ch = text_[position_];
        if (ch == '{')
            return parseObject(value, error);
        if (ch == '[')
            return parseArray(value, error);
        if (ch == '"') {
            value.type = JsonValue::Type::String;
            return parseString(value.scalar, error);
        }
        if (text_.compare(position_, 4, "true") == 0) {
            position_ += 4;
            value.type = JsonValue::Type::Bool;
            value.scalar = "true";
            return true;
        }
        if (text_.compare(position_, 5, "false") == 0) {
            position_ += 5;
            value.type = JsonValue::Type::Bool;
            value.scalar = "false";
            return true;
        }
        if (text_.compare(position_, 4, "null") == 0) {
            position_ += 4;
            value.type = JsonValue::Type::Null;
            return true;
        }
        if (ch == '-' || std::isdigit(static_cast<unsigned char>(ch))) {
            const auto begin = position_++;
            while (position_ < text_.size() &&
                   (std::isdigit(static_cast<unsigned char>(text_[position_])) ||
                    text_[position_] == '.' || text_[position_] == '-' || text_[position_] == '+' ||
                    text_[position_] == 'e' || text_[position_] == 'E')) {
                ++position_;
            }
            value.type = JsonValue::Type::Number;
            value.scalar = text_.substr(begin, position_ - begin);
            return true;
        }
        error = "manifest JSON contains an invalid value";
        return false;
    }

    bool parseString(std::string& value, std::string& error)
    {
        if (!consume('"')) {
            error = "manifest JSON string expected";
            return false;
        }
        value.clear();
        while (position_ < text_.size()) {
            const char ch = text_[position_++];
            if (ch == '"')
                return true;
            if (ch == '\\') {
                if (position_ >= text_.size())
                    break;
                const char escaped = text_[position_++];
                switch (escaped) {
                case '"':
                case '\\':
                case '/':
                    value.push_back(escaped);
                    break;
                case 'b':
                    value.push_back('\b');
                    break;
                case 'f':
                    value.push_back('\f');
                    break;
                case 'n':
                    value.push_back('\n');
                    break;
                case 'r':
                    value.push_back('\r');
                    break;
                case 't':
                    value.push_back('\t');
                    break;
                default:
                    error = "manifest JSON uses unsupported string escape";
                    return false;
                }
            } else {
                if (static_cast<unsigned char>(ch) < 0x20) {
                    error = "manifest JSON string contains control character";
                    return false;
                }
                value.push_back(ch);
            }
        }
        error = "manifest JSON string is unterminated";
        return false;
    }

    bool parseObject(JsonValue& value, std::string& error)
    {
        if (!consume('{')) {
            error = "manifest JSON object expected";
            return false;
        }
        value.type = JsonValue::Type::Object;
        value.object.clear();
        skip();
        if (consume('}'))
            return true;
        while (true) {
            std::string key;
            if (!parseString(key, error) || !consume(':')) {
                error = "manifest JSON object member expected";
                return false;
            }
            if (value.object.find(key) != value.object.end()) {
                error = "manifest JSON contains duplicate object key: " + key;
                return false;
            }
            JsonValue member;
            if (!parseValue(member, error))
                return false;
            value.object.emplace(std::move(key), std::move(member));
            if (consume('}'))
                return true;
            if (!consume(',')) {
                error = "manifest JSON object separator expected";
                return false;
            }
        }
    }

    bool parseArray(JsonValue& value, std::string& error)
    {
        if (!consume('[')) {
            error = "manifest JSON array expected";
            return false;
        }
        value.type = JsonValue::Type::Array;
        value.array.clear();
        skip();
        if (consume(']'))
            return true;
        while (true) {
            JsonValue item;
            if (!parseValue(item, error))
                return false;
            value.array.push_back(std::move(item));
            if (consume(']'))
                return true;
            if (!consume(',')) {
                error = "manifest JSON array separator expected";
                return false;
            }
        }
    }

    std::string text_;
    std::size_t position_ = 0;
};

const JsonValue* member(const JsonValue& value, const char* key)
{
    if (value.type != JsonValue::Type::Object)
        return nullptr;
    const auto it = value.object.find(key);
    return it == value.object.end() ? nullptr : &it->second;
}

bool stringMember(const JsonValue& object, const char* key, std::string& output)
{
    const auto* value = member(object, key);
    if (!value || value->type != JsonValue::Type::String || value->scalar.empty())
        return false;
    output = value->scalar;
    return true;
}

bool numberMember(const JsonValue& object, const char* key, std::uintmax_t& output)
{
    const auto* value = member(object, key);
    if (!value || value->type != JsonValue::Type::Number || value->scalar.empty() ||
        value->scalar.find_first_not_of("0123456789") != std::string::npos)
        return false;
    try {
        output = static_cast<std::uintmax_t>(std::stoull(value->scalar));
        return true;
    } catch (...) {
        return false;
    }
}

bool boolMember(const JsonValue& object, const char* key, bool& output)
{
    const auto* value = member(object, key);
    if (!value || value->type != JsonValue::Type::Bool)
        return false;
    output = value->scalar == "true";
    return true;
}

bool safeRelative(std::string_view value, bool allowNested)
{
    if (value.empty() || value.front() == '/' || value.find('\\') != std::string_view::npos ||
        value.find("..") != std::string_view::npos || value.find(':') != std::string_view::npos ||
        value.find('\0') != std::string_view::npos)
        return false;
    if (!allowNested && value.find('/') != std::string_view::npos)
        return false;
    if (!std::all_of(value.begin(), value.end(), [](unsigned char ch) {
            return std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.' || ch == '/';
        }))
        return false;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find('/', start);
        const auto part =
            value.substr(start, end == std::string_view::npos ? value.size() - start : end - start);
        if (part.empty() || part == "." || part == "..")
            return false;
        start = end == std::string_view::npos ? value.size() + 1 : end + 1;
    }
    return true;
}

bool validSha256(const std::string& value)
{
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return std::isdigit(ch) || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
           });
}

std::string sha256File(const std::filesystem::path& path, std::string& error)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "cannot read bundle file: " + path.string();
        return {};
    }
    const std::array<std::uint32_t, 64> constants = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    const auto rotateRight = [](std::uint32_t value, unsigned bits) {
        return (value >> bits) | (value << (32 - bits));
    };
    std::array<std::uint32_t, 8> state = {0x6a09e667,
                                          0xbb67ae85,
                                          0x3c6ef372,
                                          0xa54ff53a,
                                          0x510e527f,
                                          0x9b05688c,
                                          0x1f83d9ab,
                                          0x5be0cd19};
    const auto transform = [&](const std::array<unsigned char, 64>& block) {
        std::array<std::uint32_t, 64> w{};
        for (std::size_t index = 0; index < 16; ++index) {
            const auto offset = index * 4;
            w[index] = (static_cast<std::uint32_t>(block[offset]) << 24) |
                       (static_cast<std::uint32_t>(block[offset + 1]) << 16) |
                       (static_cast<std::uint32_t>(block[offset + 2]) << 8) | block[offset + 3];
        }
        for (std::size_t index = 16; index < 64; ++index) {
            const auto s0 = rotateRight(w[index - 15], 7) ^ rotateRight(w[index - 15], 18) ^
                            (w[index - 15] >> 3);
            const auto s1 = rotateRight(w[index - 2], 17) ^ rotateRight(w[index - 2], 19) ^
                            (w[index - 2] >> 10);
            w[index] = w[index - 16] + s0 + w[index - 7] + s1;
        }
        auto working = state;
        for (std::size_t index = 0; index < 64; ++index) {
            const auto sum1 = rotateRight(working[4], 6) ^ rotateRight(working[4], 11) ^
                              rotateRight(working[4], 25);
            const auto choose = (working[4] & working[5]) ^ ((~working[4]) & working[6]);
            const auto temporary1 = working[7] + sum1 + choose + constants[index] + w[index];
            const auto sum0 = rotateRight(working[0], 2) ^ rotateRight(working[0], 13) ^
                              rotateRight(working[0], 22);
            const auto majority =
                (working[0] & working[1]) ^ (working[0] & working[2]) ^ (working[1] & working[2]);
            const auto temporary2 = sum0 + majority;
            working = {temporary1 + temporary2,
                       working[0],
                       working[1],
                       working[2],
                       working[3] + temporary1,
                       working[4],
                       working[5],
                       working[6]};
        }
        for (std::size_t index = 0; index < state.size(); ++index) {
            state[index] += working[index];
        }
    };

    std::array<unsigned char, 64> block{};
    std::uint64_t byteLength = 0;
    std::size_t tailSize = 0;
    while (true) {
        input.read(reinterpret_cast<char*>(block.data()),
                   static_cast<std::streamsize>(block.size()));
        const auto count = input.gcount();
        if (count < 0) {
            error = "cannot read bundle file: " + path.string();
            return {};
        }
        byteLength += static_cast<std::uint64_t>(count);
        if (count == static_cast<std::streamsize>(block.size())) {
            transform(block);
            continue;
        }
        if (input.bad()) {
            error = "cannot read bundle file: " + path.string();
            return {};
        }
        tailSize = static_cast<std::size_t>(count);
        break;
    }

    block[tailSize++] = 0x80;
    if (tailSize > 56) {
        std::fill(block.begin() + static_cast<std::ptrdiff_t>(tailSize), block.end(), 0);
        transform(block);
        block.fill(0);
        tailSize = 0;
    }
    std::fill(block.begin() + static_cast<std::ptrdiff_t>(tailSize), block.begin() + 56, 0);
    const std::uint64_t bitLength = byteLength * 8;
    for (int shift = 56; shift >= 0; shift -= 8) {
        block[56 + static_cast<std::size_t>((56 - shift) / 8)] =
            static_cast<unsigned char>(bitLength >> shift);
    }
    transform(block);

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto value : state) {
        output << std::setw(8) << value;
    }
    return output.str();
}

bool equalFile(const std::filesystem::path& path, const AgentBundleFile& file, std::string& error)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        error = "bundle file is missing: " + path.string();
        return false;
    }
    if (std::filesystem::file_size(path, ec) != file.size || ec) {
        error = "bundle file size mismatch: " + path.string();
        return false;
    }
    const auto actual = sha256File(path, error);
    if (actual.empty() ||
        !std::equal(actual.begin(), actual.end(), file.sha256.begin(), [](char lhs, char rhs) {
            return std::tolower(static_cast<unsigned char>(lhs)) ==
                   std::tolower(static_cast<unsigned char>(rhs));
        })) {
        error = "bundle file SHA-256 mismatch: " + path.string();
        return false;
    }
    return true;
}

} // namespace

const AgentBundleFile* AgentBundlePackage::agentFile() const
{
    for (const auto& file : files)
        if (file.role == "agent")
            return &file;
    return nullptr;
}

std::optional<AgentBundle> loadAgentBundle(const std::filesystem::path& bundleRoot,
                                           std::string& error)
{
    const auto root = std::filesystem::absolute(bundleRoot).lexically_normal();
    const auto manifestPath = root / "manifest.json";
    std::error_code manifestError;
    const auto manifestSize = std::filesystem::file_size(manifestPath, manifestError);
    if (manifestError) {
        error = "Agent Bundle manifest not found: " + manifestPath.string();
        return std::nullopt;
    }
    if (manifestSize == 0 || manifestSize > 1024 * 1024) {
        error = "Agent Bundle manifest size is invalid";
        return std::nullopt;
    }
    std::ifstream input(manifestPath, std::ios::binary);
    if (!input) {
        error = "Agent Bundle manifest cannot be read: " + manifestPath.string();
        return std::nullopt;
    }
    const std::string text((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    JsonValue document;
    if (!JsonParser(text).parse(document, error))
        return std::nullopt;
    std::uintmax_t schema = 0;
    if (!numberMember(document, "schema_version", schema) || schema != 2) {
        error = "unsupported Agent Bundle schema; expected v2";
        return std::nullopt;
    }
    std::string product;
    if (!stringMember(document, "product", product) || product != "TestTools") {
        error = "manifest product is not TestTools";
        return std::nullopt;
    }
    AgentBundle result;
    result.root = root;
    if (!stringMember(document, "agent_version", result.agentVersion) ||
        !safeRelative(result.agentVersion, false) || result.agentVersion != AgentVersion) {
        error = "manifest agent_version does not match this TestTools build";
        return std::nullopt;
    }
    const auto* protocol = member(document, "protocol");
    std::uintmax_t minimum = 0, maximum = 0;
    if (!protocol || !numberMember(*protocol, "minimum", minimum) ||
        !numberMember(*protocol, "maximum", maximum) || minimum != 1 || maximum != 1) {
        error = "manifest protocol range is not v1";
        return std::nullopt;
    }
    result.protocolMinimum = static_cast<int>(minimum);
    result.protocolMaximum = static_cast<int>(maximum);
    const auto* build = member(document, "build");
    if (!build || !stringMember(*build, "build_id", result.buildId) ||
        !safeRelative(result.buildId, false)) {
        error = "manifest build_id is invalid";
        return std::nullopt;
    }
    const auto* packages = member(document, "packages");
    if (!packages || packages->type != JsonValue::Type::Array || packages->array.size() != 3) {
        error = "manifest must contain exactly three packages";
        return std::nullopt;
    }
    std::vector<std::string> seenIds;
    for (const auto& item : packages->array) {
        AgentBundlePackage package;
        std::string platform, architecture;
        if (!stringMember(item, "id", package.id) || !safeRelative(package.id, true) ||
            std::find(seenIds.begin(), seenIds.end(), package.id) != seenIds.end() ||
            !stringMember(item, "platform", platform) ||
            !stringMember(item, "architecture", architecture) ||
            !stringMember(item, "root", package.root) || !safeRelative(package.root, true)) {
            error = "manifest package identity or root is invalid";
            return std::nullopt;
        }
        seenIds.push_back(package.id);
        if (platform == "android" && architecture == "armv7")
            package.platform = AgentPlatform::Android,
            package.architecture = AgentArchitecture::ArmV7;
        else if (platform == "android" && architecture == "aarch64")
            package.platform = AgentPlatform::Android,
            package.architecture = AgentArchitecture::Arm64;
        else if (platform == "linux" && architecture == "aarch64")
            package.platform = AgentPlatform::Linux,
            package.architecture = AgentArchitecture::Arm64;
        else {
            error = "manifest package platform or architecture is unsupported";
            return std::nullopt;
        }
        const std::string expectedId =
            package.platform == AgentPlatform::Android
                ? (package.architecture == AgentArchitecture::ArmV7 ? "android/armeabi-v7a"
                                                                    : "android/arm64-v8a")
                : "linux/aarch64/glibc-2.31";
        const std::string expectedRoot =
            package.platform == AgentPlatform::Android
                ? (package.architecture == AgentArchitecture::ArmV7 ? "agents/android/armeabi-v7a"
                                                                    : "agents/android/arm64-v8a")
                : "agents/linux/aarch64/glibc-2.31";
        if (package.id != expectedId || package.root != expectedRoot) {
            error = "manifest package id or root does not match target";
            return std::nullopt;
        }
        if (package.platform == AgentPlatform::Android) {
            const auto* android = member(item, "android");
            std::uintmax_t api = 0;
            if (!android || !stringMember(*android, "abi", package.abi) ||
                !numberMember(*android, "minimum_api", api) || api != 23 ||
                (package.architecture == AgentArchitecture::ArmV7 &&
                 package.abi != "armeabi-v7a") ||
                (package.architecture == AgentArchitecture::Arm64 && package.abi != "arm64-v8a")) {
                error = "manifest Android package metadata is invalid";
                return std::nullopt;
            }
        } else {
            const auto* linux = member(item, "linux");
            if (!linux || !stringMember(*linux, "libc", package.libc) ||
                !stringMember(*linux, "minimum_version", package.libcMinimumVersion) ||
                package.libc != "glibc" || package.libcMinimumVersion != "2.31") {
                error = "manifest Linux package metadata is invalid";
                return std::nullopt;
            }
        }
        const auto* files = member(item, "files");
        if (!files || files->type != JsonValue::Type::Array || files->array.empty() ||
            files->array.size() > 128) {
            error = "manifest package file count is invalid";
            return std::nullopt;
        }
        std::size_t agentCount = 0;
        for (const auto& fileValue : files->array) {
            AgentBundleFile file;
            std::uintmax_t size = 0;
            if (!stringMember(fileValue, "path", file.path) || !safeRelative(file.path, true) ||
                !stringMember(fileValue, "role", file.role) || file.role.empty() ||
                !numberMember(fileValue, "size", size) ||
                !stringMember(fileValue, "sha256", file.sha256) || !validSha256(file.sha256) ||
                !boolMember(fileValue, "executable", file.executable)) {
                error = "manifest file entry is invalid";
                return std::nullopt;
            }
            if (std::any_of(
                    package.files.begin(), package.files.end(), [&](const AgentBundleFile& other) {
                        return other.path == file.path;
                    })) {
                error = "manifest contains duplicate install path";
                return std::nullopt;
            }
            file.size = size;
            if (file.role == "agent")
                ++agentCount;
            file.sourcePath =
                root / std::filesystem::path(package.root) / std::filesystem::path(file.path);
            if (!equalFile(file.sourcePath, file, error))
                return std::nullopt;
            package.files.push_back(std::move(file));
        }
        const auto* agent = package.agentFile();
        if (!agent || agentCount != 1 || agent->path != "testtools-agent" || !agent->executable) {
            error = "manifest package must contain one executable testtools-agent";
            return std::nullopt;
        }
        result.packages.push_back(std::move(package));
    }
    return result;
}

std::optional<AgentBundle>
loadAgentBundleFromApplication(const std::filesystem::path& applicationDirectory,
                               std::string& error)
{
    return loadAgentBundle(applicationDirectory / "agent-bundle", error);
}

const AgentBundlePackage* selectAndroidAgentPackage(const AgentBundle& bundle, std::string_view abi)
{
    for (const auto& package : bundle.packages)
        if (package.platform == AgentPlatform::Android && package.abi == abi)
            return &package;
    return nullptr;
}

const AgentBundlePackage* selectLinuxAgentPackage(const AgentBundle& bundle,
                                                  std::string_view machine,
                                                  std::string_view libcVersion)
{
    if (machine != "aarch64" && machine != "arm64")
        return nullptr;
    const auto dot = libcVersion.find('.');
    if (dot == std::string_view::npos)
        return nullptr;
    try {
        const int major = std::stoi(std::string(libcVersion.substr(0, dot)));
        const int minor = std::stoi(std::string(libcVersion.substr(dot + 1)));
        if (major < 2 || (major == 2 && minor < 31))
            return nullptr;
    } catch (...) {
        return nullptr;
    }
    for (const auto& package : bundle.packages)
        if (package.platform == AgentPlatform::Linux &&
            package.architecture == AgentArchitecture::Arm64)
            return &package;
    return nullptr;
}

} // namespace testtools
