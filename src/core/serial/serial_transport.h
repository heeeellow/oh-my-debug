#pragma once

#include "core/serial/serial_types.h"

#include <functional>
#include <mutex>
#include <string>

namespace testtools {

// 串口传输状态事件枚举。
enum class TransportState {
    Progress,
    Connected,
    Error,
    Closed
};

enum class SerialControlLine { Dtr, Rts };

// 串口传输抽象接口：统一封装本机 COM、板端 Agent 串口流与 ADB 串口桥接。
class ISerialTransport {
public:
    using DataCallback = std::function<void(const ByteBuffer &)>;
    using StatusCallback = std::function<void(TransportState state, const std::string &message)>;

    virtual ~ISerialTransport() = default;

    virtual void open(const SerialConfig &config) = 0;
    virtual void close() = 0;
    virtual bool write(const ByteBuffer &data) = 0;
    virtual bool isOpen() const = 0;
    virtual bool supportsControlLine(SerialControlLine) const { return false; }
    virtual bool setControlLine(SerialControlLine, bool, std::string &error)
    {
        error = "当前串口不支持手动控制 DTR/RTS";
        return false;
    }

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

} // namespace testtools
