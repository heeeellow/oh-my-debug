#pragma once

#include "serial_types.h"

#include <functional>
#include <mutex>
#include <string>

namespace serialdemo {

enum class TransportState {
    Progress,
    Connected,
    Error,
    Closed
};

class ISerialTransport {
public:
    using DataCallback = std::function<void(const ByteBuffer &)>;
    using StatusCallback = std::function<void(TransportState state, const std::string &message)>;

    virtual ~ISerialTransport() = default;

    virtual void open(const SerialConfig &config) = 0;
    virtual void close() = 0;
    virtual bool write(const ByteBuffer &data) = 0;
    virtual bool isOpen() const = 0;

    void setDataCallback(DataCallback callback)
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        dataCallback_ = std::move(callback);
    }
    void setStatusCallback(StatusCallback callback)
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        statusCallback_ = std::move(callback);
    }

protected:
    void publishData(const ByteBuffer &data) const
    {
        DataCallback callback;
        {
            std::lock_guard<std::mutex> lock(callbackMutex_);
            callback = dataCallback_;
        }
        if (callback) {
            callback(data);
        }
    }

    void publishStatus(TransportState state, const std::string &message) const
    {
        StatusCallback callback;
        {
            std::lock_guard<std::mutex> lock(callbackMutex_);
            callback = statusCallback_;
        }
        if (callback) {
            callback(state, message);
        }
    }

private:
    mutable std::mutex callbackMutex_;
    DataCallback dataCallback_;
    StatusCallback statusCallback_;
};

} // namespace serialdemo
