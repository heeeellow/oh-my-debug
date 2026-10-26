#include "adapters/device/scrcpy_backend.h"

#include "adapters/adb/adb_transport.h"
#include "application/device/screen_mirror_service.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwchar>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace testtools {
namespace {
#ifdef _WIN32
std::wstring wide(const std::string& text)
{
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::filesystem::path executableDirectory()
{
    wchar_t path[32768] = {};
    const auto size = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    return std::filesystem::path(std::wstring(path, size)).parent_path();
}

std::filesystem::path scrcpyPath(const std::string& requested)
{
    if (!requested.empty()) return std::filesystem::u8path(requested);
    wchar_t configured[32768] = {};
    const auto size = GetEnvironmentVariableW(L"SCRCPY_PATH", configured, static_cast<DWORD>(std::size(configured)));
    if (size > 0 && size < std::size(configured)) {
        auto path = std::filesystem::path(configured);
        if (std::filesystem::is_directory(path)) path /= L"scrcpy.exe";
        return path;
    }
    return executableDirectory() / L"scrcpy" / L"scrcpy.exe";
}

struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    HANDLE release() { const auto result = value; value = nullptr; return result; }
};

std::vector<wchar_t> childEnvironment(const std::wstring& adb, const std::filesystem::path& directory)
{
    const std::vector<std::wstring> replacements = {
        L"ADB=" + adb, L"SCRCPY_SERVER_PATH=" + (directory / L"scrcpy-server").wstring(),
        L"SCRCPY_ICON_PATH=" + (directory / L"scrcpy.png").wstring()
    };
    std::vector<std::wstring> entries;
    auto* inherited = GetEnvironmentStringsW();
    if (inherited) {
        for (const auto* entry = inherited; *entry; entry += std::wcslen(entry) + 1) {
            const bool replaced = std::any_of(replacements.begin(), replacements.end(), [&](const auto& value) {
                return _wcsnicmp(entry, value.c_str(), value.find(L'=') + 1) == 0;
            });
            if (!replaced) entries.emplace_back(entry);
        }
        FreeEnvironmentStringsW(inherited);
    }
    entries.insert(entries.end(), replacements.begin(), replacements.end());
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    std::vector<wchar_t> block;
    for (const auto& entry : entries) { block.insert(block.end(), entry.begin(), entry.end()); block.push_back(0); }
    block.push_back(0);
    return block;
}

class ScrcpyBackend final : public IScreenMirrorBackend {
public:
    ScrcpyBackend(std::string scrcpy, std::string adb) : requested_(std::move(scrcpy)), adb_(std::move(adb)) {}
    ~ScrcpyBackend() override { stop(); }

