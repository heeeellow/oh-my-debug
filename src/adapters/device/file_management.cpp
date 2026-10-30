#include "adapters/device/file_management.h"
#include "application/device/file_transfer_service.h"
#include <chrono>
#include <algorithm>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace testtools {
namespace {
namespace fs = std::filesystem;
std::string quote(const std::string& value)
{
    std::string out = "'";
    for (char c : value) out += c == '\'' ? "'\\''" : std::string(1, c);
    return out + "'";
}
bool samePath(const fs::path& a, const fs::path& b)
{
#ifdef _WIN32
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
#else
    return a == b;
#endif
}
bool reparse(const fs::path& p)
{
#ifdef _WIN32
    const auto value = GetFileAttributesW(p.c_str());
    if (value != INVALID_FILE_ATTRIBUTES) return (value & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#endif
    return fs::is_symlink(fs::symlink_status(p));
}
bool volumeRoot(const fs::path& p)
{
    if (samePath(p, p.root_path())) return true;
#ifdef _WIN32
    wchar_t volume[MAX_PATH]{};
    auto directory = p.wstring();
    if (!directory.empty() && directory.back() != L'\\') directory += L'\\';
    return GetVolumeNameForVolumeMountPointW(directory.c_str(), volume, MAX_PATH) != 0;
#else
    return false;
#endif
}
void validate(const FileChange& r)
{
    if (!FileTransferService::validChangeName(r.entry.name, r.remote) ||
        (r.operation == FileOperation::Rename && !FileTransferService::validChangeName(r.newName, r.remote)))
        throw std::runtime_error("名称包含路径、控制字符或不支持的字符");
    if (r.operation == FileOperation::Transfer) throw std::runtime_error("文件操作无效");
}
}

FileTransferResult changeLocalFile(const FileChange& r, const std::atomic<bool>& cancel)
{
    std::size_t removed = 0;
    try {
        validate(r);
        if (r.remote || r.operation == FileOperation::SetPermissions) throw std::runtime_error("本地不支持此操作");
        if (r.parent.size() > 4096 || std::any_of(r.parent.begin(), r.parent.end(), [](unsigned char c) { return c < 32 || c == 127; }))
            throw std::runtime_error("本地父目录包含控制字符或超出限额");
        const auto requested = fs::u8path(r.parent);
        if (!requested.is_absolute()) throw std::runtime_error("本地父目录必须为绝对路径");
        const auto parent = fs::canonical(requested);
        const auto target = (parent / fs::u8path(r.entry.name)).lexically_normal();
        if (target.parent_path() != parent || volumeRoot(target)) throw std::runtime_error("根目录或挂载点受保护");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        const auto check = [&] {
            if (cancel) throw std::runtime_error("已停止，已完成的变更不会恢复");
            if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("本地操作超过 30 秒，请刷新复核");
        };
        check();
        if (r.operation == FileOperation::CreateFolder) {
            if (!fs::create_directory(target)) throw std::runtime_error("同名项目已存在");
            return {TransferOutcome::Completed, "文件夹已创建"};
        }
        const bool link = reparse(target);
        const auto kind = link ? FileKind::Symlink : fs::is_directory(target) ? FileKind::Directory
            : fs::is_regular_file(target) ? FileKind::Regular : FileKind::Other;
        if (kind == FileKind::Other || kind != r.entry.kind) throw std::runtime_error("目标已消失或类型变化，请刷新");
        if (r.operation == FileOperation::Rename) {
            const auto destination = (parent / fs::u8path(r.newName)).lexically_normal();
            if (destination.parent_path() != parent || volumeRoot(destination)) throw std::runtime_error("重命名目标无效");
            check();
#ifdef _WIN32
            // 不传替换标志，同名目标即失败；不跨目录，也不跟随所选链接。
            if (!MoveFileExW(target.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error("重命名失败，目标可能已存在或没有权限（Windows " + std::to_string(GetLastError()) + "）");
#else
            throw std::runtime_error("本地重命名当前仅支持 Windows");
#endif
            return {TransferOutcome::Completed, "已重命名为：" + r.newName};
        }
        if (r.operation != FileOperation::Remove) throw std::runtime_error("文件操作无效");
        // 先检查整棵树；确认范围后逐项删除，绝不递归进入重解析点。
        std::vector<fs::path> nodes{target};
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            check();
            const auto p = nodes[i];
            if (volumeRoot(p)) throw std::runtime_error("所选目录包含挂载点，未删除");
            if (!reparse(p) && fs::is_directory(p)) {
                for (const auto& child : fs::directory_iterator(p)) {
                    if (nodes.size() >= 20000) throw std::runtime_error("目录超过 20000 项，请缩小范围");
                    const auto relative = child.path().lexically_relative(target);
                    if (std::distance(relative.begin(), relative.end()) > 32)
                        throw std::runtime_error("目录超过 32 层，请缩小范围");
                    nodes.push_back(child.path());
                }
            }
        }
        for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
            check();
            // 删除前重查实际父目录，避免新出现的目录链接使目标越出已检查范围。
            if (!samePath(fs::canonical(it->parent_path()), it->parent_path()) || volumeRoot(*it))
                throw std::runtime_error("目标路径或挂载点已变化，已停止");
#ifdef _WIN32
            const auto attributes = GetFileAttributesW(it->c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) throw std::runtime_error("目标已变化，请刷新");
            const bool ok = (attributes & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(it->c_str()) != 0 : DeleteFileW(it->c_str()) != 0;
            if (!ok) throw std::runtime_error("删除失败（Windows " + std::to_string(GetLastError()) + "）");
#else
            if (!fs::remove(*it)) throw std::runtime_error("删除失败");
#endif
            ++removed;
        }
        return {TransferOutcome::Completed, "已删除（含子项 " + std::to_string(removed - 1) + "）"};
    } catch (const std::exception& e) {
        return {cancel ? TransferOutcome::Cancelled : TransferOutcome::Failed,
            std::string(e.what()) + (removed ? "；已删除 " + std::to_string(removed) + " 项，请刷新复核" : "")};
    }
}

std::string deviceFileChangeScript(const FileChange& r)
{
    validate(r);
    const auto parent = FileTransferService::normalizeRemoteDirectory(r.parent);
    if (!r.remote || r.serial.empty() || parent.empty()) throw std::runtime_error("设备目录或目标无效");
    std::string script = "# TestTools fixed file operation v1\nparent=" + quote(parent) + "\nname=" + quote(r.entry.name) +
        "\nnew_name=" + quote(r.newName) + "\nprotect_tree=" +
        std::string(r.operation == FileOperation::Remove || r.operation == FileOperation::Rename ? "1" : "0") + R"TTFS(
deny() { printf '%s\n' "$1" >&2; exit 74; }
cd "$parent" || exit 1
p=$(pwd -P) || deny '无法解析实际父目录'
case "$p" in /*) ;; *) deny '父目录无效';; esac
case "$p/" in /proc/*|/sys/*|/dev/*) deny '此目录保持只读';; esac
full=${p%/}/$name
t=./$name
case "$full" in /|/system|/vendor|/proc|/sys|/dev|/sdcard) deny '此目录受保护';; esac
mount_info=/proc/self/mountinfo
[ -r "$mount_info" ] || deny '无法读取挂载表，未执行变更'
seen=0
while read -r a b c d mount rest; do
  seen=1
  m=$(printf '%b' "$mount") || deny '挂载表无效'
  case "$m" in /*) ;; *) deny '挂载表路径无效';; esac
  if [ "$m" = "$full" ]; then deny '目标是挂载点'; fi
)TTFS";
    if (r.operation == FileOperation::Remove || r.operation == FileOperation::Rename)
        script += R"TTFS(  case "$m" in "$full"/*) deny '所选目录包含挂载点';; esac
)TTFS";
    script += R"TTFS(done < "$mount_info"
[ "$seen" = 1 ] || deny '挂载表为空，未执行变更'
)TTFS";
    script += R"TTFS(for protected in /system /vendor /proc /sys /dev /sdcard; do
  if [ -e "$protected" ] || [ -L "$protected" ]; then
    protected_real=$(readlink -f -- "$protected") || deny '无法解析受保护目录'
    [ "$full" != "$protected_real" ] || deny '目标为受保护目录的实际位置'
    if [ "$protect_tree" = 1 ]; then
      case "$protected_real" in "$full"/*) deny '所选目录包含受保护目录';; esac
    fi
  fi
done
)TTFS";
    if (r.operation == FileOperation::CreateFolder) return script + "mkdir -- \"$t\" || exit 1\n";
    if (r.entry.kind == FileKind::Symlink) script += "[ -L \"$t\" ] || deny '目标类型已变化'\n";
    else if (r.entry.kind == FileKind::Directory) script += "[ -d \"$t\" ] && [ ! -L \"$t\" ] || deny '目标类型已变化'\n";
    else if (r.entry.kind == FileKind::Regular) script += "[ -f \"$t\" ] && [ ! -L \"$t\" ] || deny '目标类型已变化'\n";
    else throw std::runtime_error("特殊节点保持只读");
    script += R"TTFS(identity=$(stat -c '%d:%i:%f' -- "$t") || deny '无法核对目标属性'
)TTFS";
    if (r.entry.kind != FileKind::Symlink) script += R"TTFS(resolved=$(readlink -f -- "$t") || deny '无法解析目标'
[ "$resolved" = "$full" ] || deny '目标路径已变化'
)TTFS";
    script += R"TTFS([ "$(stat -c '%d:%i:%f' -- "$t")" = "$identity" ] || deny '目标已变化'
)TTFS";
    if (r.operation == FileOperation::Rename)
        script += R"TTFS(destination=./$new_name
[ ! -e "$destination" ] && [ ! -L "$destination" ] || deny '同名目标已存在'
mv -nT -- "$t" "$destination" || exit 1
[ ! -e "$t" ] && [ ! -L "$t" ] || deny '未重命名，目标可能已存在'
)TTFS";
    else if (r.operation == FileOperation::SetPermissions) {
        const auto mode = FileTransferService::normalizePermissions(r.permissions);
        if (mode.empty() || (r.entry.kind != FileKind::Regular && r.entry.kind != FileKind::Directory))
            throw std::runtime_error("设备权限或目标类型无效");
        const std::string readMode = R"TTFS(actual=$(stat -c '%a' -- "$t") || deny '无法读取目标权限'
case "$actual" in ''|*[!0-7]*) deny '目标权限格式无效';; esac
[ "${#actual}" -le 4 ] || deny '目标权限超出范围'
)TTFS";
        const auto previous = FileTransferService::normalizePermissions(r.entry.permissions);
        if (!r.entry.permissions.empty() && previous.empty()) throw std::runtime_error("原权限无效，请重新读取");
        if (!previous.empty()) script += readMode + "[ \"$((0$actual))\" -eq " + std::to_string(std::stoul(previous, nullptr, 8)) +
            " ] || deny '权限在确认后已变化，请重新读取'\n";
        // 五位形式明确设置包括特殊位在内的完整权限，避免目录 setgid 被隐式保留；不递归。
        script += "chmod " + quote("0" + mode) + " -- \"$t\" || exit 1\n";
        script += readMode + "[ \"$((0$actual))\" -eq " + std::to_string(std::stoul(mode, nullptr, 8)) +
            " ] || deny '修改后的权限与请求不一致，请刷新复核'\n";
    } else if (r.operation == FileOperation::Remove)
        script += r.entry.kind == FileKind::Directory ? "rm -rf -- \"$t\" || exit 1\n" : "rm -- \"$t\" || exit 1\n";
    else throw std::runtime_error("文件操作无效");
    return script;
}
}
