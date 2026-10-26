#pragma once

#include "adapters/adb/process_runner.h"
#include "core/serial/serial_transport.h"

#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif
#include <asio.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace serialdemo {

// ADB 命令行与设备节点转发适配器：负责 ADB 程序解析、设备枚举、远端命令执行及备用串口桥接。
class AdbTransport final : public ISerialTransport {
public:
    explicit AdbTransport(std::string adbProgram = {});
    ~AdbTransport() override;

    std::vector<AdbDevice> availableDevices(std::string &error) const;
    std::vector<std::string> availableRemotePorts(const std::string &serial, std::string &error) const;
    CommandResult runCommand(const std::string &serial,
                             const std::vector<std::string> &arguments,
                             std::chrono::milliseconds timeout = std::chrono::seconds(5)) const;
    bool openShellTerminal(const std::string &serial, std::string &host, std::string &error) const;
    const std::string& program() const noexcept { return adbProgram_; }

    void open(const SerialConfig &config) override;
    void close() override;
    bool write(const ByteBuffer &data) override;
    bool isOpen() const override;

private:
    CommandResult runAdb(const std::string &serial,
                         const std::vector<std::string> &arguments,
                         std::chrono::milliseconds timeout = std::chrono::seconds(5)) const;
    bool validateConfig(const SerialConfig &config, std::string &error) const;
    bool ensureEndpointAccess(const SerialConfig &config, std::string &error);
    bool ensureEndpointNotBusy(const SerialConfig &config, std::string &error) const;
    std::string sttyCommand(const SerialConfig &config, bool includeBaudRate = true) const;
    void workerMain(SerialConfig config);
    void removeForward(const std::string &serial, int localPort) const;

    std::string adbProgram_;
    std::atomic<bool> open_{false};
    std::atomic<bool> stopping_{false};
    std::thread worker_;
    mutable std::mutex socketMutex_;
    std::unique_ptr<asio::io_context> ioContext_;
    std::unique_ptr<asio::ip::tcp::socket> socket_;
};

} // namespace serialdemo
