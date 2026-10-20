#include "application/device/adb_terminal_service.h"

#include "adapters/adb/adb_transport.h"

namespace testtools {

AdbTerminalResult AdbTerminalService::open(const serialdemo::AdbDevice &device) const
{
    AdbTerminalResult result;
    if (device.serial.empty() || device.state != "device") {
        result.error = "请先选择在线且已授权的 ADB 设备";
        return result;
    }
    // Opening an interactive shell does not prepare or require the board Agent.
    serialdemo::AdbTransport transport;
    result.started = transport.openShellTerminal(device.serial, result.host, result.error);
    return result;
}

} // namespace testtools
