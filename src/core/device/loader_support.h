#pragma once
#include <string>

namespace testtools {
enum class LoaderSupportState { Unknown, Checking, Supported, Unsupported, Unavailable };
struct LoaderSupport {
    LoaderSupportState state = LoaderSupportState::Unknown;
    std::string reason = "尚未确认当前固件是否支持 Loader", evidence;
};
// Fixed, read-only probe. Never writes boot flags or requests privilege changes.
const std::string& loaderProbeScript();
LoaderSupport parseLoaderProbe(const std::string& output);
} // namespace testtools
