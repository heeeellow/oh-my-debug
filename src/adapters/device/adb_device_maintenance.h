#pragma once
#include "core/device/device_maintenance.h"
#include <set>

namespace testtools {
// 默认没有经过 BSP/实机确认的 Loader 目标；名称含 rk/rockchip 不构成支持依据。
std::shared_ptr<IDeviceMaintenanceBackend>
createAdbDeviceMaintenanceBackend(const std::string& adbProgram = {},
                                  const std::set<std::string>& confirmedLoaderSerials = {});
} // namespace testtools
