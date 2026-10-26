#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace testtools {

// 宿主侧串口占用声明表：用于在跨工作区冲突时给出明确的页面占用提示，操作系统或板端 Agent 仍为最终裁决者。
class SerialPortClaims {
    struct State {
        std::mutex mutex;
        std::unordered_map<std::string, std::string> owners;
    };

  public:
    class Claim {
      public:
        ~Claim();
        Claim(const Claim&) = delete;
        Claim& operator=(const Claim&) = delete;

      private:
        friend class SerialPortClaims;
        Claim(std::shared_ptr<State> state, std::string key);
        std::shared_ptr<State> state_;
        std::string key_;
    };

    static std::string resourceKey(bool agent,
                                   const std::string& serial,
                                   const std::string& endpoint);
    std::shared_ptr<Claim> acquire(const std::string& key,
                                    const std::string& owner,
                                    std::string& error);

  private:
    std::shared_ptr<State> state_ = std::make_shared<State>();
};

} // namespace testtools
