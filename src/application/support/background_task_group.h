#pragma once

#include <cstddef>
#include <functional>
#include <future>
#include <vector>

namespace testtools {

// Owns short-lived background work launched by the presentation layer.
// Completed tasks are reaped before new work is accepted, while wait() gives
// shutdown a deterministic boundary.
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
