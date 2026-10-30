#include "ui/icons.h"
#include <QtCore/QFile>
#include <QtGui/QIconEngine>
#include <QtGui/QPainter>
#include <QtGui/QPixmapCache>
#include <QtSvg/QSvgRenderer>
#include <map>
#include <memory>

namespace testtools::ui {
namespace {
const char* asset(Icon icon)
{
    switch (icon) {
    case Icon::Back:
        return "arrow-left-bold.svg";
    case Icon::Forward:
        return "arrow-right-bold.svg";
    case Icon::Up:
        return "arrow-up-bold.svg";
    case Icon::Down:
        return "arrow-down-bold.svg";
    case Icon::Refresh:
        return "arrows-clockwise-bold.svg";
    case Icon::OpenFolder:
        return "folder-open-bold.svg";
    case Icon::Upload:
        return "upload-simple-bold.svg";
    case Icon::Download:
        return "download-simple-bold.svg";
    case Icon::Favorite:
        return "star-bold.svg";
    case Icon::Search:
        return "magnifying-glass-bold.svg";
    case Icon::NewFolder:
        return "folder-simple-plus-bold.svg";
    case Icon::Rename:
        return "pencil-simple-bold.svg";
    case Icon::Delete:
        return "trash-bold.svg";
    case Icon::Folder:
    case Icon::FolderLink:
        return "folder-simple-duotone.svg";
    case Icon::Log:
        return "file-text-bold.svg";
    case Icon::Code:
        return "file-code-bold.svg";
    case Icon::Archive:
        return "file-zip-bold.svg";
    case Icon::Image:
        return "file-image-bold.svg";
    case Icon::Audio:
        return "file-audio-bold.svg";
    case Icon::Video:
        return "file-video-bold.svg";
    case Icon::Package:
        return "package-bold.svg";
    case Icon::ChevronDown:
        return "caret-down-bold.svg";
    case Icon::ChevronRight:
        return "caret-right-bold.svg";
    case Icon::ChevronLeft:
        return "caret-left-bold.svg";
    case Icon::Copy:
        return "copy-bold.svg";
    case Icon::Save:
        return "floppy-disk-bold.svg";
    case Icon::Clear:
        return "eraser-bold.svg";
    case Icon::Play:
        return "play-bold.svg";
    case Icon::Stop:
        return "stop-bold.svg";
    case Icon::Camera:
        return "camera-bold.svg";
    default:
        return "file-bold.svg";
    }
}

class SvgIconEngine final : public QIconEngine {
    Icon icon_;
    IconTone tone_;
    struct Data {
        std::map<QString, std::shared_ptr<QSvgRenderer>> renderers;
    };
    std::shared_ptr<Data> data_ = std::make_shared<Data>();

    QSvgRenderer* renderer(const QString& name, const QColor& color)
    {
        const auto key = name + color.name();
        const auto found = data_->renderers.find(key);
        if (found != data_->renderers.end())
            return found->second.get();
        QFile file(QStringLiteral(":/testtools/icons/") + name);
        if (!file.open(QIODevice::ReadOnly))
            return nullptr;
        auto svg = file.readAll();
        svg.replace("currentColor", color.name().toUtf8());
        auto value = std::make_shared<QSvgRenderer>(svg);
        if (!value->isValid())
            return nullptr;
        const auto ptr = value.get();
        data_->renderers.emplace(key, std::move(value));
        return ptr;
    }

  public:
    SvgIconEngine(Icon icon, IconTone tone) : icon_(icon), tone_(tone) {}
    QIconEngine* clone() const override
    {
        return new SvgIconEngine(*this);
    }
    QString key() const override
    {
        return QStringLiteral("TestToolsPhosphorBold");
    }
    bool isNull() override
    {
        return renderer(QString::fromLatin1(asset(icon_)), QColor("#423d39")) == nullptr;
    }
    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override
    {
        const bool folder = icon_ == Icon::Folder || icon_ == Icon::FolderLink;
        const bool linked = icon_ == Icon::FileLink || icon_ == Icon::FolderLink;
        const bool favorite = icon_ == Icon::Favorite && state == QIcon::On;
        const QColor color(mode == QIcon::Disabled        ? "#aaa29b"
                           : tone_ == IconTone::OnPrimary ? "#ffffff"
                           : folder                       ? "#e4aa32"
                           : tone_ == IconTone::Accent || favorite || mode == QIcon::Active ||
                                   mode == QIcon::Selected
                               ? "#ad694e"
                               : "#423d39");
        const auto name =
            favorite ? QStringLiteral("star-fill.svg") : QString::fromLatin1(asset(icon_));
        auto* svg = renderer(name, color);
        if (!svg)
            return;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const QRectF box(rect);
        svg->render(painter,
                    linked ? box.adjusted(0, 0, -box.width() * .18, -box.height() * .18) : box);
        if (icon_ == Icon::Config) {
            // 使用同库齿轮组合到文件轮廓内，不手绘新的图形路径。
            if (auto* gear = renderer(QStringLiteral("gear-six-bold.svg"), color))
                gear->render(painter,
                             QRectF(box.x() + box.width() * .32,
                                    box.y() + box.height() * .40,
                                    box.width() * .38,
                                    box.height() * .38));
        }
        if (linked) {
            const QColor linkColor(mode == QIcon::Disabled ? "#aaa29b" : "#423d39");
            if (auto* link = renderer(QStringLiteral("link-simple-bold.svg"), linkColor))
                link->render(painter,
                             QRectF(box.x() + box.width() * .54,
                                    box.y() + box.height() * .54,
                                    box.width() * .46,
                                    box.height() * .46));
        }
        painter->restore();
    }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override
    {
        return scaledPixmap(size, mode, state, 1.0);
    }
    QPixmap
    scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale) override
    {
        // Qt 6.8 起 size 为逻辑尺寸，scale 为实际 DPR（本项目 Qt 6.11）。
        const auto key = QStringLiteral("tt-icon:%1:%2:%3:%4:%5x%6:%7")
                             .arg(int(icon_))
                             .arg(int(tone_))
                             .arg(int(mode))
                             .arg(int(state))
                             .arg(size.width())
                             .arg(size.height())
                             .arg(scale, 0, 'f', 3);
        QPixmap cached;
        if (QPixmapCache::find(key, &cached))
            return cached;
        QPixmap value((QSizeF(size) * scale).toSize());
        value.setDevicePixelRatio(scale);
        value.fill(Qt::transparent);
        QPainter painter(&value);
        paint(&painter, QRect(QPoint(), size), mode, state);
        painter.end();
        QPixmapCache::insert(key, value);
        return value;
    }
};
} // namespace

QIcon appIcon(Icon icon, IconTone tone)
{
    static std::map<std::pair<Icon, IconTone>, QIcon> icons;
    const auto key = std::make_pair(icon, tone);
    const auto found = icons.find(key);
    if (found != icons.end())
        return found->second;
    auto value = QIcon(new SvgIconEngine(icon, tone));
    icons.emplace(key, value);
    return value;
}
} // namespace testtools::ui
