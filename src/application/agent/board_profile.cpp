#include "application/agent/board_profile.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <limits>
#include <set>
#include <system_error>

namespace testtools {
namespace {

constexpr std::size_t MaximumProfileIdBytes = 128;
constexpr std::size_t MaximumIdentityFields = 32;
constexpr std::size_t MaximumIdentityKeyBytes = 64;
constexpr std::size_t MaximumIdentityValueBytes = 256;

const JsonValue* member(const JsonValue& object, const char* key)
{
    return object.find(key);
}

bool hasOnlyKeys(const JsonValue::Object& object,
                 const std::set<std::string>& allowed,
                 std::string& error)
{
    for (const auto& entry : object) {
        if (allowed.find(entry.first) == allowed.end()) {
            error = "unsupported profile field: " + entry.first;
            return false;
        }
    }
    return true;
}

bool validKey(const std::string& value, std::size_t maximum)
{
    if (value.empty() || value.size() > maximum) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '_' || character == '-' ||
               character == '.';
    });
}

bool stringValue(const JsonValue* value, std::string& output)
{
    if (!value || !value->isString()) {
        return false;
    }
    output = value->stringValue();
    return true;
}

bool positiveInt(const JsonValue* value, int& output)
{
    if (!value || !value->isInt64() || value->int64Value() <= 0 ||
        value->int64Value() > std::numeric_limits<int>::max()) {
        return false;
    }
    output = static_cast<int>(value->int64Value());
    return true;
}

bool unsignedValue(const JsonValue* value, std::uint64_t& output)
{
    if (!value || !value->isInt64() || value->int64Value() < 0) {
        return false;
    }
    output = static_cast<std::uint64_t>(value->int64Value());
    return true;
}

bool optionalUnsigned(const JsonValue::Object& object,
                      const char* key,
                      std::optional<std::uint64_t>& output,
                      std::string& error)
{
    const auto iterator = object.find(key);
    if (iterator == object.end()) {
        return true;
    }
    std::uint64_t value = 0;
    if (!unsignedValue(&iterator->second, value)) {
        error = std::string("profile field is not a non-negative integer: ") + key;
        return false;
    }
    output = value;
    return true;
}

bool optionalUnsigned32(const JsonValue::Object& object,
                        const char* key,
                        std::optional<std::uint32_t>& output,
                        std::string& error)
{
    std::optional<std::uint64_t> value;
    if (!optionalUnsigned(object, key, value, error)) {
        return false;
    }
    if (value && *value > std::numeric_limits<std::uint32_t>::max()) {
        error = std::string("profile field exceeds uint32 range: ") + key;
        return false;
    }
    if (value) {
        output = static_cast<std::uint32_t>(*value);
    }
    return true;
}

bool parseRange(const JsonValue& value,
                const char* name,
                std::optional<std::uint64_t>& minimum,
                std::optional<std::uint64_t>& maximum,
                std::string& error)
{
    if (!value.isObject()) {
        error = std::string("profile range must be an object: ") + name;
        return false;
    }
    if (!hasOnlyKeys(value.objectItems(), {"min", "max"}, error) ||
        !optionalUnsigned(value.objectItems(), "min", minimum, error) ||
        !optionalUnsigned(value.objectItems(), "max", maximum, error)) {
        return false;
    }
    if (!minimum && !maximum) {
        error = std::string("profile range has no bound: ") + name;
        return false;
    }
    if (minimum && maximum && *minimum > *maximum) {
        error = std::string("profile range min exceeds max: ") + name;
        return false;
    }
    return true;
}

bool parseMemory(const JsonValue& value, Profile& profile, std::string& error)
{
    if (!value.isObject() || !hasOnlyKeys(value.objectItems(), {"memtotal_kib"}, error)) {
        if (error.empty())
            error = "profile memory must be an object";
        return false;
    }
    const auto* range = member(value, "memtotal_kib");
    if (!range) {
        error = "profile memory.memtotal_kib is invalid";
        return false;
    }
    auto& memory = profile.memory.emplace();
    if (!parseRange(*range, "memory.memtotal_kib", memory.minimumKiB, memory.maximumKiB, error)) {
        if (error.empty())
            error = "profile memory.memtotal_kib is invalid";
        return false;
    }
    return true;
}

