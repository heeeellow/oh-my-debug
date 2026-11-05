#pragma once
#include "core/protocol/agent_connection.h"
#include "core/toolbox/toolbox_host.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

namespace testtools {
struct ToolboxSession {
    std::string serial;
    std::uint64_t generation = 0;
    std::function<std::shared_ptr<IAgentConnection>(std::string&)> openConnection;
};
struct NetworkTestOptions {
    std::uint32_t localInterface = 0;
    std::string sourceIp, boardInterface, boardIp, protocol = "tcp";
    int direction = 2, seconds = 10, parallel = 1, port = 5201, udpMbps = 10;
};
struct ToolboxRequest {
    ToolboxSession session;
    std::string capability;
    JsonValue arguments = JsonValue::object();
    NetworkTestOptions network;
    bool networkTest = false;
};
struct ToolboxSnapshot {
    std::uint64_t id = 0, generation = 0;
    std::string serial, capability, state = "idle", message, detail, instance, operation, job;
    bool active = false, cancelling = false, remotePending = false, complete = true;
    JsonValue progress = JsonValue::object(), report = JsonValue::object();
};
// 所有阻塞调用在 executePrepared/reconcile 的调用线程执行，界面只读快照和提交停止意图。
class ToolboxService {
  public:
    explicit ToolboxService(std::shared_ptr<IToolboxHost> host);
    bool prepare(ToolboxRequest request, bool conflict, std::string& error);
    void executePrepared();
    void cancel(std::uint64_t expectedId = 0);
    void extendHold(int seconds);
    bool reconcile(const ToolboxSession& session, std::string& error);
    ToolboxSnapshot snapshot() const;
    std::shared_ptr<IToolboxHost> host() const { return host_; }
  private:
    mutable std::mutex mutex_;
    std::shared_ptr<IToolboxHost> host_;
    const std::string owner_;
    ToolboxRequest request_;
    ToolboxSnapshot state_;
    std::atomic_bool cancel_{false};
    std::atomic<int> extend_{0};
    bool executing_ = false;
};
JsonValue toolboxRpc(IAgentConnection& connection, const std::string& method, const JsonValue& params);
std::string toolStageText(const std::string& phase);
} // namespace testtools
