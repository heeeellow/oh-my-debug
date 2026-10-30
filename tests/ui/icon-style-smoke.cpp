#include "test_paths.h"
#include "ui/icons.h"
#include "ui/theme.h"
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtWidgets>
#include <iostream>
#include <stdexcept>

using testtools::ui::appIcon;
using testtools::ui::Icon;
using testtools::ui::IconTone;
namespace {
void check(bool value, const char* message)
{
    if (!value)
        throw std::runtime_error(message);
}
int visiblePixels(const QPixmap& pixmap)
{
    const auto image = pixmap.toImage().convertToFormat(QImage::Format_ARGB32);
    int count = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            count += qAlpha(image.pixel(x, y)) > 20;
    return count;
}
QLabel* label(const QString& text, int pixels, bool bold = false)
{
    auto* value = new QLabel(text);
    value->setStyleSheet(QStringLiteral("font-size:%1px;font-weight:%2;color:#423d39;")
                             .arg(pixels)
                             .arg(bold ? 600 : 400));
    return value;
}
class IconSample final : public QWidget {
    QIcon icon_;
    QIcon::Mode mode_;
    QIcon::State state_;

  public:
    IconSample(Icon icon,
               int size,
               IconTone tone = IconTone::Normal,
               QIcon::Mode mode = QIcon::Normal,
               QIcon::State state = QIcon::Off)
        : icon_(appIcon(icon, tone)), mode_(mode), state_(state)
    {
        setFixedSize(size, size);
    }
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        icon_.paint(&painter, rect(), Qt::AlignCenter, mode_, state_);
    }
};
void captureBoard(qreal dpr, const QString& name)
{
    QWidget board;
    board.setObjectName("iconSpecimen");
    board.setFixedSize(1440, 800);
    board.setStyleSheet(QStringLiteral("QWidget#iconSpecimen {background:#fbfaf8;}"));
    auto* layout = new QVBoxLayout(&board);
    layout->setContentsMargins(48, 22, 48, 24);
    layout->setSpacing(14);
    layout->addWidget(label(QStringLiteral("清晰线性"), 40, true));
    layout->addWidget(label(QStringLiteral("TestTools · 图标风格预览"), 22));
    auto* toolbar = new QHBoxLayout;
    toolbar->setSpacing(12);
    for (const auto glyph : {Icon::ChevronLeft, Icon::ChevronRight, Icon::Up, Icon::Refresh}) {
        auto* button = new QToolButton;
        button->setIcon(appIcon(glyph));
        button->setIconSize({20, 20});
        button->setFixedSize(36, 36);
        toolbar->addWidget(button);
    }
    auto* address = new QComboBox;
    address->setEditable(true);
    address->addItem(QStringLiteral("/sdcard/oem_log"));
    toolbar->addWidget(address, 1);
    for (const auto glyph : {Icon::Favorite, Icon::OpenFolder}) {
        auto* button = new QToolButton;
        button->setIcon(appIcon(glyph));
        button->setIconSize({20, 20});
        button->setFixedSize(36, 36);
        toolbar->addWidget(button);
    }
    auto* download = new QPushButton(QStringLiteral("下载所选"));
    download->setProperty("buttonRole", "primary");
    download->setIcon(appIcon(Icon::Download, IconTone::OnPrimary));
    download->setIconSize({20, 20});
    toolbar->addWidget(download);
    layout->addLayout(toolbar);
    const std::pair<Icon, const char*> actions[] = {{Icon::Back, u8"后退"},
                                                    {Icon::Forward, u8"前进"},
                                                    {Icon::Up, u8"上一级"},
                                                    {Icon::Refresh, u8"刷新"},
                                                    {Icon::OpenFolder, u8"打开目录"},
                                                    {Icon::Upload, u8"上传"},
                                                    {Icon::Download, u8"下载"},
                                                    {Icon::Favorite, u8"收藏"},
                                                    {Icon::Search, u8"搜索"},
                                                    {Icon::NewFolder, u8"新建文件夹"},
                                                    {Icon::Rename, u8"重命名"},
                                                    {Icon::Delete, u8"删除"}};
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(24);
    grid->setVerticalSpacing(22);
    for (int i = 0; i < 12; ++i) {
        auto* cell = new QVBoxLayout;
        cell->setSpacing(10);
        cell->setAlignment(Qt::AlignCenter);
        const auto tone = actions[i].first == Icon::Download ? IconTone::Accent : IconTone::Normal;
        cell->addWidget(new IconSample(actions[i].first, 64, tone), 0, Qt::AlignHCenter);
        auto* title = label(QString::fromUtf8(actions[i].second), 20);
        title->setAlignment(Qt::AlignCenter);
        cell->addWidget(title);
        grid->addLayout(cell, i / 6, i % 6);
    }
    layout->addLayout(grid, 1);
    auto* types = new QHBoxLayout;
    types->setSpacing(30);
    types->addWidget(label(QStringLiteral("设备文件类型"), 20, true));
    for (const auto& entry : {std::make_pair(Icon::Folder, QStringLiteral("文件夹")),
                              {Icon::Log, QStringLiteral("日志")},
                              {Icon::Config, QStringLiteral("配置")},
                              {Icon::Archive, QStringLiteral("压缩包")}}) {
        types->addWidget(new IconSample(entry.first, 40));
        types->addWidget(label(entry.second, 20));
    }
    layout->addLayout(types);
    auto* states = new QHBoxLayout;
    states->addWidget(label(QStringLiteral("实际 20px 状态"), 14));
    for (const auto mode : {QIcon::Normal, QIcon::Active, QIcon::Disabled}) {
        states->addWidget(new IconSample(Icon::Refresh, 20, IconTone::Normal, mode));
    }
    states->addWidget(
        new IconSample(Icon::Favorite, 20, IconTone::Normal, QIcon::Normal, QIcon::On));
    states->addStretch();
    layout->addLayout(states);
    board.ensurePolished();
    layout->activate();
    QPixmap pixels((QSizeF(board.size()) * dpr).toSize());
    pixels.setDevicePixelRatio(dpr);
    pixels.fill(Qt::transparent);
    QPainter painter(&pixels);
    board.render(&painter);
    painter.end();
    check(pixels.save(testOutputPath(name + ".png")), "save icon specimen");
    QFile metadata(testOutputPath(name + ".json"));
    check(metadata.open(QIODevice::WriteOnly), "icon specimen metadata");
    metadata.write(QJsonDocument(QJsonObject{{"logical_width", 1440},
                                             {"logical_height", 800},
                                             {"pixel_width", pixels.width()},
                                             {"pixel_height", pixels.height()},
                                             {"device_pixel_ratio", dpr},
                                             {"source", "actual embedded application icons"}})
                       .toJson());
}
} // namespace
int main(int argc, char** argv)
{
    try {
        QApplication app(argc, argv);
        app.setStyle("Fusion");
        app.setFont(QFont("Microsoft YaHei UI", 10));
        app.setStyleSheet(testtools::ui::applicationStyleSheet());
        for (int id = 0; id <= int(Icon::Camera); ++id) {
            const auto icon = appIcon(Icon(id));
            check(!icon.isNull(), "embedded SVG exists and parses");
            check(icon.cacheKey() == appIcon(Icon(id)).cacheKey(),
                  "icon requests reuse the cached handle");
            for (const auto dpr : {1.0, 1.25, 1.5, 2.0})
                for (const auto size : {16, 20, 24}) {
                    const auto pix = icon.pixmap({size, size}, dpr);
                    check(!pix.isNull() && qAbs(pix.width() - size * dpr) <= 1 &&
                              qAbs(pix.height() - size * dpr) <= 1 && visiblePixels(pix) > 4,
                          "scalable icon has the requested size and visible pixels");
                }
            check(visiblePixels(icon.pixmap({20, 20}, 1.5, QIcon::Disabled)) > 4,
                  "disabled icon remains visible");
        }
        const auto star = appIcon(Icon::Favorite);
        check(visiblePixels(star.pixmap({24, 24}, 1.0, QIcon::Normal, QIcon::On)) >
                  visiblePixels(star.pixmap({24, 24}, 1.0, QIcon::Normal, QIcon::Off)),
              "favorite checked state uses the filled glyph");
        check(appIcon(Icon::Download, IconTone::OnPrimary).pixmap({20, 20}, 1.0).toImage() !=
                  appIcon(Icon::Download).pixmap({20, 20}, 1.0).toImage(),
              "primary action icon has a distinct contrast color");
        if (qEnvironmentVariableIsSet("TESTTOOLS_ICON_SNAPSHOTS")) {
            captureBoard(1.0, "icons-clear-linear");
            captureBoard(1.5, "icons-clear-linear-highdpi");
        }
        std::cout << "PASS embedded icon resources, cached handles, 16/20/24px at DPR "
                     "1/1.25/1.5/2, disabled/primary/favorite states\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
