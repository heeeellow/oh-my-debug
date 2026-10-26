#pragma once

#include "core/device/screen_mirror.h"

#include <memory>
#include <string>

namespace testtools {

// 显式可执行文件路径便于宿主测试夹具注入；生产环境默认解析随包工具目录。
std::unique_ptr<IScreenMirrorBackend> makeScrcpyBackend(std::string scrcpy = {}, std::string adb = {});

} // namespace testtools
