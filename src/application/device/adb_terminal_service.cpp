#include "application/device/adb_terminal_service.h"

#include "adapters/adb/adb_transport.h"
#include <charconv>

namespace testtools {

int AdbTerminalService::shellUid(const serialdemo::AdbDevice& device) const
{
    if (device.serial.empty() || device.state != "device")
        return -1;
    serialdemo::AdbTransport transport;
    const auto result =
        transport.runCommand(device.serial, {"shell", "-T", "id -u"}, std::chrono::seconds(2));
    if (result.exitCode != 0 || result.timedOut)
        return -1;
    const auto begin = result.standardOutput.find_first_not_of(" \r\n\t");
    if (begin == std::string::npos)
        return -1;
    const auto end = result.standardOutput.find_last_not_of(" \r\n\t") + 1;
    int uid = -1;
    const auto parsed = std::from_chars(
        result.standardOutput.data() + begin, result.standardOutput.data() + end, uid);
    return parsed.ec == std::errc{} && parsed.ptr == result.standardOutput.data() + end && uid >= 0
               ? uid
               : -1;
}

AdbTerminalResult AdbTerminalService::open(const serialdemo::AdbDevice &device) const
{
    AdbTerminalResult result;
    if (device.serial.empty() || device.state != "device") {
        result.error = "请先选择在线且已授权的 ADB 设备";
        return result;
    }
    // 打开交互式 Shell 终端无需准备或依赖板端 Agent。
    serialdemo::AdbTransport transport;
    result.effectiveUid = shellUid(device);
    result.started = transport.openShellTerminal(device.serial, result.host, result.error);
    return result;
}

} // namespace testtools
