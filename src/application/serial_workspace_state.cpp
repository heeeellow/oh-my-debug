#include "serial_workspace_state.h"

#include <algorithm>

namespace testtools {

SerialWorkspaceMode SerialWorkspaceState::mode() const
{
    return mode_;
}

SerialModeSwitchDecision SerialWorkspaceState::evaluateModeSwitch(
    SerialWorkspaceMode requested, const SerialWorkspaceActivity &activity) const
{
    if (requested == mode_) {
        return {};
    }
    const bool scheduledSendRunning = std::any_of(
        activity.scheduledSendRunning.begin(), activity.scheduledSendRunning.end(),
        [](bool running) { return running; });
    if (scheduledSendRunning || activity.bidirectionalTestRunning
        || activity.reliabilityTestRunning) {
        return {false, SerialModeSwitchBlock::TaskRunning};
    }
    // Channel A is hidden in single-port mode. Never hide a live connection.
    if (requested == SerialWorkspaceMode::Single && activity.channelActive[0]) {
        return {false, SerialModeSwitchBlock::HiddenChannelActive};
    }
    return {};
}

bool SerialWorkspaceState::applyMode(SerialWorkspaceMode requested,
                                     const SerialWorkspaceActivity &activity)
{
    if (!evaluateModeSwitch(requested, activity).allowed) {
        return false;
    }
    mode_ = requested;
    return true;
}

SerialEndpointConflict SerialWorkspaceState::endpointConflict(
    const SerialEndpointSelection &first, const SerialEndpointSelection &second)
{
    if (!first.active || !second.active) {
        return SerialEndpointConflict::None;
    }
    if (first.kind == SerialEndpointKind::Agent
        && second.kind == SerialEndpointKind::Agent) {
        return SerialEndpointConflict::AgentSingleSession;
    }
    if (first.kind == second.kind && first.endpoint == second.endpoint
        && (first.kind == SerialEndpointKind::Local
            || first.deviceSerial == second.deviceSerial)) {
        return SerialEndpointConflict::SameResource;
    }
    return SerialEndpointConflict::None;
}

} // namespace testtools
