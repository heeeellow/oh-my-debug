#pragma once

#include "application/support/background_task_group.h"
#include <QtCore/QCache>
#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QSet>
#include <QtCore/QTimer>
#include <QtGui/QIcon>
#include <atomic>
#include <functional>

// Windows Shell work stays off the GUI thread. QPixmap creation stays on it.
class FileIconProvider final : public QObject {
  public:
    explicit FileIconProvider(QObject* parent);
    ~FileIconProvider() override;
    QIcon icon(const QString& key,
               const QString& path,
               bool attributesOnly,
               bool directory,
               bool link,
               const QIcon& fallback);
    std::function<void()> changed;

  private:
    struct Request {
        QString key, path;
        bool attributesOnly, directory, link;
        QIcon fallback;
    };
    void dispatch();
    QCache<QString, QIcon> cache_{512};
    QHash<QString, Request> pending_;
    QSet<QString> inFlight_;
    QTimer timer_;
    testtools::BackgroundTaskGroup tasks_;
    std::shared_ptr<std::atomic<bool>> cancelled_;
    bool busy_ = false;
};