    bool start(const std::string& serial, std::string& error) override
    {
        stop();
        if (cancelled_) { error = "投屏启动已取消"; return false; }
        const auto sdk = adb_.runCommand(serial, {"shell", "getprop", "ro.build.version.sdk"}, std::chrono::seconds(3));
        int api = 0;
        try { api = std::stoi(sdk.standardOutput); } catch (...) {}
        if (sdk.exitCode != 0 || sdk.timedOut || api < 21) {
            error = sdk.timedOut ? "读取设备系统超时，请检查 ADB 连接" : "投屏需要 Android 5.0 或更新版本；请检查设备类型和授权";
            return false;
        }
        const auto program = std::filesystem::absolute(scrcpyPath(requested_));
        if (!std::filesystem::is_regular_file(program) || !std::filesystem::is_regular_file(program.parent_path() / L"scrcpy-server")) {
            error = "投屏工具不完整，请恢复程序旁的 scrcpy 目录或检查 SCRCPY_PATH";
            return false;
        }
        wchar_t adbPath[32768] = {};
        const auto adbSize = SearchPathW(nullptr, wide(adb_.program()).c_str(), L".exe", static_cast<DWORD>(std::size(adbPath)), adbPath, nullptr);
        if (adbSize == 0 || adbSize >= std::size(adbPath)) { error = "找不到 ADB 程序，请检查 platform-tools 或 ADB_PATH"; return false; }
        if (cancelled_) { error = "投屏启动已取消"; return false; }
        auto environment = childEnvironment(std::wstring(adbPath, adbSize), program.parent_path());
        const std::vector<std::string> arguments = {"--serial=" + serial, "--window-title=TestTools - " + serial,
            "--no-audio", "--max-size=1920", "--max-fps=60", "--no-clipboard-autosync", "--no-terminal-title"};
        auto command = wide(serialdemo::ProcessRunner::buildCommandLine(program.u8string(), arguments));
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        Handle read, write, input;
        if (!CreatePipe(&read.value, &write.value, &security, 0) || !SetHandleInformation(read.value, HANDLE_FLAG_INHERIT, 0)) {
            error = "无法创建投屏输出通道"; return false;
        }
        input.value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr);
        if (input.value == INVALID_HANDLE_VALUE) { error = "无法初始化投屏进程输入"; return false; }
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = input.value;
        startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = write.value;
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        std::vector<unsigned char> attributes(bytes);
        startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes)) { error = "无法初始化投屏进程属性"; return false; }
        HANDLE inheritedHandles[] = {input.value, write.value};
        const bool attributesReady = UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inheritedHandles, sizeof(inheritedHandles), nullptr, nullptr) != FALSE;
        PROCESS_INFORMATION process{};
        // 使用私有环境块为 scrcpy 绑定 ADB 路径，避免修改全局环境影响其他并发命令。
        const bool created = attributesReady && CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
            environment.data(), program.parent_path().c_str(), &startup.StartupInfo, &process);
        const auto nativeError = GetLastError();
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        if (!created) { error = "无法启动投屏工具（Windows 错误 " + std::to_string(nativeError) + "）"; return false; }
        CloseHandle(process.hThread);
        process_ = process.hProcess; pid_ = process.dwProcessId; serial_ = serial;
        { std::lock_guard<std::mutex> lock(logMutex_); log_.clear(); }
        readerHandle_ = read.release();
        reader_ = std::thread([this] {
            char buffer[2048]; DWORD count = 0;
            while (ReadFile(readerHandle_, buffer, sizeof(buffer), &count, nullptr) && count) {
                std::lock_guard<std::mutex> lock(logMutex_);
                log_.append(buffer, count);
                if (log_.size() > 8192) log_.erase(0, log_.size() - 8192);
            }
        });
        CloseHandle(write.release());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!cancelled_ && std::chrono::steady_clock::now() < deadline) {
            const auto state = poll();
            if (!state.running) {
                error = "投屏进程提前退出（退出码 " + std::to_string(state.exitCode) + "），请检查工具依赖或设备连接。\n" + state.detail;
                stop();
                return false;
            }
            if (output().find("Texture:") != std::string::npos) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        error = cancelled_ ? "投屏启动已取消" : "等待投屏画面超时，请检查设备视频编码能力与连接\n" + output();
        stop();
        return false;
    }

    void requestStop() noexcept override { cancelled_ = true; }

    void stop() override
    {
        if (process_) {
            // 仅关闭归属于当前 scrcpy 进程 PID 的窗口，绝不终止共享的 ADB Server。
            EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
                const auto target = static_cast<DWORD>(parameter); DWORD owner = 0;
                GetWindowThreadProcessId(window, &owner);
                if (owner == target) PostMessageW(window, WM_CLOSE, 0, 0);
                return TRUE;
            }, static_cast<LPARAM>(pid_));
            if (WaitForSingleObject(process_, 1500) == WAIT_TIMEOUT) {
                TerminateProcess(process_, 130); WaitForSingleObject(process_, 1000);
            }
            CloseHandle(process_); process_ = nullptr;
        }
        if (reader_.joinable()) {
            CancelSynchronousIo(reader_.native_handle());
            reader_.join();
        }
        if (readerHandle_) { CloseHandle(readerHandle_); readerHandle_ = nullptr; }
    }

    MirrorProcessStatus poll() override
    {
        if (!process_) return {};
        if (WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) return {true, 0, {}};
        DWORD code = 0; GetExitCodeProcess(process_, &code);
        return {false, static_cast<int>(code), output()};
    }

    bool control(MirrorControl control, std::string& error) override
    {
        const char* key = nullptr;
        switch (control) {
        case MirrorControl::Back: key = "4"; break;
        case MirrorControl::Home: key = "3"; break;
        case MirrorControl::Recents: key = "187"; break;
        case MirrorControl::Power: key = "26"; break;
        case MirrorControl::VolumeUp: key = "24"; break;
        case MirrorControl::VolumeDown: key = "25"; break;
        }
        if (!key) { error = "不支持的控制动作"; return false; }
        const auto result = adb_.runCommand(serial_, {"shell", "input", "keyevent", key}, std::chrono::seconds(3));
        if (result.exitCode == 0 && !result.timedOut) return true;
        error = result.timedOut ? "设备操作超时" : "设备操作失败：" + result.standardError;
        return false;
    }

    bool screenshot(const std::filesystem::path& path, std::string& error) override
    {
        const auto result = adb_.runCommand(serial_, {"exec-out", "screencap", "-p"}, std::chrono::seconds(8));
        if (result.exitCode != 0 || result.timedOut || result.standardOutput.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) != 0) {
            error = result.timedOut ? "截图超时" : "未收到有效 PNG 截图：" + result.standardError;
            return false;
        }
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(result.standardOutput.data(), static_cast<std::streamsize>(result.standardOutput.size()));
        file.close();
        if (!file) { error = "截图写入失败，请检查保存目录和磁盘空间"; return false; }
        return true;
    }

private:
    std::string output() const { std::lock_guard<std::mutex> lock(logMutex_); return log_; }
    std::string requested_, serial_;
    serialdemo::AdbTransport adb_;
    HANDLE process_ = nullptr, readerHandle_ = nullptr;
    DWORD pid_ = 0;
    std::thread reader_;
    std::atomic<bool> cancelled_{false};
    mutable std::mutex logMutex_;
    std::string log_;
};
#endif
} // namespace

std::unique_ptr<IScreenMirrorBackend> makeScrcpyBackend(std::string scrcpy, std::string adb)
{
#ifdef _WIN32
    return std::make_unique<ScrcpyBackend>(std::move(scrcpy), std::move(adb));
#else
    return {};
#endif
}

std::shared_ptr<ScreenMirrorService> createScreenMirrorService()
{
    return std::make_shared<ScreenMirrorService>(makeScrcpyBackend());
}

} // namespace testtools
