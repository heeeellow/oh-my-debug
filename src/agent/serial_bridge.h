#pragma once

#include "core/serial_types.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace testtools::agent {

struct SerialEndpointInfo {
    std::string endpoint;
    std::string interfaceName;
    std::string occupiedBy;
    bool readable = false;
    bool writable = false;
};

std::vector<SerialEndpointInfo> listSerialEndpoints();

class SerialBridge {
public:
    SerialBridge() = default;
    ~SerialBridge();
    SerialBridge(const SerialBridge &) = delete;
    SerialBridge &operator=(const SerialBridge &) = delete;

    bool start(const serialdemo::SerialConfig &config, std::uint16_t &port,
               std::string &sessionId, int &effectiveBaudRate,
               bool &baudRateApplied, std::string &error);
    void stop();
    bool isRunning() const;

private:
    void workerMain();

    mutable std::mutex mutex_;
    serialdemo::SerialConfig config_;
    std::string sessionId_;
    std::thread worker_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> running_{false};
    int serialFd_ = -1;
    int listenFd_ = -1;
};

} // namespace testtools::agent
