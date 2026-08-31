#pragma once

#include <array>
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

} // namespace testtools
