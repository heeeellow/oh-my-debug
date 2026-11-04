#include "application/serial/serial_preferences.h"
#include "application/serial/serial_preferences_writer.h"
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace testtools {

struct SerialPreferencesWriter::Impl {
    std::filesystem::path path;
    Save save;
    mutable std::mutex mutex;
    std::condition_variable ready, idle;
    std::optional<SerialWorkspacePreferences> next;
    std::string lastError;
    bool writing = false, stopping = false;
    std::thread worker;

    Impl(std::filesystem::path p, Save s)
        : path(std::move(p)), save(std::move(s)), worker([this] { run(); })
    {
    }
    ~Impl()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }
        ready.notify_one();
        worker.join();
    }
    void run()
    {
        for (;;) {
            SerialWorkspacePreferences snapshot;
            {
                std::unique_lock<std::mutex> lock(mutex);
                ready.wait(lock, [this] { return stopping || next.has_value(); });
                if (!next)
                    return;
                snapshot = std::move(*next);
                next.reset();
                writing = true;
            }
            std::string error;
            try {
                if (!save(path, snapshot, error) && error.empty())
                    error = "串口设置保存失败";
            } catch (const std::exception& exception) {
                error = exception.what();
            } catch (...) {
                error = "串口设置保存失败";
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                lastError = std::move(error);
                writing = false;
                if (!next)
                    idle.notify_all();
            }
        }
    }
};

SerialPreferencesWriter::SerialPreferencesWriter(std::filesystem::path path, Save save)
    : impl_(std::make_unique<Impl>(std::move(path), std::move(save)))
{
}
SerialPreferencesWriter::~SerialPreferencesWriter() = default;
void SerialPreferencesWriter::submit(SerialWorkspacePreferences preferences)
{
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->next = std::move(preferences);
    }
    impl_->ready.notify_one();
}
bool SerialPreferencesWriter::pending() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->writing || impl_->next.has_value();
}
std::string SerialPreferencesWriter::error() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->lastError;
}
void SerialPreferencesWriter::waitIdle()
{
    std::unique_lock<std::mutex> lock(impl_->mutex);
    impl_->idle.wait(lock, [this] { return !impl_->writing && !impl_->next; });
}

} // namespace testtools
