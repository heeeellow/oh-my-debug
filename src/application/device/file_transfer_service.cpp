#include "application/device/file_transfer_service.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <exception>
#include <set>

namespace testtools {

FileTransferService::FileTransferService(std::shared_ptr<IFileTransferBackend> backend)
    : backend_(std::move(backend))
{
}

bool FileTransferService::validName(const std::string& name)
{
    return !name.empty() && name != "." && name != ".." && name.size() <= 4096 &&
           name.find('/') == std::string::npos && name.find('\0') == std::string::npos;
}

bool FileTransferService::validChangeName(const std::string& name, bool remote)
{
    if (!validName(name) || (remote && name.size() > 255) || name.find('\\') != std::string::npos ||
        std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32 || c == 127; })) return false;
    if (remote) return true;
#ifdef _WIN32
    try { if (std::filesystem::u8path(name).native().size() > 255) return false; }
    catch (...) { return false; }
#else
    if (name.size() > 255) return false;
#endif
    if (name.back() == '.' || name.back() == ' ' || name.find_first_of(":<>\"|?*") != std::string::npos) return false;
    auto base = name.substr(0, name.find('.'));
    std::transform(base.begin(), base.end(), base.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" || base == "CLOCK$" || base == "CONIN$" || base == "CONOUT$") return false;
    if (base.size() > 3 && (base.substr(0, 3) == "COM" || base.substr(0, 3) == "LPT")) {
        const auto suffix = base.substr(3);
        if ((suffix.size() == 1 && suffix[0] >= '0' && suffix[0] <= '9') || suffix == u8"¹" || suffix == u8"²" || suffix == u8"³") return false;
    }
    return true;
}

std::string FileTransferService::normalizeRemoteDirectory(const std::string& directory)
{
    if (directory.empty() || directory.front() != '/' || directory.size() > 4096 ||
        std::any_of(
            directory.begin(), directory.end(), [](unsigned char c) { return c < 32 || c == 127; }))
        return {};
    std::vector<std::string> parts;
    std::size_t start = 1;
    while (start <= directory.size()) {
        auto end = directory.find('/', start);
        if (end == std::string::npos)
            end = directory.size();
        const auto part = directory.substr(start, end - start);
        if (part == "..") {
            if (!parts.empty())
                parts.pop_back();
        } else if (!part.empty() && part != ".")
            parts.push_back(part);
        start = end + 1;
    }
    std::string result;
    for (const auto& part : parts)
        result += '/' + part;
    return result.empty() ? "/" : result;
}

FileListing FileTransferService::listRemote(const serialdemo::AdbDevice& device,
                                            const std::string& directory,
                                            const std::atomic<bool>& cancel) const
{
    FileListing result;
    result.path = normalizeRemoteDirectory(directory);
    if (device.serial.empty() || device.state != "device")
        result.error = "请先选择在线且已授权的 ADB 设备";
    else if (result.path.empty())
        result.error = "请输入有效的设备绝对目录（以 / 开头）";
    else {
        try {
            result = backend_->list(device.serial, result.path, cancel);
        } catch (const std::exception& e) {
            result.error = e.what();
        }
    }
    if (!result.error.empty())
        result.entries.clear();
    return result;
}

