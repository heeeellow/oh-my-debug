#pragma once

#include "core/protocol/json_value.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace testtools::agent {

struct BoardTestRunRequest {
    std::string capabilityId;
    int capabilityVersion = 1;
};

struct BoardTestProviderIdentity {
    std::string kind;
    std::string id;
    int version = 0;
};

struct BoardTestRunResult {
    bool succeeded = false;
    bool cancelled = false;
    bool timedOut = false;
    int exitCode = -1;
    BoardTestProviderIdentity provider;
    std::string observationSchema;
    testtools::JsonValue observations = testtools::JsonValue::object();
    std::optional<std::string> providerClaim;
    std::string summary;
    std::string log;
};

enum class BoardTestJobState {
    running,
    cancelling,
    succeeded,
    failed,
    cancelled,
    timed_out,
};

struct BoardTestJobSnapshot {
    std::uint64_t jobId = 0;
    std::string capabilityId;
    int capabilityVersion = 1;
    BoardTestJobState state = BoardTestJobState::failed;
    BoardTestRunResult result;
};

class BoardTestJobManager {
  public:
    using RunFunction =
        std::function<BoardTestRunResult(const BoardTestRunRequest&, const std::atomic_bool&)>;

    explicit BoardTestJobManager(RunFunction runFunction);
    ~BoardTestJobManager();

    BoardTestJobManager(const BoardTestJobManager&) = delete;
    BoardTestJobManager& operator=(const BoardTestJobManager&) = delete;

    // 返回在状态锁下捕获的值快照；即使另一调用方在应答序列化前启动了新任务，当前应答依然有效。
    std::optional<BoardTestJobSnapshot> start(const BoardTestRunRequest& request);
    std::optional<BoardTestJobSnapshot> cancel(std::uint64_t jobId);
    std::optional<BoardTestJobSnapshot> snapshot(std::uint64_t jobId) const;
    std::optional<BoardTestJobSnapshot> currentSnapshot() const;

    // 请求取消并在释放状态锁后等待工作线程退出。
    void stop();

  private:
    static bool isValidCapabilityId(const std::string& capabilityId);
    static std::string truncate(const std::string& value, std::size_t maximum);
    static BoardTestJobState finalState(const BoardTestRunResult& result,
                                        bool cancellationRequested);
    void finish(std::uint64_t jobId, BoardTestRunResult result);
    void finishJoining() noexcept;

    mutable std::mutex mutex_;
    std::condition_variable stateChanged_;
    RunFunction runFunction_;
    std::thread worker_;
    std::shared_ptr<std::atomic_bool> cancelRequested_;
    std::optional<BoardTestJobSnapshot> current_;
    std::uint64_t nextJobId_ = 1;
    bool active_ = false;
    bool joining_ = false;
};

const char* boardTestJobStateName(BoardTestJobState state) noexcept;

} // namespace testtools::agent
