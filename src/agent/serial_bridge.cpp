#include "serial_bridge.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

#ifdef _WIN32

namespace testtools::agent {

std::vector<SerialEndpointInfo> listSerialEndpoints()
{
    return {};
}

SerialBridge::~SerialBridge()
{
    stop();
}

bool SerialBridge::start(const serialdemo::SerialConfig &, std::uint16_t &,
                         std::string &, int &, bool &, std::string &error)
{
    error = "Managed serial streams are only available on Linux and Android agents";
    return false;
}

void SerialBridge::stop() {}
bool SerialBridge::isRunning() const { return false; }
void SerialBridge::workerMain() {}

} // namespace testtools::agent

#else

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

namespace testtools::agent {

namespace {

bool allowedEndpoint(const std::string &endpoint)
{
    static const std::regex pattern(R"(^/dev/tty[A-Za-z0-9._-]+$)");
    return std::regex_match(endpoint, pattern);
}

std::string readLine(const std::filesystem::path &path)
{
    std::ifstream input(path);
    std::string value;
    std::getline(input, value);
    return value;
}

std::string endpointInterface(const std::string &endpoint)
{
    const auto name = std::filesystem::path(endpoint).filename();
    std::error_code error;
    const auto device = std::filesystem::canonical(std::filesystem::path("/sys/class/tty")
                                                    / name / "device", error);
    if (error) {
        return {};
    }
    for (const auto &candidate : {device / "interface", device.parent_path() / "interface"}) {
        const std::string value = readLine(candidate);
        if (!value.empty()) {
            return value;
        }
    }
    return {};
}

std::string endpointOwner(const std::string &endpoint)
{
    std::error_code canonicalError;
    const auto canonicalEndpoint = std::filesystem::canonical(endpoint, canonicalError);
    if (canonicalError) {
        return {};
    }
    DIR *proc = opendir("/proc");
    if (!proc) {
        return {};
    }
    std::string result;
    while (dirent *entry = readdir(proc)) {
        if (!std::isdigit(static_cast<unsigned char>(entry->d_name[0]))) {
            continue;
        }
        const std::string pid = entry->d_name;
        const std::filesystem::path fdDirectory = std::filesystem::path("/proc") / pid / "fd";
        std::error_code iteratorError;
        for (std::filesystem::directory_iterator iterator(fdDirectory, iteratorError), end;
             !iteratorError && iterator != end; iterator.increment(iteratorError)) {
            std::error_code linkError;
            const auto target = std::filesystem::canonical(iterator->path(), linkError);
            if (!linkError && target == canonicalEndpoint) {
                std::string command = readLine(std::filesystem::path("/proc") / pid / "comm");
                result = (command.empty() ? "process" : command) + " (PID " + pid + ")";
                break;
            }
        }
        if (!result.empty()) {
            break;
        }
    }
    closedir(proc);
    return result;
}

speed_t baudConstant(int baudRate)
{
    switch (baudRate) {
    case 1200: return B1200;
    case 2400: return B2400;
    case 4800: return B4800;
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
#ifdef B230400
    case 230400: return B230400;
#endif
#ifdef B460800
    case 460800: return B460800;
#endif
#ifdef B921600
    case 921600: return B921600;
#endif
    default: return 0;
    }
}

int baudValue(speed_t speed)
{
    switch (speed) {
    case B1200: return 1200;
    case B2400: return 2400;
    case B4800: return 4800;
    case B9600: return 9600;
    case B19200: return 19200;
    case B38400: return 38400;
    case B57600: return 57600;
    case B115200: return 115200;
#ifdef B230400
    case B230400: return 230400;
#endif
#ifdef B460800
    case B460800: return 460800;
#endif
#ifdef B921600
    case B921600: return 921600;
#endif
    default: return 0;
    }
}

void readEffectiveBaud(int descriptor, int requested, int &effective, bool &applied)
{
    termios current{};
    effective = 0;
    applied = false;
    if (tcgetattr(descriptor, &current) == 0) {
        effective = baudValue(cfgetospeed(&current));
        applied = effective == requested;
    }
}

bool configureSerial(int descriptor, const serialdemo::SerialConfig &config,
                     int &effectiveBaudRate, bool &baudRateApplied, std::string &error)
{
    termios original{};
    if (tcgetattr(descriptor, &original) != 0) {
        error = "tcgetattr failed: " + std::string(std::strerror(errno));
        return false;
    }
    termios settings = original;
    cfmakeraw(&settings);
    settings.c_cflag |= CLOCAL | CREAD;
    settings.c_cflag &= ~CSIZE;
    switch (config.dataBits) {
    case 5: settings.c_cflag |= CS5; break;
    case 6: settings.c_cflag |= CS6; break;
    case 7: settings.c_cflag |= CS7; break;
    case 8: settings.c_cflag |= CS8; break;
    default:
        error = "data_bits must be between 5 and 8";
        return false;
    }
    if (config.parity == serialdemo::Parity::None) {
        settings.c_cflag &= ~PARENB;
    } else {
        settings.c_cflag |= PARENB;
        if (config.parity == serialdemo::Parity::Odd) {
            settings.c_cflag |= PARODD;
        } else {
            settings.c_cflag &= ~PARODD;
        }
    }
    if (config.stopBits == serialdemo::StopBits::Two) {
        settings.c_cflag |= CSTOPB;
    } else {
        settings.c_cflag &= ~CSTOPB;
    }
#ifdef CRTSCTS
    if (config.flowControl == serialdemo::FlowControl::Hardware) {
        settings.c_cflag |= CRTSCTS;
    } else {
        settings.c_cflag &= ~CRTSCTS;
    }
#endif
    if (config.flowControl == serialdemo::FlowControl::Software) {
        settings.c_iflag |= IXON | IXOFF;
    } else {
        settings.c_iflag &= ~(IXON | IXOFF);
    }
    settings.c_cc[VMIN] = 1;
    settings.c_cc[VTIME] = 0;

    const speed_t speed = baudConstant(config.baudRate);
    if (speed == 0) {
        error = "unsupported baud rate";
        return false;
    }
    cfsetispeed(&settings, speed);
    cfsetospeed(&settings, speed);
    if (tcsetattr(descriptor, TCSANOW, &settings) == 0) {
        readEffectiveBaud(descriptor, config.baudRate, effectiveBaudRate, baudRateApplied);
        return true;
    }

    // 部分 USB 调制解调器驱动在所有帧格式标志均合法时仍拒绝修改 termios 波特率；此时保留原速率重试。
    // 若包含波特率参数设置失败，先保留原有速率仅应用其余帧格式参数。
    cfsetispeed(&settings, cfgetispeed(&original));
    cfsetospeed(&settings, cfgetospeed(&original));
    if (tcsetattr(descriptor, TCSANOW, &settings) == 0) {
        readEffectiveBaud(descriptor, config.baudRate, effectiveBaudRate, baudRateApplied);
        return true;
    }
    error = "tcsetattr failed: " + std::string(std::strerror(errno));
    return false;
}

bool writeAll(int descriptor, const std::uint8_t *data, std::size_t size)
{
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t written = ::write(descriptor, data + offset, size - offset);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd ready{descriptor, POLLOUT, 0};
            if (poll(&ready, 1, 1000) <= 0) {
                return false;
            }
        } else {
            return false;
        }
    }
    return true;
}

} // namespace

