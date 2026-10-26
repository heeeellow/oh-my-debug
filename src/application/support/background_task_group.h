#pragma once

#include <cstddef>
#include <functional>
#include <future>
#include <vector>

namespace testtools {

// 管理由表现层发起的短生命周期后台异步任务：
// 每次接受新任务前自动回收已完成任务，并通过 wait() 提供确定性的退出等待边界。
class BackgroundTaskGroup {
public:
    using Task = std::function<void()>;

    BackgroundTaskGroup() = default;
    ~BackgroundTaskGroup();
    BackgroundTaskGroup(const BackgroundTaskGroup &) = delete;
    BackgroundTaskGroup &operator=(const BackgroundTaskGroup &) = delete;

    void start(Task task);
    void reapFinished();
    void wait();
    std::size_t retainedTaskCount() const;

private:
    static void consume(std::future<void> &task) noexcept;

    std::vector<std::future<void>> tasks_;
};

} // namespace testtools
