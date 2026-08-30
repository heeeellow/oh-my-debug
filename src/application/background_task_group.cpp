#include "background_task_group.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace testtools {

BackgroundTaskGroup::~BackgroundTaskGroup()
{
    wait();
}

void BackgroundTaskGroup::start(Task task)
{
    reapFinished();
    tasks_.push_back(std::async(std::launch::async, [task = std::move(task)] {
        if (task) {
            task();
        }
    }));
}

void BackgroundTaskGroup::reapFinished()
{
    const auto ready = [](std::future<void> &task) {
        if (task.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            return false;
        }
        consume(task);
        return true;
    };
    tasks_.erase(std::remove_if(tasks_.begin(), tasks_.end(), ready), tasks_.end());
}

void BackgroundTaskGroup::wait()
{
    for (auto &task : tasks_) {
        consume(task);
    }
    tasks_.clear();
}

std::size_t BackgroundTaskGroup::retainedTaskCount() const
{
    return tasks_.size();
}

void BackgroundTaskGroup::consume(std::future<void> &task) noexcept
{
    if (!task.valid()) {
        return;
    }
    try {
        task.get();
    } catch (...) {
        // Boundary tasks report user-facing failures through their normal
        // completion callbacks. Never let an abandoned exception terminate
        // shutdown or a later refresh.
    }
}

} // namespace testtools
