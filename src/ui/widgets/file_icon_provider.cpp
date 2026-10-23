#include "ui/widgets/file_icon_provider.h"
#include <QtCore/QMetaObject>
#include <QtCore/QPointer>
#include <QtGui/QImage>
#include <QtGui/QPixmap>
#include <vector>
#ifdef _WIN32
#include <objbase.h>
#include <shellapi.h>
#include <windows.h>
#endif

FileIconProvider::FileIconProvider(QObject* parent)
    : QObject(parent), cancelled_(std::make_shared<std::atomic<bool>>(false))
{
    timer_.setSingleShot(true);
    timer_.setInterval(20);
    connect(&timer_, &QTimer::timeout, this, [this] { dispatch(); });
}
FileIconProvider::~FileIconProvider()
{
    *cancelled_ = true;
    tasks_.wait();
}
QIcon FileIconProvider::icon(const QString& key,
                             const QString& path,
                             bool attributesOnly,
                             bool directory,
                             bool link,
                             const QIcon& fallback)
{
    if (const auto* found = cache_.object(key))
        return *found;
    if (!inFlight_.contains(key) && pending_.size() < 512)
        pending_.insert(key, {key, path, attributesOnly, directory, link, fallback});
    if (!busy_ && !timer_.isActive())
        timer_.start();
    return fallback;
}
void FileIconProvider::dispatch()
{
    tasks_.reapFinished();
    if (busy_ || pending_.isEmpty())
        return;
    std::vector<Request> batch;
    while (!pending_.isEmpty() && batch.size() < 12) {
        auto it = pending_.begin();
        batch.push_back(it.value());
        inFlight_.insert(it.key());
        pending_.erase(it);
    }
    busy_ = true;
    QPointer<FileIconProvider> guard(this);
    tasks_.start([guard, batch = std::move(batch), cancel = cancelled_] {
        std::vector<QImage> images;
#ifdef _WIN32
        const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
#endif
        for (const auto& request : batch) {
            QImage image;
            if (*cancel)
                break;
#ifdef _WIN32
            SHFILEINFOW info{};
            const auto path = request.path.toStdWString();
            UINT flags = SHGFI_ICON | SHGFI_LARGEICON;
            if (request.attributesOnly)
                flags |= SHGFI_USEFILEATTRIBUTES;
            if (request.link)
                flags |= SHGFI_LINKOVERLAY;
            if (SHGetFileInfoW(path.c_str(),
                               request.directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL,
                               &info,
                               sizeof(info),
                               flags) &&
                info.hIcon) {
                image = QImage::fromHICON(info.hIcon);
                DestroyIcon(info.hIcon);
            }
#endif
            images.push_back(std::move(image));
        }
#ifdef _WIN32
        if (SUCCEEDED(com))
            CoUninitialize();
#endif
        if (*cancel || !guard)
            return;
        QMetaObject::invokeMethod(
            guard,
            [guard, batch, images = std::move(images)] {
                if (!guard)
                    return;
                guard->busy_ = false;
                for (std::size_t i = 0; i < images.size(); ++i) {
                    guard->inFlight_.remove(batch[i].key);
                    const auto icon = images[i].isNull() ? batch[i].fallback
                                                         : QIcon(QPixmap::fromImage(images[i]));
                    guard->cache_.insert(batch[i].key, new QIcon(icon));
                }
                if (guard->changed)
                    guard->changed();
                if (!guard->pending_.isEmpty())
                    guard->timer_.start();
            },
            Qt::QueuedConnection);
    });
}