FileListing FileTransferService::listLocal(const std::filesystem::path& directory,
                                           const std::atomic<bool>& cancel)
{
    FileListing result;
    try {
        result.path = std::filesystem::absolute(directory).lexically_normal().u8string();
        for (const auto& entry :
             std::filesystem::directory_iterator(std::filesystem::u8path(result.path))) {
            if (cancel) {
                result.error = "目录读取已取消";
                break;
            }
            if (result.entries.size() >= 20000) {
                result.error = "目录超过 20000 项，请选择更具体的目录";
                break;
            }
            FileEntry item;
            item.name = entry.path().filename().u8string();
            std::error_code ec;
            const auto status = entry.symlink_status(ec);
            if (ec)
                continue;
            item.kind = std::filesystem::is_symlink(status)        ? FileKind::Symlink
                        : std::filesystem::is_directory(status)    ? FileKind::Directory
                        : std::filesystem::is_regular_file(status) ? FileKind::Regular
                                                                   : FileKind::Other;
            if (item.kind == FileKind::Symlink) {
                const auto target = entry.status(ec);
                item.linkTargetKind = std::filesystem::is_directory(target) ? FileKind::Directory :
                                      std::filesystem::is_regular_file(target) ? FileKind::Regular : FileKind::Other;
                const auto linked = std::filesystem::read_symlink(entry.path(), ec);
                if (!ec) item.linkTarget = linked.u8string();
            }
            const auto modified = entry.last_write_time(ec);
            if (!ec) {
                const auto stamp = std::chrono::time_point_cast<std::chrono::seconds>(
                    modified - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
                item.modified = std::to_string(stamp.time_since_epoch().count());
            }
            if (item.kind == FileKind::Regular) {
                item.size = entry.file_size(ec);
                if (ec)
                    item.size = 0;
            }
            result.entries.push_back(std::move(item));
        }
    } catch (const std::exception& e) {
        result.error = std::string("本地目录读取失败：") + e.what();
    }
    if (!result.error.empty())
        result.entries.clear();
    return result;
}

bool FileTransferService::prepare(FileTransferRequest request, std::string& error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.active) {
        error = "已有文件传输正在进行";
        return false;
    }
    if (request.device.serial.empty() || request.device.state != "device") {
        error = "请先选择在线且已授权的 ADB 设备";
        return false;
    }
    request.remoteDirectory = normalizeRemoteDirectory(request.remoteDirectory);
    if (request.remoteDirectory.empty() || request.localDirectory.empty() ||
        !request.localDirectory.is_absolute()) {
        error = "请先打开有效的本地目录和设备绝对目录";
        return false;
    }
    if (request.entries.empty() || request.entries.size() > 256) {
        error = "请选择 1–256 个文件或文件夹";
        return false;
    }
    std::set<std::string> names;
    for (const auto& entry : request.entries) {
        // 远端文件名同样必须保持为合法的单个 Windows 路径组件。
        if (!validName(entry.name) || entry.name.find_first_of("\\:") != std::string::npos ||
            !names.insert(entry.name).second) {
            error = "所选文件名无效或重复";
            return false;
        }
    }
    request_ = std::move(request);
    cancel_ = false;
    const auto nextId = state_.id + 1;
    state_ = {};
    state_.id = nextId;
    state_.active = true;
    state_.serial = request_.device.serial;
    state_.direction = request_.direction;
    state_.total = request_.entries.size();
    state_.parent = request_.remoteDirectory;
    state_.summary = "准备传输";
    return true;
}

std::string FileTransferService::normalizePermissions(const std::string& value)
{
    if ((value.size() != 3 && value.size() != 4) ||
        value.find_first_not_of("01234567") != std::string::npos) return {};
    return value.size() == 3 ? "0" + value : value;
}

bool FileTransferService::prepareChange(FileManagementRequest request, std::string& error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    error.clear();
    if (state_.active) { error = "已有文件任务正在进行"; return false; }
    if (request.operation == FileOperation::Transfer ||
        (request.operation != FileOperation::CreateFolder && request.operation != FileOperation::Rename &&
         request.operation != FileOperation::Remove && request.operation != FileOperation::SetPermissions)) {
        error = "文件操作无效"; return false;
    }
    if (request.remote) {
        request.parent = normalizeRemoteDirectory(request.parent);
        if (request.device.serial.empty() || request.device.state != "device") {
            error = "请先选择在线且已授权的 ADB 设备"; return false;
        }
    } else {
        try {
            if (request.parent.size() > 4096 || std::any_of(request.parent.begin(), request.parent.end(), [](unsigned char c) { return c < 32 || c == 127; }) ||
                !std::filesystem::u8path(request.parent).is_absolute()) request.parent.clear();
        } catch (...) { request.parent.clear(); }
    }
    if (request.parent.empty() || request.entries.empty() || request.entries.size() > 256 ||
        ((request.operation == FileOperation::CreateFolder || request.operation == FileOperation::Rename) && request.entries.size() != 1)) {
        error = "请先打开目录并选择有效项目"; return false;
    }
    if (request.operation == FileOperation::SetPermissions) {
        request.permissions = normalizePermissions(request.permissions);
        if (!request.remote || request.permissions.empty() ||
            std::any_of(request.entries.begin(), request.entries.end(), [](const auto& e) {
                return e.kind != FileKind::Regular && e.kind != FileKind::Directory;
            })) {
            error = "仅设备普通文件和文件夹可修改权限，请输入 3 或 4 位八进制数（0–7）"; return false;
        }
    }
    std::set<std::string> names;
    for (const auto& entry : request.entries) {
        if (!validChangeName(entry.name, request.remote) || !names.insert(entry.name).second) {
            error = "名称无效、重复或包含路径/控制字符"; return false;
        }
    }
    if (request.operation == FileOperation::Rename &&
        (!validChangeName(request.newName, request.remote) || request.newName == request.entries.front().name)) {
        error = "请输入有效且不同的新名称"; return false;
    }
    change_ = std::move(request);
    cancel_ = false;
    const auto id = state_.id + 1;
    state_ = {}; state_.id = id; state_.active = true;
    state_.operation = change_.operation; state_.remote = change_.remote;
    state_.serial = change_.remote ? change_.device.serial : std::string{};
    state_.parent = change_.parent; state_.total = change_.entries.size(); state_.summary = "准备文件操作";
    return true;
}

