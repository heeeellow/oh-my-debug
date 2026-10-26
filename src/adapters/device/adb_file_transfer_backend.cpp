#include "adapters/adb/adb_transport.h"
#include "application/device/file_transfer_service.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <random>
#include <set>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif

namespace testtools {
namespace {

std::string quote(const std::string& value)
{
    std::string result = "'";
    for (char c : value)
        result += c == '\'' ? "'\\''" : std::string(1, c);
    return result + '\'';
}

std::string token()
{
    static std::atomic<unsigned long long> sequence{0};
    std::random_device random;
    return std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
           std::to_string(random()) + "-" + std::to_string(++sequence);
}

bool windowsName(const std::string& name)
{
    if (!FileTransferService::validName(name) || name.back() == '.' || name.back() == ' ' ||
        name.find_first_of("\\:<>\"|?*") != std::string::npos ||
        std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32; }))
        return false;
    auto base = name.substr(0, name.find('.'));
    std::transform(base.begin(), base.end(), base.begin(), [](unsigned char c) {
        return char(std::toupper(c));
    });
    if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" || base == "CLOCK$")
        return false;
    if (base.size() >= 4 && (base.substr(0, 3) == "COM" || base.substr(0, 3) == "LPT") &&
        (std::isdigit(static_cast<unsigned char>(base[3])) ||
         static_cast<unsigned char>(base[3]) >= 128))
        return false;
    return true;
}

bool linkOrReparse(const std::filesystem::path& path)
{
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return true;
#endif
    return std::filesystem::is_symlink(std::filesystem::symlink_status(path));
}

void validateLocalTree(const std::filesystem::path& path,
                       FileKind kind,
                       const std::atomic<bool>& cancel)
{
    if (linkOrReparse(path) ||
        (kind == FileKind::Directory ? !std::filesystem::is_directory(path)
                                     : !std::filesystem::is_regular_file(path)))
        throw std::runtime_error("文件已消失、类型发生变化或包含链接/重解析点");
    if (kind != FileKind::Directory)
        return;
    std::size_t count = 0;
    for (auto it = std::filesystem::recursive_directory_iterator(path);
         it != std::filesystem::recursive_directory_iterator();
         ++it) {
        if (cancel)
            throw std::runtime_error("已取消");
        if (++count > 20000 || it.depth() >= 32)
            throw std::runtime_error("文件夹超过 20000 项或 32 层限额");
        if (linkOrReparse(it->path()) || (!it->is_regular_file() && !it->is_directory()))
            throw std::runtime_error("文件夹包含软链接、重解析点或特殊文件");
    }
}

struct Cleanup {
    std::function<void()> run;
    ~Cleanup()
    {
        try {
            run();
        } catch (...) {
        }
    }
};

struct WindowsPathLess {
    bool operator()(const std::filesystem::path& a, const std::filesystem::path& b) const
    {
#ifdef _WIN32
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
#else
        return a < b;
#endif
    }
};

class AdbFileTransferBackend final : public IFileTransferBackend {
  public:
    FileListing list(const std::string& serial,
                     const std::string& directory,
                     const std::atomic<bool>& cancel) override
    {
        FileListing result;
        result.path = directory;
        // 使用 NUL 分隔字段，完整保留远端文件名中可能包含的空格与换行符。
        const std::string script = "cd " + quote(directory) + R"( || exit 1
[ -r . ] && [ -x . ] || { printf 'Permission denied: directory is not readable\n' >&2; exit 1; }
for f in * .[!.]* ..?*; do
  [ -e "$f" ] || [ -L "$f" ] || continue
  target=; tk=o; warning=
  if [ -L "$f" ]; then
    k=l; target=$(readlink -- "$f" 2>/dev/null)
    if [ -d "$f" ]; then tk=d; elif [ -f "$f" ]; then tk=f; fi
  elif [ -d "$f" ]; then k=d; elif [ -f "$f" ]; then k=f; else k=o; fi
  if info=$(stat -c '%s:%Y' -- "$f" 2>/dev/null); then :; else info=0:0; warning='无法读取属性'; fi
  s=${info%%:*}; m=${info#*:}
  printf '%s\000%s\000%s\000%s\000%s\000%s\000%s\000' "$f" "$k" "$s" "$m" "$tk" "$target" "$warning"
done)";
        const auto command =
            run(serial, {"shell", "-T", script}, cancel, {}, std::chrono::seconds(10));
        if (!success(command, result.error))
            return result;
        const auto& output = command.command.standardOutput;
        std::size_t pos = 0;
        const auto field = [&]() {
            const auto end = output.find('\0', pos);
            if (end == std::string::npos)
                throw std::runtime_error("设备目录响应不完整或缺少 stat/printf 支持");
            auto value = output.substr(pos, end - pos);
            pos = end + 1;
            return value;
        };
        try {
            while (pos < output.size()) {
                FileEntry item;
                item.name = field();
                const auto kind = field();
                const auto size = field();
                item.modified = field();
                const auto targetKind = field();
                item.linkTarget = field();
                item.metadataError = field();
                item.linkTargetKind = targetKind == "d" ? FileKind::Directory :
                                      targetKind == "f" ? FileKind::Regular : FileKind::Other;
                if (!FileTransferService::validName(item.name) || result.entries.size() >= 20000 ||
                    size.empty() || size.find_first_not_of("0123456789") != std::string::npos)
                    throw std::runtime_error("设备目录项目超限或格式不正确");
                item.kind = kind == "d"   ? FileKind::Directory
                            : kind == "f" ? FileKind::Regular
                            : kind == "l" ? FileKind::Symlink
                                          : FileKind::Other;
                item.size = std::stoull(size);
                result.entries.push_back(std::move(item));
            }
        } catch (const std::exception& e) {
            result.error = e.what();
            result.entries.clear();
        }
        return result;
    }

