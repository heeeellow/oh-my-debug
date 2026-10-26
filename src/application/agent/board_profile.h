#pragma once

#include "core/protocol/json_value.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace testtools {

// 板卡身份字段来自结构化 system.info 响应；Profile 身份字段按字面值精确比对，
// 缺失字段绝不会匹配已配置的预期字段。
struct BoardIdentity {
    std::map<std::string, std::string> fields;
};

struct MemoryCapacityExpectation {
    std::optional<std::uint64_t> minimumKiB;
    std::optional<std::uint64_t> maximumKiB;
};

struct StorageHealthExpectation {
    std::optional<std::uint32_t> maximumPreEolInfo;
    std::optional<std::uint32_t> maximumLifeTimeEstimationA;
    std::optional<std::uint32_t> maximumLifeTimeEstimationB;
};

struct StorageExpectation {
    // 规范化取值为 "emmc" 和 "ufs"；加载器接受常见大小写拼写变体并统一存为小写规范形式。
    std::optional<std::string> type;
    std::optional<std::uint64_t> minimumCapacityBytes;
    std::optional<std::uint64_t> maximumCapacityBytes;
    StorageHealthExpectation health;
};

struct Profile {
    std::string profileId;
    int profileVersion = 0;
    std::map<std::string, std::string> identityEquals;
    std::optional<MemoryCapacityExpectation> memory;
    std::optional<StorageExpectation> storage;
};

struct MemoryCapacityObservation {
    std::optional<std::uint64_t> memtotalKiB;
};

struct StorageHealthObservation {
    std::optional<std::uint32_t> preEolInfo;
    std::optional<std::uint32_t> lifeTimeEstimationA;
    std::optional<std::uint32_t> lifeTimeEstimationB;
};

struct StorageObservation {
    // 规范化取值为 "emmc" 和 "ufs"；保留空值或未知值以便评估器给出 VERIFY 而非猜测。
    std::string type;
    std::optional<std::uint64_t> capacityBytes;
    StorageHealthObservation health;
};

struct BoardObservations {
    MemoryCapacityObservation memory;
    std::vector<StorageObservation> storage;
};

enum class ProfileCheckVerdict {
    Pass,
    Verify,
    Fail,
};

const char* profileCheckVerdictName(ProfileCheckVerdict verdict) noexcept;

struct ProfileCheck {
    std::string name;
    ProfileCheckVerdict verdict = ProfileCheckVerdict::Verify;
    std::string expected;
    std::string observed;
    std::string reason;
};

enum class ProfileVerdict {
    Pass,
    Verify,
    Fail,
};

const char* profileVerdictName(ProfileVerdict verdict) noexcept;

struct ProfileEvaluation {
    ProfileVerdict verdict = ProfileVerdict::Verify;
    std::size_t matchedProfileCount = 0;
    std::optional<std::string> profileId;
    int profileVersion = 0;
    std::vector<ProfileCheck> checks;
    std::string reason;
};

// 零个或多个身份匹配结果一律保持 VERIFY；仅当唯一匹配某个 Profile 时，
// 才针对该 Profile 中声明的预期项进行评估（观测缺失记为 VERIFY，明确不符记为 FAIL）。
ProfileEvaluation evaluateBoardProfile(const BoardIdentity& identity,
                                       const BoardObservations& observations,
                                       const std::vector<Profile>& profiles);

// 仅评估单个能力对应的预期域，使只读内存检查独立于存储观测（存储身份也独立于健康度），
// 同时保持完全一致的 Board Profile 唯一身份匹配规则。
ProfileEvaluation evaluateBoardProfileForCapability(const BoardIdentity& identity,
                                                    const BoardObservations& observations,
                                                    const std::vector<Profile>& profiles,
                                                    const std::string& capabilityId);

struct BoardProfileLoaderLimits {
    static constexpr std::size_t DefaultMaxFiles = 128;
    static constexpr std::size_t DefaultMaxFileBytes = 128 * 1024;
    static constexpr std::size_t DefaultMaxTotalBytes = 2 * 1024 * 1024;
    static constexpr std::size_t DefaultMaxProfiles = 128;

    std::size_t maxFiles = DefaultMaxFiles;
    std::size_t maxFileBytes = DefaultMaxFileBytes;
    std::size_t maxTotalBytes = DefaultMaxTotalBytes;
    std::size_t maxProfiles = DefaultMaxProfiles;
    JsonLimits jsonLimits{};
};

struct BoardProfileLoadError {
    std::filesystem::path path;
    std::string message;
};

struct BoardProfileLoadResult {
    bool ok = false;
    std::vector<Profile> profiles;
    std::vector<BoardProfileLoadError> errors;
};

// 仅加载指定目录顶层的常规 *.json 文件：不写入、不跟随符号链接、不递归子目录，
// 并严格执行文件数/字节数/Profile 数量上限；有效配置与错误列表一并返回以便诊断。
BoardProfileLoadResult loadBoardProfiles(const std::filesystem::path& directory,
                                         const BoardProfileLoaderLimits& limits = {});

// 解析单个已加载的有界 JSON 文档为 Profile 结构，仅使用固定 Schema，不解释表达式或继承。
bool parseBoardProfile(const JsonValue& document, Profile& profile, std::string& error);

} // namespace testtools
