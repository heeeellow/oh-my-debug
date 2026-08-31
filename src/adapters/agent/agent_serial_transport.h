#pragma once

#include "core/serial/serial_transport.h"

#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif
#include <asio.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace serialdemo {

class AgentSerialTransport final : public ISerialTransport {
public:
    AgentSerialTransport(std::string adbSerial, std::uint16_t agentControlPort);
    ~AgentSerialTransport() override;

    static std::vector<AgentSerialPort> availablePorts(std::uint16_t agentControlPort,
                                                        std::string &error);

    void open(const SerialConfig &config) override;
    void close() override;
    bool write(const ByteBuffer &data) override;
    bool isOpen() const override;

private:
    void workerMain(SerialConfig config);
    void removeForward(int localPort) const;

    std::string adbSerial_;
    std::uint16_t agentControlPort_ = 0;
    std::atomic<bool> open_{false};
    std::atomic<bool> stopping_{false};
    std::thread worker_;
    mutable std::mutex socketMutex_;
    std::unique_ptr<asio::io_context> ioContext_;
    std::unique_ptr<asio::ip::tcp::socket> socket_;
};

} // namespace serialdemo
