#include "windows_toolbox_host.h"
#include "adapters/adb/process_runner.h"
#include <filesystem>
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#endif

namespace testtools {
WindowsToolboxHost::WindowsToolboxHost(std::string directory)
    : executable_((std::filesystem::u8path(directory) / "iperf3" / "iperf3.exe").u8string()) {}
JsonValue WindowsToolboxHost::interfaces()
{
    JsonValue::Array result;
#if defined(_WIN32)
    ULONG size = 16000;
    std::vector<unsigned char> buffer(size);
    ULONG status;
    for (int attempt = 0; attempt < 3; ++attempt) {
        status = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
        if (status != ERROR_BUFFER_OVERFLOW) break;
        if (size > 1024 * 1024) return JsonValue::object({{"interfaces", JsonValue::array()}});
        buffer.resize(size);
    }
    if (status != NO_ERROR) return JsonValue::object({{"interfaces", JsonValue::array()}});
    const auto utf8 = [](const wchar_t* text) {
        if (!text) return std::string{};
        const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
        if (size <= 1) return std::string{};
        std::string value(size, '\0'); WideCharToMultiByte(CP_UTF8, 0, text, -1, value.data(), size, nullptr, nullptr); value.resize(size - 1); return value;
    };
    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter; adapter = adapter->Next) {
        MIB_IF_ROW2 row{}; row.InterfaceLuid = adapter->Luid;
        const bool known = GetIfEntry2(&row) == NO_ERROR;
        const bool physical = adapter->IfType == IF_TYPE_ETHERNET_CSMACD && known && row.InterfaceAndOperStatusFlags.HardwareInterface;
        JsonValue::Array addresses;
        for (auto* address = adapter->FirstUnicastAddress; address; address = address->Next) {
            if (address->Address.lpSockaddr->sa_family != AF_INET) continue;
            char text[INET_ADDRSTRLEN]{};
            const auto* ipv4 = reinterpret_cast<sockaddr_in*>(address->Address.lpSockaddr);
            if (InetNtopA(AF_INET, const_cast<in_addr*>(&ipv4->sin_addr), text, sizeof(text))) addresses.push_back(JsonValue::string(text));
        }
        result.push_back(JsonValue::object({{"index", JsonValue::integer(adapter->IfIndex)},
            {"id", JsonValue::string(adapter->AdapterName)}, {"name", JsonValue::string(utf8(adapter->FriendlyName))},
            {"ipv4", JsonValue::array(std::move(addresses))}, {"physical", JsonValue::boolean(physical)},
            {"up", JsonValue::boolean(adapter->OperStatus == IfOperStatusUp)},
            {"speed_bps", known ? JsonValue::integer(static_cast<std::int64_t>(row.TransmitLinkSpeed)) : JsonValue::null()}}));
    }
#endif
    return JsonValue::object({{"interfaces", JsonValue::array(std::move(result))}});
}
bool WindowsToolboxHost::available(std::string& error)
{
    if (!std::filesystem::is_regular_file(std::filesystem::u8path(executable_))) { error = "随包网速工具不存在"; return false; }
    const auto result = ProcessRunner::run(executable_, {"--version"}, std::chrono::seconds(5));
    if (result.exitCode != 0 || result.standardOutput.find("iperf 3.20") == std::string::npos) { error = "随包网速工具版本或运行库不可用"; return false; }
    return true;
}
bool WindowsToolboxHost::validateRoute(std::uint32_t index, const std::string& source,
                                      const std::string& destination, std::string& error)
{
#if defined(_WIN32)
    const auto inventory = interfaces();
    bool exists = false;
    for (const auto& adapter : inventory.find("interfaces")->arrayItems()) {
        const auto* value = adapter.find("index"); const auto* physical = adapter.find("physical"); const auto* up = adapter.find("up");
        if (!value || value->int64Value() != index || !physical->boolValue() || !up->boolValue()) continue;
        for (const auto& address : adapter.find("ipv4")->arrayItems()) if (address.stringValue() == source) exists = true;
    }
    if (!exists) { error = "所选电脑以太网接口或源地址已不可用"; return false; }
    SOCKADDR_INET from{}, to{}, best{}; from.Ipv4.sin_family = AF_INET; to.Ipv4.sin_family = AF_INET;
    if (InetPtonA(AF_INET, source.c_str(), &from.Ipv4.sin_addr) != 1 || InetPtonA(AF_INET, destination.c_str(), &to.Ipv4.sin_addr) != 1) { error = "无效 IPv4 地址"; return false; }
    MIB_IPFORWARD_ROW2 route{};
    if (GetBestRoute2(nullptr, 0, &from, &to, 0, &route, &best) != NO_ERROR || route.InterfaceIndex != index) {
        error = "到板卡的路由未经过所选电脑网卡"; return false;
    }
    return true;
#else
    (void)index; (void)source; (void)destination; error = "仅支持 Windows 主机"; return false;
#endif
}
ToolProcessResult WindowsToolboxHost::run(const std::vector<std::string>& arguments, int timeoutSeconds,
                                         const std::atomic_bool& cancel,
                                         const std::function<bool(const char*, std::size_t)>& output)
{
    const auto result = ProcessRunner::runManaged(executable_, arguments, std::chrono::seconds(timeoutSeconds), cancel, {}, output);
    return {result.started, result.cancelled, result.outputTruncated || result.outputFailed, result.command.exitCode, result.command.standardError};
}
} // namespace testtools