bool parseHealth(const JsonValue& value, StorageExpectation& storage, std::string& error)
{
    if (!value.isObject() ||
        !hasOnlyKeys(
            value.objectItems(),
            {"pre_eol_info_max", "life_time_estimation_a_max", "life_time_estimation_b_max"},
            error)) {
        if (error.empty())
            error = "profile storage.health must be an object";
        return false;
    }
    return optionalUnsigned32(
               value.objectItems(), "pre_eol_info_max", storage.health.maximumPreEolInfo, error) &&
           optionalUnsigned32(value.objectItems(),
                              "life_time_estimation_a_max",
                              storage.health.maximumLifeTimeEstimationA,
                              error) &&
           optionalUnsigned32(value.objectItems(),
                              "life_time_estimation_b_max",
                              storage.health.maximumLifeTimeEstimationB,
                              error);
}

std::optional<std::string> canonicalStorageType(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    if (value == "emmc" || value == "e-mmc")
        return "emmc";
    if (value == "ufs")
        return "ufs";
    return std::nullopt;
}

bool parseStorage(const JsonValue& value, Profile& profile, std::string& error)
{
    if (!value.isObject() ||
        !hasOnlyKeys(value.objectItems(), {"type", "capacity", "health"}, error)) {
        if (error.empty())
            error = "profile storage must be an object";
        return false;
    }
    StorageExpectation& storage = profile.storage.emplace();
    if (const auto* type = member(value, "type")) {
        std::string text;
        if (!stringValue(type, text) || text.empty()) {
            error = "profile storage.type must be a non-empty string";
            return false;
        }
        storage.type = canonicalStorageType(text);
        if (!storage.type) {
            error = "profile storage.type must be emmc or ufs";
            return false;
        }
    }
    if (const auto* capacity = member(value, "capacity")) {
        if (!parseRange(*capacity,
                        "storage.capacity",
                        storage.minimumCapacityBytes,
                        storage.maximumCapacityBytes,
                        error)) {
            return false;
        }
    }
    if (const auto* health = member(value, "health")) {
        if (!parseHealth(*health, storage, error))
            return false;
    }
    if (!storage.type && !storage.minimumCapacityBytes && !storage.maximumCapacityBytes &&
        !storage.health.maximumPreEolInfo && !storage.health.maximumLifeTimeEstimationA &&
        !storage.health.maximumLifeTimeEstimationB) {
        error = "profile storage has no expectation";
        return false;
    }
    return true;
}

bool identityMatches(const BoardIdentity& identity, const Profile& profile)
{
    for (const auto& expected : profile.identityEquals) {
        const auto observed = identity.fields.find(expected.first);
        if (observed == identity.fields.end() || observed->second != expected.second) {
            return false;
        }
    }
    return true;
}

std::string numberText(std::uint64_t value)
{
    return std::to_string(value);
}

void addCheck(ProfileEvaluation& evaluation,
              std::string name,
              ProfileCheckVerdict verdict,
              std::string expected,
              std::string observed,
              std::string reason)
{
    evaluation.checks.push_back(
        {std::move(name), verdict, std::move(expected), std::move(observed), std::move(reason)});
}

bool checkRange(std::uint64_t observed,
                const std::optional<std::uint64_t>& minimum,
                const std::optional<std::uint64_t>& maximum)
{
    return (!minimum || observed >= *minimum) && (!maximum || observed <= *maximum);
}

std::string rangeText(const std::optional<std::uint64_t>& minimum,
                      const std::optional<std::uint64_t>& maximum)
{
    if (minimum && maximum)
        return numberText(*minimum) + ".." + numberText(*maximum);
    if (minimum)
        return ">=" + numberText(*minimum);
    return "<=" + numberText(*maximum);
}

} // namespace

const char* profileCheckVerdictName(ProfileCheckVerdict verdict) noexcept
{
    switch (verdict) {
    case ProfileCheckVerdict::Pass:
        return "PASS";
    case ProfileCheckVerdict::Verify:
        return "VERIFY";
    case ProfileCheckVerdict::Fail:
        return "FAIL";
    }
    return "VERIFY";
}