    FileTransferResult transfer(TransferDirection direction,
                                const std::string& serial,
                                const std::filesystem::path& local,
                                const std::string& remote,
                                FileKind kind,
                                bool overwrite,
                                const std::atomic<bool>& cancel,
                                const FileProgress& progress) override
    {
        if (cancel)
            return {TransferOutcome::Cancelled, "已取消"};
        return direction == TransferDirection::Download
                   ? download(serial, local, remote, kind, overwrite, cancel, progress)
                   : upload(serial, local, remote, kind, overwrite, cancel, progress);
    }

  private:
    serialdemo::AdbTransport adb_;

    serialdemo::ManagedCommandResult
    run(const std::string& serial,
        std::vector<std::string> arguments,
        const std::atomic<bool>& cancel,
        const FileProgress& progress = {},
        std::chrono::milliseconds timeout = std::chrono::minutes(10)) const
    {
        arguments.insert(arguments.begin(), {"-s", serial});
        return serialdemo::ProcessRunner::runManaged(
            adb_.program(), arguments, timeout, cancel, progress);
    }

    static bool success(const serialdemo::ManagedCommandResult& command, std::string& error)
    {
        if (command.cancelled)
            error = "已取消";
        else if (command.outputTruncated)
            error = "命令输出超过 4 MiB 限额，请缩小目录或传输范围";
        else if (command.command.timedOut)
            error = "ADB 操作超时，请检查设备连接";
        else if (command.command.exitCode != 0)
            error = command.command.standardError + command.command.standardOutput;
        else
            return true;
        if (error.empty())
            error = "ADB 操作失败（退出码 " + std::to_string(command.command.exitCode) + "）";
        if (error.size() > 4096)
            error.resize(4096);
        return false;
    }

    static FileTransferResult failure(const serialdemo::ManagedCommandResult& command)
    {
        std::string error;
        success(command, error);
        return {command.cancelled ? TransferOutcome::Cancelled : TransferOutcome::Failed, error};
    }

    void validateRemoteTree(const std::string& serial,
                            const std::string& path,
                            const std::atomic<bool>& cancel,
                            std::size_t& count,
                            int depth)
    {
        if (cancel)
            throw std::runtime_error("已取消");
        if (depth > 32)
            throw std::runtime_error("目录深度超过 32 层");
        auto listing = list(serial, path, cancel);
        if (!listing.error.empty())
            throw std::runtime_error(listing.error);
        std::set<std::filesystem::path, WindowsPathLess> names;
        for (const auto& entry : listing.entries) {
            if (++count > 20000)
                throw std::runtime_error("文件夹超过 20000 项，请缩小传输范围");
            if (!windowsName(entry.name))
                throw std::runtime_error("文件名不适用于 Windows：" + entry.name);
            if (!names.insert(std::filesystem::u8path(entry.name)).second)
                throw std::runtime_error("目录包含 Windows 无法区分的同名项目：" + entry.name);
            if (entry.kind == FileKind::Symlink || entry.kind == FileKind::Other)
                throw std::runtime_error("文件夹包含软链接或特殊文件：" + entry.name);
            if (entry.kind == FileKind::Directory)
                validateRemoteTree(serial, path + '/' + entry.name, cancel, count, depth + 1);
        }
    }

