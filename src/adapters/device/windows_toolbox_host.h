#pragma once
#include "core/toolbox/toolbox_host.h"

namespace testtools {
class WindowsToolboxHost final : public IToolboxHost {
  public:
    explicit WindowsToolboxHost(std::string applicationDirectory);
    JsonValue interfaces() override;
    bool available(std::string& error) override;
    bool validateRoute(std::uint32_t index, const std::string& source, const std::string& destination, std::string& error) override;
    ToolProcessResult run(const std::vector<std::string>& arguments, int timeoutSeconds,
                          const std::atomic_bool& cancel,
                          const std::function<bool(const char*, std::size_t)>& output) override;
  private: std::string executable_;
};
} // namespace testtools
