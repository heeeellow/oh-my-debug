#pragma once

#include "core/serial/serial_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>

namespace testtools {

enum class SerialWorkspaceMode {
    Single,
    Dual,
};

enum class SerialModeSwitchBlock {
    None,
    TaskRunning,
    HiddenChannelActive,
};

struct SerialWorkspaceActivity {
    std::array<bool, 2> channelActive{};
    std::array<bool, 2> scheduledSendRunning{};
    bool bidirectionalTestRunning = false;
    bool reliabilityTestRunning = false;
};

struct SerialModeSwitchDecision {
    bool allowed = true;
    SerialModeSwitchBlock blockedBy = SerialModeSwitchBlock::None;
};

enum class SerialEndpointKind {
    Local,
    Agent,
};

struct SerialEndpointSelection {
    bool active = false;
    SerialEndpointKind kind = SerialEndpointKind::Local;
    std::string endpoint;
    std::string deviceSerial;
};

enum class SerialEndpointConflict {
    None,
    SameResource,
    AgentSingleSession,
};

class SerialWorkspaceState {
public:
    SerialWorkspaceMode mode() const;
    SerialModeSwitchDecision evaluateModeSwitch(
        SerialWorkspaceMode requested, const SerialWorkspaceActivity &activity) const;
    bool applyMode(SerialWorkspaceMode requested, const SerialWorkspaceActivity &activity);

    static SerialEndpointConflict endpointConflict(
        const SerialEndpointSelection &first, const SerialEndpointSelection &second);

private:
    SerialWorkspaceMode mode_ = SerialWorkspaceMode::Single;
};

enum class SerialLogDirection {
    Rx,
    Tx,
    Notice,
};

struct SerialLogEntry {
    SerialLogDirection direction = SerialLogDirection::Rx;
    std::int64_t timestampMs = 0;
    std::string timestampText;
    serialdemo::ByteBuffer bytes;
};

struct SerialLogAppendResult {
    std::size_t addedEntries = 0;
    bool trimmedOldest = false;
};

class SerialLogBuffer {
public:
    explicit SerialLogBuffer(std::size_t maxEntries = 2000,
                             std::size_t maxBytes = 512 * 1024);

    SerialLogAppendResult appendTx(const serialdemo::ByteBuffer &data,
                                   std::int64_t timestampMs,
                                   std::string timestampText);
    SerialLogAppendResult appendRx(const serialdemo::ByteBuffer &data,
                                   std::int64_t timestampMs,
                                   std::string timestampText);
    SerialLogAppendResult appendNotice(const std::string &text,
                                       std::int64_t timestampMs = 0,
                                       std::string timestampText = {});
    void clear();

    bool empty() const noexcept;
    std::size_t size() const noexcept;
    std::size_t totalBytes() const noexcept;
    bool truncated() const noexcept;
    const std::deque<SerialLogEntry> &entries() const noexcept;

    static std::string formatEntry(const SerialLogEntry &entry,
                                   bool hexDisplay,
                                   bool showTimestamp);
    std::string formatAll(bool hexDisplay, bool showTimestamp,
                          const std::string &filter = {}) const;

private:
    bool trimToLimits();

    std::deque<SerialLogEntry> entries_;
    std::size_t totalBytes_ = 0;
    std::size_t maxEntries_ = 2000;
    std::size_t maxBytes_ = 512 * 1024;
    bool truncated_ = false;
};

} // namespace testtools
