#pragma once
#include "application/agent/board_test_job_client.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <vector>

namespace testtools {
enum class BoardInfoState { NotRead, Unavailable, Reading, Completed, Failed, Cancelled, TimedOut };
struct BoardInfoField {
    std::string group, name, value, note;
};
struct BoardInfoRecord {
    std::string capability;
    BoardInfoState state = BoardInfoState::NotRead;
    std::vector<BoardInfoField> fields;
    std::string summary, error;
    std::optional<BoardTestJobSnapshot> job;
    std::int64_t finishedAtMs = 0;
};
struct BoardInfoSession {
    std::string serial;
    std::uint64_t generation = 0;
    std::shared_ptr<IAgentConnection> connection;
    std::vector<std::string> capabilities;
    std::shared_ptr<std::atomic<bool>> alive;
    // 实际桌面在工作线程打开独立控制连接，避免与主窗口关闭/重连共享 socket。
    std::function<std::shared_ptr<IAgentConnection>(std::string&)> openConnection;
};
struct BoardInfoRequest {
    BoardInfoSession session;
    std::vector<std::string> capabilities;
    std::chrono::milliseconds timeout{10000}, pollInterval{80}, cancelGrace{3000};
};
struct BoardInfoSnapshot {
    std::uint64_t id = 0, generation = 0;
    std::string serial, current, message;
    bool active = false, cancelling = false, remotePending = false;
    std::vector<BoardInfoRecord> records;
};

// 三个固定只读能力的顺序读取；不做规格匹配或合格判定。
class BoardInfoService {
  public:
    static const std::vector<std::string>& capabilities();
    bool prepare(BoardInfoRequest request, bool maintenanceBusy, std::string& error);
    void executePrepared();
    void cancel(std::uint64_t expectedId = 0) noexcept;
    BoardInfoSnapshot snapshot() const;

  private:
    mutable std::mutex mutex_;
    std::atomic<bool> cancel_{false};
    bool executing_ = false;
    BoardInfoRequest request_;
    BoardInfoSnapshot state_;
};
// 严格按已知观察值 schema 展示；缺失/无效字段保持未知，原始结果留在 record.job。
bool decodeBoardInfo(const std::string& capability,
                     const BoardTestResult& result,
                     std::vector<BoardInfoField>& fields,
                     std::string& error);
} // namespace testtools
