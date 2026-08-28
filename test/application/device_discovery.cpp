#include "device_discovery.h"

#include "core/adb_transport.h"

namespace testtools {

DeviceScanResult DeviceDiscovery::scanAdbDevices() const
{
    DeviceScanResult result;
    serialdemo::AdbTransport transport;
    result.devices = transport.availableDevices(result.error);
    return result;
}

} // namespace testtools
