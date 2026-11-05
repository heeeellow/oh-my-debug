#include "core/device/loader_support.h"
#include <algorithm>
#include <sstream>
#include <vector>

namespace testtools {
const std::string& loaderProbeScript()
{
    static const std::string script = R"(echo OMD_LOADER_V1
echo PLATFORM
tr '\000' '\n' < /sys/firmware/devicetree/base/compatible || exit 1
echo ENDPLATFORM
n=0
for d in /sys/bus/platform/drivers/syscon-reboot-mode/*; do
 [ -d "$d/of_node" ] || continue
 n=$((n+1)); [ "$n" -le 16 ] || exit 1
 echo NODE
 tr '\000' '\n' < "$d/of_node/compatible" || exit 1
 echo MAGIC
 if [ -e "$d/of_node/mode-loader" ]; then od -An -tx1 "$d/of_node/mode-loader" || exit 1; fi
 echo ENDNODE
done
echo END)";
    return script;
}
LoaderSupport parseLoaderProbe(const std::string& output)
{
    const LoaderSupport invalid{LoaderSupportState::Unknown, "无法完整读取 Loader 支持信息，请检查设备权限", output.substr(0, 16384)};
    if (output.size() > 16384 || output.find('\0') != std::string::npos) return invalid;
    std::istringstream input(output); std::vector<std::string> lines; std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() > 512 || lines.size() >= 256) return invalid;
        lines.push_back(line);
    }
    std::size_t i = 0;
    auto take = [&](const char* value) { return i < lines.size() && lines[i++] == value; };
    if (!take("OMD_LOADER_V1") || !take("PLATFORM")) return invalid;
    bool rockchip = false; int platforms = 0;
    while (i < lines.size() && lines[i] != "ENDPLATFORM") {
        if (lines[i].rfind("rockchip,", 0) == 0) rockchip = true;
        ++i; ++platforms;
    }
    if (!platforms || !take("ENDPLATFORM")) return invalid;
    int nodes = 0, confirmed = 0;
    while (i < lines.size() && lines[i] == "NODE") {
        ++i; if (++nodes > 16) return invalid;
        bool compatible = false;
        while (i < lines.size() && lines[i] != "MAGIC") {
            if (lines[i] == "syscon-reboot-mode") compatible = true;
            ++i;
        }
        if (!take("MAGIC")) return invalid;
        std::string magic;
        while (i < lines.size() && lines[i] != "ENDNODE") { magic += lines[i++] + " "; }
        if (!take("ENDNODE")) return invalid;
        std::istringstream bytes(magic); std::string token, compact;
        while (bytes >> token) compact += token;
        // BSP BOOT_BL_DOWNLOAD = REBOOT_FLAG(0x5242c300) + 1, DT big endian.
        if (compatible && compact == "5242c301") ++confirmed;
    }
    if (!take("END") || i != lines.size()) return invalid;
    if (rockchip && confirmed == 1)
        return {LoaderSupportState::Supported, "已识别 Loader 重启接口；进入后需在烧录工具确认 USB 模式", output};
    return {LoaderSupportState::Unsupported, "当前固件未提供可确认的 Loader 重启接口", output};
}
} // namespace testtools