    FileTransferResult download(const std::string& serial,
                                const std::filesystem::path& target,
                                const std::string& remote,
                                FileKind kind,
                                bool overwrite,
                                const std::atomic<bool>& cancel,
                                const FileProgress& progress)
    {
        if (!windowsName(target.filename().u8string()))
            return {TransferOutcome::Failed, "文件名不适用于 Windows"};
        if (linkOrReparse(target))
            return {TransferOutcome::Failed, "目标是软链接或重解析点，未覆盖"};
        if (std::filesystem::exists(target) && (!overwrite || kind == FileKind::Directory ||
                                                !std::filesystem::is_regular_file(target)))
            return {TransferOutcome::Skipped, "同名项目已存在（已有文件夹首版不合并）"};
        try {
            if (kind == FileKind::Directory) {
                std::size_t count = 0;
                validateRemoteTree(serial, remote, cancel, count, 0);
            }
        } catch (const std::exception& e) {
            return {cancel ? TransferOutcome::Cancelled : TransferOutcome::Failed, e.what()};
        }
        if (cancel)
            return {TransferOutcome::Cancelled, "已取消"};
        const auto parent = std::filesystem::canonical(target.parent_path());
        const auto staging = parent / std::filesystem::u8path(".testtools-download-" + token());
        if (!std::filesystem::create_directory(staging))
            return {TransferOutcome::Failed, "无法创建本地传输临时目录"};
        const auto staged = staging / target.filename();
        const Cleanup cleanup{[&] {
            // 仅清理本次新创建的直接子级临时暂存目录。
            std::error_code ec;
            if (staging.parent_path() == parent &&
                staging.filename().u8string().find(".testtools-download-") == 0)
                std::filesystem::remove_all(staging, ec);
        }};
        const auto command = run(serial, {"pull", remote, staged.u8string()}, cancel, progress);
        std::string error;
        if (!success(command, error) || cancel)
            return cancel ? FileTransferResult{TransferOutcome::Cancelled, "已取消"}
                          : failure(command);
        try {
            validateLocalTree(staged, kind, cancel);
        } catch (const std::exception& e) {
            return {cancel ? TransferOutcome::Cancelled : TransferOutcome::Failed, e.what()};
        }
        if (linkOrReparse(target))
            return {TransferOutcome::Failed, "目标已变成链接，未覆盖"};
        if (cancel)
            return {TransferOutcome::Cancelled, "已取消"};
        bool renamed = false;
#ifdef _WIN32
        DWORD flags = MOVEFILE_WRITE_THROUGH;
        if (overwrite && kind == FileKind::Regular && std::filesystem::is_regular_file(target))
            flags |= MOVEFILE_REPLACE_EXISTING;
        renamed = MoveFileExW(staged.c_str(), target.c_str(), flags) != 0;
        if (!renamed)
            error = "本地文件替换失败（Windows 错误 " + std::to_string(GetLastError()) + "）";
#else
        std::error_code ec;
        if (!std::filesystem::exists(target) || (overwrite && kind == FileKind::Regular)) {
            std::filesystem::rename(staged, target, ec);
            renamed = !ec;
        }
        if (!renamed)
            error = "本地目标已存在或无法替换";
#endif
        return {renamed ? TransferOutcome::Completed : TransferOutcome::Failed,
                renamed ? "下载完成" : error};
    }

