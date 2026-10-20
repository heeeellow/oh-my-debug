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

// Board identity values come from the structured system.info response.  Profile
// identity fields are compared literally; an absent value never matches a
// configured field.
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
    // Canonical values are "emmc" and "ufs".  The loader accepts their
    // conventional spelling variants and stores the canonical lower-case form.
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
    // Canonical values are "emmc" and "ufs"; an empty or unknown value is
    // retained so the evaluator can report VERIFY instead of guessing.
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

// Zero or multiple identity matches deliberately remain VERIFY.  A unique
// identity match is then evaluated against only the expectations present in
// that profile.  Missing observations are VERIFY; explicit mismatches are
// FAIL.
ProfileEvaluation evaluateBoardProfile(const BoardIdentity& identity,
                                       const BoardObservations& observations,
                                       const std::vector<Profile>& profiles);

// Evaluates only the expectation domain represented by a single capability.
// This keeps a read-only memory result independent from storage observations
// (and storage identity independent from health) while preserving the same
// exact Board Profile match rule.
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

// Loads only regular *.json files immediately within directory.  The loader
// never writes, follows symlink files, recurses, or accepts more than the
// configured file/byte/profile limits.  Valid profiles are returned alongside
// errors so callers may present diagnostics without silently hiding them.
BoardProfileLoadResult loadBoardProfiles(const std::filesystem::path& directory,
                                         const BoardProfileLoaderLimits& limits = {});

// Exposed for callers that already have a bounded JSON document (for example,
// tests or a packaged profile reader).  The document must use the fixed
// profile schema described by Profile; no expressions or inheritance are
// interpreted.
bool parseBoardProfile(const JsonValue& document, Profile& profile, std::string& error);

} // namespace testtools
