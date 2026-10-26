#include "adapters/serial/cserialport_transport.h"

#include "CSerialPort/SerialPortInfo.h"

#include <algorithm>

namespace serialdemo {

namespace {

itas109::Parity toLibraryParity(Parity value)
{
    switch (value) {
    case Parity::Odd:
        return itas109::ParityOdd;
    case Parity::Even:
        return itas109::ParityEven;
    case Parity::None:
    default:
        return itas109::ParityNone;
    }
}

itas109::StopBits toLibraryStopBits(StopBits value)
{
    return value == StopBits::Two ? itas109::StopTwo : itas109::StopOne;
}

itas109::FlowControl toLibraryFlowControl(FlowControl value)
{
    switch (value) {
    case FlowControl::Hardware:
        return itas109::FlowHardware;
    case FlowControl::Software:
        return itas109::FlowSoftware;
    case FlowControl::None:
    default:
        return itas109::FlowNone;
    }
}

} // namespace

CSerialPortTransport::CSerialPortTransport()
{
    serialPort_.connectReadEvent(this);
    // 收到字节后立即投递每次 RX 事件，确保交互式终端回显无需等待空闲超时。
    serialPort_.setReadIntervalTimeout(0);
}

CSerialPortTransport::~CSerialPortTransport()
{
    close();
    serialPort_.disconnectReadEvent();
}

std::vector<PortInfo> CSerialPortTransport::availablePorts()
{
    std::vector<PortInfo> result;
    for (const auto &port : itas109::CSerialPortInfo::availablePortInfos()) {
        result.push_back({port.portName, port.description, port.hardwareId});
    }
    return result;
}

void CSerialPortTransport::open(const SerialConfig &config)
{
    close();
    std::lock_guard<std::mutex> lock(controlMutex_);

    serialPort_.init(config.endpoint.c_str(),
                     config.baudRate,
                     toLibraryParity(config.parity),
                     static_cast<itas109::DataBits>(config.dataBits),
                     toLibraryStopBits(config.stopBits),
                     toLibraryFlowControl(config.flowControl),
                     65536);
    serialPort_.setReadIntervalTimeout(0);
    serialPort_.setMinByteReadNotify(1);
    serialPort_.setByteReadBufferFullNotify(49152);

    if (!serialPort_.open()) {
        open_.store(false);
        const auto reason = serialPort_.getLastError() == itas109::ErrorAccessDenied
            ? std::string("拒绝访问，端口可能被其他程序占用或权限不足。请先断开其他终端中的此串口后重试。")
            : std::string(serialPort_.getLastErrorMsg());
        publishStatus(TransportState::Error, "无法打开本地串口 " + config.endpoint + "：" + reason);
        return;
    }

    hardwareFlow_.store(config.flowControl == FlowControl::Hardware);
    open_.store(true);
    publishStatus(TransportState::Connected, "本地串口已连接: " + config.endpoint);
}

void CSerialPortTransport::close()
{
    std::lock_guard<std::mutex> lock(controlMutex_);
    const bool wasOpen = open_.exchange(false);
    if (serialPort_.isOpen()) {
        serialPort_.close();
    }
    if (wasOpen) {
        publishStatus(TransportState::Closed, "本地串口已关闭");
    }
}

bool CSerialPortTransport::write(const ByteBuffer &data)
{
    if (data.empty()) {
        return true;
    }

    if (!open_.load() || !serialPort_.isOpen()) {
        return false;
    }
    return serialPort_.writeData(data.data(), static_cast<int>(data.size())) == static_cast<int>(data.size());
}

bool CSerialPortTransport::isOpen() const
{
    return open_.load();
}

bool CSerialPortTransport::supportsControlLine(SerialControlLine line) const
{
    return open_.load() && (line == SerialControlLine::Dtr || !hardwareFlow_.load());
}

bool CSerialPortTransport::setControlLine(SerialControlLine line, bool enabled, std::string &error)
{
    // 保持底层控制线请求与串口句柄关闭操作互斥。
    std::lock_guard<std::mutex> lock(controlMutex_);
    if (!supportsControlLine(line)) {
        error = "串口未连接或 RTS 正由硬件流控管理";
        return false;
    }
    unsigned long nativeError = 0;
    if (!serialPort_.setControlLineChecked(line == SerialControlLine::Dtr, enabled, nativeError)) {
        error = nativeError == 170 ? "控制线由驱动握手管理，不能手动修改"
                                  : "控制线设置失败，Windows 错误 " + std::to_string(nativeError);
        return false;
    }
    error.clear();
    return true;
}

void CSerialPortTransport::onReadEvent(const char *, unsigned int readBufferLen)
{
    if (!open_.load() || readBufferLen == 0) {
        return;
    }

    ByteBuffer data(std::min<unsigned int>(readBufferLen, 65536));
    int received = 0;
    if (!serialPort_.isOpen()) {
        return;
    }
    received = serialPort_.readData(data.data(), static_cast<int>(data.size()));
    if (received > 0) {
        data.resize(static_cast<std::size_t>(received));
        publishData(data);
    }
}

} // namespace serialdemo
