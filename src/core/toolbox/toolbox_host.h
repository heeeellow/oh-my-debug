#pragma once
#include "core/protocol/json_value.h"
#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace testtools {
struct ToolProcessResult {
    bool started = false, cancelled = false, truncated = false;
    int exitCode = -1;
    std::string error;
};
class IToolboxHost {
  public:
    virtual ~IToolboxHost() = default;
    virtual JsonValue interfaces() = 0;
    virtual bool available(std::string& error) = 0;
    virtual bool validateRoute(std::uint32_t interfaceIndex, const std::string& source,
                               const std::string& destination, std::string& error) = 0;
    virtual ToolProcessResult run(const std::vector<std::string>& arguments, int timeoutSeconds,
                                  const std::atomic_bool& cancel,
                                  const std::function<bool(const char*, std::size_t)>& output) = 0;
};
} // namespace testtools
