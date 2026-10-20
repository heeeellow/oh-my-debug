#pragma once

#include "core/serial/serial_transport.h"

#include "CSerialPort/SerialPort.h"
#include "CSerialPort/SerialPortListener.h"

#include <atomic>
#include <mutex>

namespace serialdemo {

class CSerialPortTransport final : public ISerialTransport,
                                   private itas109::CSerialPortListener {
public:
    CSerialPortTransport();
    ~CSerialPortTransport() override;

    static std::vector<PortInfo> availablePorts();

    void open(const SerialConfig &config) override;
    void close() override;
    bool write(const ByteBuffer &data) override;
    bool isOpen() const override;
    bool supportsControlLine(SerialControlLine line) const override;
    bool setControlLine(SerialControlLine line, bool enabled, std::string &error) override;

private:
    void onReadEvent(const char *portName, unsigned int readBufferLen) override;

    itas109::CSerialPort serialPort_;
    std::atomic<bool> open_{false};
    std::atomic<bool> hardwareFlow_{false};
    std::mutex controlMutex_;
};

} // namespace serialdemo