const char* profileVerdictName(ProfileVerdict verdict) noexcept
{
    switch (verdict) {
    case ProfileVerdict::Pass:
        return "PASS";
    case ProfileVerdict::Verify:
        return "VERIFY";
    case ProfileVerdict::Fail:
        return "FAIL";
    }
    return "VERIFY";
}

ProfileEvaluation evaluateBoardProfileForCapability(const BoardIdentity& identity,
                                                    const BoardObservations& observations,
                                                    const std::vector<Profile>& profiles,
                                                    const std::string& capabilityId)
{
    ProfileEvaluation evaluation;
    std::vector<const Profile*> matches;
    for (const auto& profile : profiles) {
        if (identityMatches(identity, profile))
            matches.push_back(&profile);
    }
    evaluation.matchedProfileCount = matches.size();
    if (matches.size() != 1) {
        evaluation.reason = matches.empty() ? "no Board Profile matches board identity"
                                            : "multiple Board Profiles match board identity";
        addCheck(evaluation,
                 "profile.match",
                 ProfileCheckVerdict::Verify,
                 "exactly one profile",
                 std::to_string(matches.size()),
                 evaluation.reason);
        return evaluation;
    }

    const Profile& profile = *matches.front();
    evaluation.profileId = profile.profileId;
    evaluation.profileVersion = profile.profileVersion;
    const bool allCapabilities = capabilityId.empty();
    const bool evaluateMemory = allCapabilities || capabilityId == "memory.capacity";
    const bool evaluateStorageInfo = allCapabilities || capabilityId == "storage.device.info";
    const bool evaluateStorageHealth = allCapabilities || capabilityId == "storage.device.health";
    if (profile.memory && evaluateMemory) {
        const auto& expectation = *profile.memory;
        if (!observations.memory.memtotalKiB) {
            addCheck(evaluation,
                     "memory.memtotal_kib",
                     ProfileCheckVerdict::Verify,
                     rangeText(expectation.minimumKiB, expectation.maximumKiB),
                     "missing",
                     "memory capacity observation is missing");
        } else {
            const bool pass = checkRange(
                *observations.memory.memtotalKiB, expectation.minimumKiB, expectation.maximumKiB);
            addCheck(evaluation,
                     "memory.memtotal_kib",
                     pass ? ProfileCheckVerdict::Pass : ProfileCheckVerdict::Fail,
                     rangeText(expectation.minimumKiB, expectation.maximumKiB),
                     numberText(*observations.memory.memtotalKiB),
                     pass ? "within configured range" : "outside configured range");
        }
    }
    if (profile.storage && (evaluateStorageInfo || evaluateStorageHealth)) {
        const auto& expectation = *profile.storage;
        std::vector<const StorageObservation*> candidates;
        bool hasUnknownType = false;
        for (const auto& storage : observations.storage) {
            const auto observedType = canonicalStorageType(storage.type);
            if (expectation.type && !observedType)
                hasUnknownType = true;
            if (!expectation.type || observedType == expectation.type) {
                candidates.push_back(&storage);
            }
        }
        if (candidates.size() != 1) {
            const bool explicitTypeMismatch = candidates.empty() && expectation.type &&
                                              !observations.storage.empty() && !hasUnknownType;
            addCheck(evaluation,
                     "storage.device",
                     explicitTypeMismatch ? ProfileCheckVerdict::Fail : ProfileCheckVerdict::Verify,
                     expectation.type ? *expectation.type : "exactly one device",
                     std::to_string(candidates.size()),
                     explicitTypeMismatch ? "observed storage type differs from the Board Profile"
                     : candidates.empty() ? "expected storage observation is missing"
                                          : "storage expectation matches multiple devices");
        } else {
            const StorageObservation& storage = *candidates.front();
            if (evaluateStorageInfo && expectation.type) {
                addCheck(evaluation,
                         "storage.type",
                         canonicalStorageType(storage.type) == expectation.type
                             ? ProfileCheckVerdict::Pass
                             : ProfileCheckVerdict::Fail,
                         *expectation.type,
                         storage.type,
                         canonicalStorageType(storage.type) == expectation.type
                             ? "storage type matches"
                             : "storage type differs");
            }
            if (evaluateStorageInfo &&
                (expectation.minimumCapacityBytes || expectation.maximumCapacityBytes)) {
                const auto& capacity = storage.capacityBytes;
                if (!capacity) {
                    addCheck(evaluation,
                             "storage.capacity_bytes",
                             ProfileCheckVerdict::Verify,
                             rangeText(expectation.minimumCapacityBytes,
                                       expectation.maximumCapacityBytes),
                             "missing",
                             "storage capacity observation is missing");
                } else {
                    const bool pass = checkRange(*capacity,
                                                 expectation.minimumCapacityBytes,
                                                 expectation.maximumCapacityBytes);
                    addCheck(evaluation,
                             "storage.capacity_bytes",
                             pass ? ProfileCheckVerdict::Pass : ProfileCheckVerdict::Fail,
                             rangeText(expectation.minimumCapacityBytes,
                                       expectation.maximumCapacityBytes),
                             numberText(*capacity),
                             pass ? "within configured range" : "outside configured range");
                }
            }
            const auto checkHealth = [&](const char* name,
                                         const auto& expected,
                                         const auto& observed) {
                if (!expected)
                    return;
                if (!observed) {
                    addCheck(evaluation,
                             name,
                             ProfileCheckVerdict::Verify,
                             "<=" + numberText(*expected),
                             "missing",
                             "storage health observation is missing");
                    return;
                }
                const bool pass = *observed <= *expected;
                addCheck(evaluation,
                         name,
                         pass ? ProfileCheckVerdict::Pass : ProfileCheckVerdict::Fail,
                         "<=" + numberText(*expected),
                         numberText(*observed),
                         pass ? "within configured health limit" : "above configured health limit");
            };
            if (evaluateStorageHealth) {
                checkHealth("storage.health.pre_eol_info",
                            expectation.health.maximumPreEolInfo,
                            storage.health.preEolInfo);
                checkHealth("storage.health.life_time_estimation_a",
                            expectation.health.maximumLifeTimeEstimationA,
                            storage.health.lifeTimeEstimationA);
                checkHealth("storage.health.life_time_estimation_b",
                            expectation.health.maximumLifeTimeEstimationB,
                            storage.health.lifeTimeEstimationB);
            }
        }
    }
    if (evaluation.checks.empty()) {
        evaluation.reason = "unique Board Profile has no configured observations to evaluate";
        addCheck(evaluation,
                 "profile.expectations",
                 ProfileCheckVerdict::Verify,
                 "at least one expectation",
                 "none",
                 evaluation.reason);
        return evaluation;
    }
    bool hasVerify = false;
    for (const auto& check : evaluation.checks) {
        if (check.verdict == ProfileCheckVerdict::Fail)
            evaluation.verdict = ProfileVerdict::Fail;
        if (check.verdict == ProfileCheckVerdict::Verify)
            hasVerify = true;
    }
    if (evaluation.verdict != ProfileVerdict::Fail && hasVerify) {
        evaluation.verdict = ProfileVerdict::Verify;
    } else if (evaluation.verdict != ProfileVerdict::Fail) {
        evaluation.verdict = ProfileVerdict::Pass;
    }
    evaluation.reason =
        evaluation.verdict == ProfileVerdict::Pass   ? "all configured Board Profile checks passed"
        : evaluation.verdict == ProfileVerdict::Fail ? "one or more Board Profile checks failed"
                                                     : "Board Profile checks require verification";
    return evaluation;
}