std::vector<SerialEndpointInfo> listSerialEndpoints()
{
    std::vector<SerialEndpointInfo> result;
    std::error_code error;
    for (std::filesystem::directory_iterator iterator("/dev", error), end;
         !error && iterator != end; iterator.increment(error)) {
        const std::string name = iterator->path().filename().string();
        if (name.rfind("ttyS", 0) != 0 && name.rfind("ttyUSB", 0) != 0
            && name.rfind("ttyACM", 0) != 0) {
            continue;
        }
        struct stat information{};
        if (stat(iterator->path().c_str(), &information) != 0 || !S_ISCHR(information.st_mode)) {
            continue;
        }
        SerialEndpointInfo endpoint;
        endpoint.endpoint = iterator->path().string();
        endpoint.interfaceName = endpointInterface(endpoint.endpoint);
        endpoint.occupiedBy = endpointOwner(endpoint.endpoint);
        endpoint.readable = access(endpoint.endpoint.c_str(), R_OK) == 0;
        endpoint.writable = access(endpoint.endpoint.c_str(), W_OK) == 0;
        result.push_back(std::move(endpoint));
    }
    std::sort(result.begin(), result.end(), [](const auto &left, const auto &right) {
        return left.endpoint < right.endpoint;
    });
    return result;
}

SerialBridge::~SerialBridge()
{
    stop();
}

