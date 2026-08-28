#pragma once

#include "core/serial_types.h"

#include <string>
#include <vector>

namespace testtools {

struct DeviceScanResult {
    std::vector<serialdemo::AdbDevice> devices;
    std::string error;
};

class DeviceDiscovery {
public:
    DeviceScanResult scanAdbDevices() const;
};

} // namespace testtools