void FileTransferService::executePrepared()
{
    FileTransferRequest request;
    FileManagementRequest change;
    bool management = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!state_.active)
            return;
        request = request_;
        management = state_.operation != FileOperation::Transfer;
        if (management) change = change_;
    }
    if (management) {
        for (const auto& entry : change.entries) {
            { std::lock_guard<std::mutex> lock(mutex_); state_.current = entry.name; }
            const auto full = change.remote ? (change.parent == "/" ? "/" : change.parent + '/') + entry.name
                : (std::filesystem::u8path(change.parent) / std::filesystem::u8path(entry.name)).u8string();
            FileTransferResult result;
            try {
                result = cancel_ ? FileTransferResult{TransferOutcome::Cancelled, "尚未执行，已取消"}
                    : backend_->change({change.operation, change.remote, change.device.serial, change.parent, entry, change.newName, change.permissions}, cancel_);
            } catch (const std::exception& e) { result = {TransferOutcome::Failed, e.what()}; }
              catch (...) { result = {TransferOutcome::Failed, "文件操作发生未知错误"}; }
            std::lock_guard<std::mutex> lock(mutex_);
            state_.records.push_back({entry.name, change.remote ? std::string{} : full, change.remote ? full : std::string{}, std::move(result)});
        }
    }
    else for (const auto& entry : request.entries) {
        std::filesystem::path local;
        const auto remote =
            (request.remoteDirectory == "/" ? "/" : request.remoteDirectory + '/') + entry.name;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_.current = entry.name;
            state_.progress.clear();
        }
        FileTransferResult result;
        try {
            local = request.localDirectory / std::filesystem::u8path(entry.name);
            if (cancel_)
                result = {TransferOutcome::Cancelled, "已取消"};
            else if (entry.kind != FileKind::Regular && entry.kind != FileKind::Directory)
                result = {TransferOutcome::Failed,
                          "首版仅传输普通文件和目录，不跟随软链接或特殊文件"};
            else
                result =
                    backend_->transfer(request.direction,
                                       request.device.serial,
                                       local,
                                       remote,
                                       entry.kind,
                                       request.overwrite,
                                       cancel_,
                                       [this](const std::string& progress) {
                                           std::lock_guard<std::mutex> lock(mutex_);
                                           state_.progress = progress.substr(
                                               progress.size() > 512 ? progress.size() - 512 : 0);
                                       });
        } catch (const std::exception& e) {
            result = {TransferOutcome::Failed, e.what()};
        } catch (...) {
            result = {TransferOutcome::Failed, "文件传输发生未知错误"};
        }
        std::lock_guard<std::mutex> lock(mutex_);
        state_.records.push_back({entry.name, local.u8string(), remote, std::move(result)});
    }
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t ok = 0, skipped = 0, failed = 0, cancelled = 0;
    for (const auto& record : state_.records) {
        switch (record.result.outcome) {
        case TransferOutcome::Completed:
            ++ok;
            break;
        case TransferOutcome::Skipped:
            ++skipped;
            break;
        case TransferOutcome::Failed:
            ++failed;
            break;
        case TransferOutcome::Cancelled:
            ++cancelled;
            break;
        }
    }
    state_.summary = "完成 " + std::to_string(ok) + "，跳过 " + std::to_string(skipped) +
                     "，失败 " + std::to_string(failed) + "，取消 " + std::to_string(cancelled);
    state_.active = false;
    state_.cancelling = false;
    state_.current.clear();
    state_.progress.clear();
}

void FileTransferService::cancel(std::uint64_t expectedId) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.active && (!expectedId || state_.id == expectedId)) {
        cancel_ = true;
        state_.cancelling = true;
    }
}

FileTransferSnapshot FileTransferService::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

} // namespace testtools