ProfileEvaluation evaluateBoardProfile(const BoardIdentity& identity,
                                       const BoardObservations& observations,
                                       const std::vector<Profile>& profiles)
{
    return evaluateBoardProfileForCapability(identity, observations, profiles, {});
}

bool parseBoardProfile(const JsonValue& document, Profile& profile, std::string& error)
{
    error.clear();
    if (!document.isObject() ||
        !hasOnlyKeys(document.objectItems(),
                     {"profile_id", "profile_version", "identity", "memory", "storage"},
                     error)) {
        if (error.empty())
            error = "Board Profile must be an object";
        return false;
    }
    Profile parsed;
    if (!stringValue(member(document, "profile_id"), parsed.profileId) ||
        parsed.profileId.empty() || parsed.profileId.size() > MaximumProfileIdBytes) {
        error = "profile_id must be a bounded non-empty string";
        return false;
    }
    if (!positiveInt(member(document, "profile_version"), parsed.profileVersion)) {
        error = "profile_version must be a positive integer";
        return false;
    }
    const auto* identity = member(document, "identity");
    if (!identity || !identity->isObject() || identity->objectItems().empty() ||
        identity->objectItems().size() > MaximumIdentityFields) {
        error = "profile identity must be a bounded non-empty object";
        return false;
    }
    for (const auto& entry : identity->objectItems()) {
        if (!validKey(entry.first, MaximumIdentityKeyBytes) || !entry.second.isString() ||
            entry.second.stringValue().empty() ||
            entry.second.stringValue().size() > MaximumIdentityValueBytes) {
            error = "profile identity contains an invalid exact field";
            return false;
        }
        parsed.identityEquals.emplace(entry.first, entry.second.stringValue());
    }
    if (const auto* memory = member(document, "memory")) {
        if (!parseMemory(*memory, parsed, error))
            return false;
    }
    if (const auto* storage = member(document, "storage")) {
        if (!parseStorage(*storage, parsed, error))
            return false;
    }
    if (!parsed.memory && !parsed.storage) {
        error = "Board Profile must define memory or storage expectations";
        return false;
    }
    profile = std::move(parsed);
    return true;
}

