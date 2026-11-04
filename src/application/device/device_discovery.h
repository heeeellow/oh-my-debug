#pragma once

#include "core/serial/serial_types.h"

#include <string>
#include <vector>

namespace testtools {

// ADB 在线与离线设备扫描用例服务。
struct DeviceScanResult {
    std::vector<testtools::AdbDevice> devices;
    std::string error;
};

class DeviceDiscovery {
public:
    DeviceScanResult scanAdbDevices() const;
};

} // namespace testtools
