#pragma once
#include "core/device/device_maintenance.h"

namespace testtools {
// Runtime BSP evidence is inspected on a worker, with no device writes.
std::shared_ptr<IDeviceMaintenanceBackend>
createAdbDeviceMaintenanceBackend(const std::string& adbProgram = {});
} // namespace testtools
