#pragma once
#include <QtGui/QIcon>

namespace testtools::ui {
enum class Icon {
    Back,
    Forward,
    Up,
    Down,
    Refresh,
    OpenFolder,
    Upload,
    Download,
    Favorite,
    Search,
    NewFolder,
    Rename,
    Delete,
    Folder,
    File,
    Log,
    Config,
    Code,
    Archive,
    Image,
    Audio,
    Video,
    Package,
    FileLink,
    FolderLink,
    ChevronDown,
    ChevronRight,
    ChevronLeft,
    Copy,
    Save,
    Clear,
    Play,
    Stop,
    Camera
};
enum class IconTone { Normal, Accent, OnPrimary };

// 所有资源来自内置 SVG；按图标和色调复用 QIcon，不查询 Windows Shell。
QIcon appIcon(Icon icon, IconTone tone = IconTone::Normal);
} // namespace testtools::ui