    FileTransferResult upload(const std::string& serial,
                              const std::filesystem::path& source,
                              const std::string& remote,
                              FileKind kind,
                              bool overwrite,
                              const std::atomic<bool>& cancel,
                              const FileProgress& progress)
    {
        try {
            validateLocalTree(source, kind, cancel);
        } catch (const std::exception& e) {
            return {cancel ? TransferOutcome::Cancelled : TransferOutcome::Failed, e.what()};
        }
        const auto parent = remote.substr(0, remote.find_last_of('/') + 1);
        const auto name = remote.substr(remote.find_last_of('/') + 1);
        // 预先探测已知的跳过冲突，避免先上传大文件才发现目标不可覆盖；最终提交阶段仍会二次校验。
        std::string probe = "t=" + quote(remote) + "; if [ -e \"$t\" ] || [ -L \"$t\" ]; then ";
        probe += overwrite && kind == FileKind::Regular
                     ? "if [ -L \"$t\" ] || [ ! -f \"$t\" ]; then exit 73; fi; "
                     : "exit 73; ";
        probe += "fi";
        auto command = run(serial, {"shell", "-T", probe}, cancel, {}, std::chrono::seconds(10));
        if (command.command.exitCode == 73)
            return {TransferOutcome::Skipped, "同名项目已存在（已有文件夹首版不合并）"};
        std::string error;
        if (!success(command, error))
            return failure(command);
        const auto staging = parent + ".testtools-upload-" + token();
        command = run(serial,
                      {"shell", "-T", "mkdir " + quote(staging)},
                      cancel,
                      {},
                      std::chrono::seconds(10));
        if (!success(command, error))
            return failure(command);
        const Cleanup cleanup{[&] {
            // 暂存目录名由本函数内部生成，绝不接受外部文件选择器的输入。
            const std::atomic<bool> noCancel{false};
            if (staging.rfind(parent + ".testtools-upload-", 0) == 0)
                run(serial,
                    {"shell", "-T", "rm -rf -- " + quote(staging)},
                    noCancel,
                    {},
                    std::chrono::seconds(3));
        }};
        const auto staged = staging + '/' + name;
        if (kind == FileKind::Directory) {
            // adb push 可能会忽略空目录；先显式建立目录骨架，再推送到暂存父目录以保留源目录名。
            const auto prefix = "cd " + quote(staging) + " && mkdir -p -- ";
            std::string script = prefix + quote(name);
            const auto flush = [&]() {
                auto result =
                    run(serial, {"shell", "-T", script}, cancel, {}, std::chrono::seconds(10));
                script = prefix;
                return result;
            };
            std::size_t count = 0;
            for (auto it = std::filesystem::recursive_directory_iterator(source);
                 it != std::filesystem::recursive_directory_iterator();
                 ++it) {
                if (cancel)
                    return {TransferOutcome::Cancelled, "已取消"};
                if (++count > 20000 || it.depth() >= 32)
                    return {TransferOutcome::Failed, "目录超过传输限额"};
                if (linkOrReparse(it->path()))
                    return {TransferOutcome::Failed, "目录已变成链接，未上传"};
                if (!it->is_directory())
                    continue;
                const auto relative =
                    name + '/' + it->path().lexically_relative(source).generic_u8string();
                const auto argument = ' ' + quote(relative);
                if (script.size() + argument.size() > 8192 && script.size() > prefix.size()) {
                    command = flush();
                    if (!success(command, error))
                        return failure(command);
                }
                script += argument;
            }
            if (script.size() > prefix.size()) {
                command = flush();
                if (!success(command, error))
                    return failure(command);
            }
        }
        command = run(serial,
                      {"push", source.u8string(), kind == FileKind::Directory ? staging : staged},
                      cancel,
                      progress);
        if (!success(command, error) || cancel)
            return cancel ? FileTransferResult{TransferOutcome::Cancelled, "已取消"}
                          : failure(command);
        // 仅在暂存上传成功后替换目标文件；在最终同文件系统原子移动前复制原文件的属主、权限与 SELinux 标签。
        std::string script = "t=" + quote(remote) + "; s=" + quote(staged) + "; ";
        script += (kind == FileKind::Directory ? "[ -d \"$s\" ]" : "[ -f \"$s\" ]");
        script += R"( && [ ! -L "$s" ] || exit 1
if [ -e "$t" ] || [ -L "$t" ]; then
if [ -L "$t" ] || [ ! -f "$t" ]; then exit 73; fi
)";
        if (!overwrite || kind == FileKind::Directory)
            script += "exit 73\n";
        else
            script += R"(owner=$(stat -c '%u:%g' -- "$t") || exit 1
mode=$(stat -c '%a' -- "$t") || exit 1
chown "$owner" "$s" && chmod "$mode" "$s" || exit 1
if context=$(ls -Zd -- "$t" 2>/dev/null); then
context=${context%% *}
case "$context" in *:*:*) chcon "$context" "$s" || exit 1;; esac
fi
)";
        script += "mv -fT -- \"$s\" \"$t\" || exit 1\nelse\nmv -nT -- \"$s\" \"$t\" || exit 1\n[ ! "
                  "-e \"$s\" ] || exit 73\nfi";
        command = run(serial, {"shell", "-T", script}, cancel, {}, std::chrono::seconds(10));
        if (command.command.exitCode == 73)
            return {TransferOutcome::Skipped, "同名项目已存在（已有文件夹首版不合并）"};
        if (!success(command, error)) {
            auto result = failure(command);
            result.message += "（替换阶段中断，请刷新并复核目标文件）";
            return result;
        }
        return {TransferOutcome::Completed, "上传完成"};
    }
};
} // namespace

std::shared_ptr<FileTransferService> createFileTransferService()
{
    return std::make_shared<FileTransferService>(std::make_shared<AdbFileTransferBackend>());
}

} // namespace testtools
