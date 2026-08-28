#pragma once

#include "process_runner.h"
#include "serial_transport.h"

#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif
#include <asio.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace serialdemo {

class AdbTransport final : public ISerialTransport {
public:
    explicit AdbTransport(std::string adbProgram = {});
    ~AdbTransport() override;

    std::vector<AdbDevice> availableDevices(std::string &error) const;
    std::vector<std::string> availableRemotePorts(const std::string &serial, std::string &error) const;

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
    std::string sttyCommand(const SerialConfig &config) const;
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
