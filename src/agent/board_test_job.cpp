#include "board_test_job.h"
#include "toolbox.h"

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace testtools::agent {

namespace {

constexpr std::size_t MaxSummaryBytes = 1024;
constexpr std::size_t MaxLogBytes = 64U * 1024U;
constexpr std::size_t MaxIdentityBytes = 128;
constexpr std::size_t MaxObservationBytes = 256U * 1024U;

} // namespace

BoardTestJobManager::BoardTestJobManager(RunFunction runFunction)
    : runFunction_(std::move(runFunction))
{
    if (!runFunction_) {
        throw std::invalid_argument("BoardTestJobManager requires a run function");
    }
}

BoardTestJobManager::~BoardTestJobManager()
{
    stop();
}

std::optional<BoardTestJobSnapshot> BoardTestJobManager::start(const BoardTestRunRequest& request)
{
    if (!isValidCapabilityId(request.capabilityId)) {
        return std::nullopt;
    }

    // 已执行完毕的 std::thread 仍处于 joinable 状态；先标记回收区间（joining_），
    // 防止并发调用方在 join 期间抢先启动第二个任务。
    std::thread completedWorker;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (active_ || joining_) {
            return std::nullopt;
        }
        joining_ = true;
        completedWorker = std::move(worker_);
    }
    if (completedWorker.joinable()) {
        completedWorker.join();
    }
    finishJoining();

    std::unique_lock<std::mutex> lock(mutex_);
    if (active_ || joining_) {
        return std::nullopt;
    }

    const auto jobId = nextJobId_++;
    auto cancelToken = std::make_shared<std::atomic_bool>(false);
    current_ = BoardTestJobSnapshot{
        jobId,
        request.capabilityId,
        request.capabilityVersion,
        BoardTestJobState::running,
        {},
    };
    active_ = true;
    cancelRequested_ = cancelToken;

    try {
        worker_ = std::thread([this, request, jobId, cancelToken] {
            BoardTestRunResult result;
            try {
                result = runFunction_(request, *cancelToken);
            } catch (const std::exception& error) {
                result.summary = std::string("runner exception: ") + error.what();
                result.exitCode = -1;
            } catch (...) {
                result.summary = "runner exception";
                result.exitCode = -1;
            }
            finish(jobId, std::move(result));
            if (request.toolbox) {
                if (const auto state = snapshot(jobId)) request.toolbox->finish(*state);
            }
        });
    } catch (...) {
        active_ = false;
        cancelRequested_.reset();
        current_->state = BoardTestJobState::failed;
        current_->result.summary = "worker creation failed";
        current_->result.exitCode = -1;
        stateChanged_.notify_all();
    }
    return current_;
}

std::optional<BoardTestJobSnapshot> BoardTestJobManager::cancel(std::uint64_t jobId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_ || !current_ || current_->jobId != jobId || !cancelRequested_) {
        return std::nullopt;
    }
    cancelRequested_->store(true);
    current_->state = BoardTestJobState::cancelling;
    return current_;
}

std::optional<BoardTestJobSnapshot> BoardTestJobManager::snapshot(std::uint64_t jobId) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!current_ || current_->jobId != jobId) {
        return std::nullopt;
    }
    return current_;
}

std::optional<BoardTestJobSnapshot> BoardTestJobManager::currentSnapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return current_;
}

void BoardTestJobManager::stop()
{
    std::thread workerToJoin;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        stateChanged_.wait(lock, [this] { return !joining_; });
        if (active_ && current_ && cancelRequested_) {
            cancelRequested_->store(true);
            current_->state = BoardTestJobState::cancelling;
        }
        joining_ = true;
        workerToJoin = std::move(worker_);
    }
    if (workerToJoin.joinable()) {
        workerToJoin.join();
    }
    finishJoining();
}

bool BoardTestJobManager::isValidCapabilityId(const std::string& capabilityId)
{
    if (capabilityId.empty() || capabilityId.size() > 128) {
        return false;
    }
    for (const unsigned char character : capabilityId) {
        if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '_' || character == '-' ||
            character == '.' || character == ':') {
            continue;
        }
        return false;
    }
    return capabilityId != "." && capabilityId != "..";
}

std::string BoardTestJobManager::truncate(const std::string& value, std::size_t maximum)
{
    return value.size() <= maximum ? value : value.substr(0, maximum);
}

BoardTestJobState BoardTestJobManager::finalState(const BoardTestRunResult& result,
                                                  bool cancellationRequested)
{
    if (result.timedOut) {
        return BoardTestJobState::timed_out;
    }
    if (result.cancelled || cancellationRequested) {
        return BoardTestJobState::cancelled;
    }
    return result.succeeded ? BoardTestJobState::succeeded : BoardTestJobState::failed;
}

void BoardTestJobManager::finish(std::uint64_t jobId, BoardTestRunResult result)
{
    result.summary = truncate(result.summary, MaxSummaryBytes);
    result.log = truncate(result.log, MaxLogBytes);
    result.provider.kind = truncate(result.provider.kind, MaxIdentityBytes);
    result.provider.id = truncate(result.provider.id, MaxIdentityBytes);
    result.observationSchema = truncate(result.observationSchema, MaxIdentityBytes);
    if (result.providerClaim) {
        *result.providerClaim = truncate(*result.providerClaim, MaxIdentityBytes);
    }
    JsonLimits limits;
    limits.max_output_bytes = MaxObservationBytes;
    limits.max_string_bytes = MaxLogBytes;
    limits.max_array_items = 256;
    limits.max_object_members = 256;
    JsonError observationError;
    std::string encodedObservations;
    if (result.succeeded &&
        (result.provider.kind.empty() || result.provider.id.empty() ||
         result.provider.version <= 0 || result.observationSchema.empty() ||
         !result.observations.isObject() ||
         !serializeJson(result.observations, encodedObservations, observationError, limits))) {
        result.succeeded = false;
        result.exitCode = -1;
        result.summary = "provider returned an invalid structured observation result";
        result.observations = JsonValue::object();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!current_ || current_->jobId != jobId) {
        return;
    }
    // 将任务完成与 cancel() 线性化：确保在锁外校验结果期间已被接受的取消请求不会被遗漏。
    current_->state = finalState(result, cancelRequested_ && cancelRequested_->load());
    result.succeeded = current_->state == BoardTestJobState::succeeded;
    result.cancelled = current_->state == BoardTestJobState::cancelled;
    result.timedOut = current_->state == BoardTestJobState::timed_out;
    current_->result = std::move(result);
    active_ = false;
    cancelRequested_.reset();
    stateChanged_.notify_all();
}

void BoardTestJobManager::finishJoining() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    joining_ = false;
    stateChanged_.notify_all();
}

const char* boardTestJobStateName(BoardTestJobState state) noexcept
{
    switch (state) {
    case BoardTestJobState::running:
        return "running";
    case BoardTestJobState::cancelling:
        return "cancelling";
    case BoardTestJobState::succeeded:
        return "succeeded";
    case BoardTestJobState::failed:
        return "failed";
    case BoardTestJobState::cancelled:
        return "cancelled";
    case BoardTestJobState::timed_out:
        return "timed_out";
    }
    return "failed";
}

} // namespace testtools::agent
