#pragma once
#include "board_capability.h"
#include <functional>
#include <memory>

namespace testtools::agent {
// 生产环境固定 /sys；替换根目录仅用于无硬件文件系统夹具，不接受 RPC 路径。
class BoardGpioRegistry {
  public:
    using AttributeStore = std::function<std::int64_t(int, const std::string&)>;
    explicit BoardGpioRegistry(std::string sysRoot = "/sys", AttributeStore store = {});
    ~BoardGpioRegistry();
    JsonValue inventory(const JsonValue& nativeInventory);
    BoardTestRunResult run(const BoardTestRunRequest&, const std::atomic_bool&,
                          const std::function<JsonValue()>& nativeInventory);
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace testtools::agent