BoardProfileLoadResult loadBoardProfiles(const std::filesystem::path& directory,
                                         const BoardProfileLoaderLimits& limits)
{
    BoardProfileLoadResult result;
    std::error_code statusError;
    const auto status = std::filesystem::symlink_status(directory, statusError);
    if (statusError || !std::filesystem::is_directory(status) ||
        std::filesystem::is_symlink(status)) {
        result.errors.push_back({directory, "Board Profile directory is unavailable"});
        return result;
    }
    std::error_code iteratorError;
    std::filesystem::directory_iterator iterator(
        directory, std::filesystem::directory_options::skip_permission_denied, iteratorError);
    std::set<std::string> profileIds;
    std::size_t fileCount = 0;
    std::size_t totalBytes = 0;
    const std::filesystem::directory_iterator end;
    for (; !iteratorError && iterator != end; iterator.increment(iteratorError)) {
        const auto& entry = *iterator;
        const auto path = entry.path();
        if (path.extension() != ".json")
            continue;
        if (++fileCount > limits.maxFiles) {
            result.errors.push_back({path, "Board Profile file limit exceeded"});
            break;
        }
        const auto entryStatus = std::filesystem::symlink_status(path, statusError);
        if (statusError || std::filesystem::is_symlink(entryStatus) ||
            !std::filesystem::is_regular_file(entryStatus)) {
            result.errors.push_back({path, "Board Profile file is not a regular non-symlink file"});
            continue;
        }
        const auto size = std::filesystem::file_size(path, statusError);
        if (statusError || size > limits.maxFileBytes || totalBytes > limits.maxTotalBytes ||
            size > limits.maxTotalBytes - totalBytes) {
            result.errors.push_back({path, "Board Profile file exceeds the configured byte limit"});
            continue;
        }
        std::ifstream input(path, std::ios::binary);
        std::string content(static_cast<std::size_t>(size), '\0');
        if (size != 0 &&
            (!input || !input.read(content.data(), static_cast<std::streamsize>(size)))) {
            result.errors.push_back({path, "Board Profile file could not be read"});
            continue;
        }
        totalBytes += static_cast<std::size_t>(size);
        JsonValue document;
        JsonError jsonError;
        if (!parseJson(content, document, jsonError, limits.jsonLimits)) {
            result.errors.push_back({path, "invalid Board Profile JSON: " + jsonError.message});
            continue;
        }
        Profile profile;
        std::string profileError;
        if (!parseBoardProfile(document, profile, profileError)) {
            result.errors.push_back({path, profileError});
            continue;
        }
        if (result.profiles.size() >= limits.maxProfiles ||
            !profileIds.insert(profile.profileId).second) {
            result.errors.push_back({path, "duplicate or excessive Board Profile id"});
            continue;
        }
        result.profiles.push_back(std::move(profile));
    }
    if (iteratorError) {
        result.errors.push_back({directory, "Board Profile directory enumeration failed"});
    }
    std::sort(
        result.profiles.begin(),
        result.profiles.end(),
        [](const Profile& left, const Profile& right) { return left.profileId < right.profileId; });
    result.ok = result.errors.empty();
    return result;
}

} // namespace testtools
