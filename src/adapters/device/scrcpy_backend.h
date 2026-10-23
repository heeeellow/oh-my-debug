#pragma once

#include "core/device/screen_mirror.h"

#include <memory>
#include <string>

namespace testtools {

// Explicit executable paths are useful for host fixtures; production uses bundled tools.
std::unique_ptr<IScreenMirrorBackend> makeScrcpyBackend(std::string scrcpy = {}, std::string adb = {});

} // namespace testtools
