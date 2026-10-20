#pragma once

#include "board_test_job.h"
#include "native_board_observations.h"

#include <atomic>
#include <string>
#include <vector>

namespace testtools::agent {

struct BoardCapabilityDefinition {
    std::string id;
    std::string name;
    int version = 1;
    BoardTestProviderIdentity provider;
    std::vector<std::string> risks;
    std::vector<std::string> resources;
    std::string observationSchema;
    std::string recovery;
    int defaultTimeoutMs = 0;
    bool supportsCancel = false;
};

std::vector<BoardCapabilityDefinition>
availableBoardCapabilities(const NativeObservationRoots& roots = {});

BoardTestRunResult runBoardCapability(const BoardTestRunRequest& request,
                                      const std::atomic_bool& cancelRequested,
                                      const NativeObservationRoots& roots = {});

} // namespace testtools::agent