bool SerialBridge::start(const serialdemo::SerialConfig &config, std::uint16_t &port,
                         std::string &sessionId, int &effectiveBaudRate,
                         bool &baudRateApplied, std::string &error)
{
    stop();
    if (!allowedEndpoint(config.endpoint)) {
        error = "endpoint must be a /dev/tty* character device";
        return false;
    }
    struct stat information{};
    if (stat(config.endpoint.c_str(), &information) != 0 || !S_ISCHR(information.st_mode)) {
        error = "serial endpoint does not exist or is not a character device";
        return false;
    }
    const std::string owner = endpointOwner(config.endpoint);
    if (!owner.empty()) {
        error = "serial endpoint is occupied by " + owner;
        return false;
    }

    const int serialDescriptor = ::open(config.endpoint.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (serialDescriptor < 0) {
        error = "open failed: " + std::string(std::strerror(errno));
        return false;
    }
    if (flock(serialDescriptor, LOCK_EX | LOCK_NB) != 0) {
        error = "serial endpoint lock failed: " + std::string(std::strerror(errno));
        ::close(serialDescriptor);
        return false;
    }
    if (!configureSerial(serialDescriptor, config, effectiveBaudRate, baudRateApplied, error)) {
        ::close(serialDescriptor);
        return false;
    }

    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        error = "stream socket failed: " + std::string(std::strerror(errno));
        ::close(serialDescriptor);
        return false;
    }
    int enabled = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0
        || listen(listener, 1) != 0) {
        error = "stream listener failed: " + std::string(std::strerror(errno));
        ::close(listener);
        ::close(serialDescriptor);
        return false;
    }
    socklen_t addressSize = sizeof(address);
    if (getsockname(listener, reinterpret_cast<sockaddr *>(&address), &addressSize) != 0) {
        error = "stream port query failed";
        ::close(listener);
        ::close(serialDescriptor);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = config;
        serialFd_ = serialDescriptor;
        listenFd_ = listener;
        sessionId_ = std::to_string(static_cast<long long>(getpid())) + "-"
                     + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        sessionId = sessionId_;
    }
    port = ntohs(address.sin_port);
    stopping_.store(false);
    running_.store(true);
    worker_ = std::thread(&SerialBridge::workerMain, this);
    error.clear();
    return true;
}

void SerialBridge::workerMain()
{
    int serialDescriptor = -1;
    int listener = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        serialDescriptor = serialFd_;
        listener = listenFd_;
    }
    const int client = accept(listener, nullptr, nullptr);
    if (client >= 0) {
        std::array<std::uint8_t, 16384> buffer{};
        while (!stopping_.load()) {
            pollfd descriptors[2] = {{serialDescriptor, POLLIN, 0}, {client, POLLIN, 0}};
            const int ready = poll(descriptors, 2, 250);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if ((descriptors[0].revents & POLLIN) != 0) {
                const ssize_t received = ::read(serialDescriptor, buffer.data(), buffer.size());
                if (received > 0) {
#ifdef MSG_NOSIGNAL
                    if (send(client, buffer.data(), static_cast<std::size_t>(received), MSG_NOSIGNAL) <= 0) {
#else
                    if (send(client, buffer.data(), static_cast<std::size_t>(received), 0) <= 0) {
#endif
                        break;
                    }
                } else if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    break;
                }
            }
            if ((descriptors[1].revents & POLLIN) != 0) {
                const ssize_t received = recv(client, buffer.data(), buffer.size(), 0);
                if (received <= 0
                    || !writeAll(serialDescriptor, buffer.data(), static_cast<std::size_t>(received))) {
                    break;
                }
            }
            if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0
                || (descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                break;
            }
        }
        shutdown(client, SHUT_RDWR);
        ::close(client);
    }
    running_.store(false);
    std::lock_guard<std::mutex> lock(mutex_);
    if (listenFd_ == listener) {
        ::close(listenFd_);
        listenFd_ = -1;
    }
    if (serialFd_ == serialDescriptor) {
        ::close(serialFd_);
        serialFd_ = -1;
    }
}

void SerialBridge::stop()
{
    stopping_.store(true);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (listenFd_ >= 0) {
            shutdown(listenFd_, SHUT_RDWR);
        }
        // start() 成功后串口文件描述符归工作线程所有；在此处直接 close 会与 poll/read 竞态并导致文件描述符编号被重用。
        // 250 ms 的轮询周期限制了退出延迟，随后由 workerMain 唯一关闭一次串口描述符。
        // 确保描述符生命周期不跨越工作线程边界。
        // 待 worker_.join() 返回后再统一收尾。
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (listenFd_ >= 0) {
            ::close(listenFd_);
            listenFd_ = -1;
        }
    }
    running_.store(false);
}

bool SerialBridge::isRunning() const
{
    return running_.load();
}

} // namespace testtools::agent

#endif
