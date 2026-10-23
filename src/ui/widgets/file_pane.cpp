#include "ui/widgets/file_pane.h"
#include "application/device/file_transfer_service.h"
#include "ui/widgets/file_icon_provider.h"
#include <QtCore/QCollator>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QMimeData>
#include <QtGui/QClipboard>
#include <QtGui/QDrag>
#include <QtGui/QDragEnterEvent>
#include <QtGui/QShortcut>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMenu>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QStyle>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QTreeView>
#include <QtWidgets/QVBoxLayout>
#include <algorithm>

namespace {
QString text(const std::string& s)
{
    return QString::fromUtf8(s.data(), qsizetype(s.size()));
}
QString type(const testtools::FileEntry& e)
{
    using K = testtools::FileKind;
    if (e.kind == K::Symlink)
        return e.browsable() ? QStringLiteral("文件夹链接") : QStringLiteral("文件链接 / 不可达");
    if (e.kind == K::Directory)
        return QStringLiteral("文件夹");
    if (e.kind == K::Other)
        return QStringLiteral("特殊节点");
    const auto suffix = text(e.name).section('.', -1).toLower();
    if (suffix == "apk")
        return QStringLiteral("Android 安装包");
    if (suffix == "log")
        return QStringLiteral("日志文件");
    static const QStringList config = {
        "conf", "cfg", "ini", "prop", "rc", "yaml", "yml", "toml", "xml", "json"};
    static const QStringList documents = {"txt", "md", "csv", "c", "cpp", "h", "dts", "dtsi"};
    static const QStringList images = {"png", "jpg", "jpeg", "bmp", "gif", "webp", "svg"};
    static const QStringList audio = {"wav", "mp3", "pcm", "aac", "amr", "ogg", "flac"};
    static const QStringList video = {"mp4", "mkv", "avi", "webm", "mov"};
    static const QStringList scripts = {"sh", "bash", "py", "bat", "cmd", "ps1"};
    static const QStringList archives = {"zip", "gz", "tgz", "tar", "bz2", "xz", "7z", "rar"};
    if (config.contains(suffix))
        return QStringLiteral("配置文件");
    if (documents.contains(suffix))
        return QStringLiteral("文本文档");
    if (images.contains(suffix))
        return QStringLiteral("图像文件");
    if (audio.contains(suffix))
        return QStringLiteral("音频文件");
    if (video.contains(suffix))
        return QStringLiteral("视频文件");
    if (scripts.contains(suffix))
        return QStringLiteral("脚本文件");
    if (archives.contains(suffix))
        return QStringLiteral("压缩文件");
    if (suffix == "so" || suffix == "a" || suffix == "dll" || text(e.name).contains(".so."))
        return QStringLiteral("库文件");
    return suffix == text(e.name) ? QStringLiteral("文件")
                                  : suffix.toUpper() + QStringLiteral(" 文件");
}
class FileProxy final : public QSortFilterProxyModel {
  public:
    explicit FileProxy(QObject* p) : QSortFilterProxyModel(p)
    {
        collator_.setNumericMode(true);
        collator_.setCaseSensitivity(Qt::CaseInsensitive);
    }

  protected:
    bool lessThan(const QModelIndex& a, const QModelIndex& b) const override
    {
        const auto* m = static_cast<FileListModel*>(sourceModel());
        const auto& x = m->entry(a.row());
        const auto& y = m->entry(b.row());
        if (x.browsable() != y.browsable())
            return sortOrder() == Qt::AscendingOrder ? x.browsable() : y.browsable();
        if (a.column() == 1)
            return x.size < y.size;
        if (a.column() == 3)
            return text(x.modified).toLongLong() < text(y.modified).toLongLong();
        return collator_.compare(sourceModel()->data(a).toString(),
                                 sourceModel()->data(b).toString()) < 0;
    }

  private:
    QCollator collator_;
};
class FileView final : public QTreeView {
  public:
    explicit FileView(FilePane* owner) : QTreeView(owner), owner_(owner)
    {
        setDragEnabled(true);
        setAcceptDrops(true);
        setDropIndicatorShown(true);
    }

  protected:
    void startDrag(Qt::DropActions) override
    {
        if (!owner_->dragData)
            return;
        auto* mime = owner_->dragData();
        if (!mime)
            return;
        auto* drag = new QDrag(this);
        drag->setMimeData(mime);
        drag->exec(Qt::CopyAction);
        drag->deleteLater();
    }
    void dragEnterEvent(QDragEnterEvent* e) override
    {
        if (e->mimeData()->hasFormat("application/x-testtools-files") || e->mimeData()->hasUrls())
            e->acceptProposedAction();
    }
    void dragMoveEvent(QDragMoveEvent* e) override
    {
        e->setDropAction(Qt::CopyAction);
        e->accept();
    }
    void dropEvent(QDropEvent* e) override
    {
        if (owner_->dropData && owner_->dropData(e->mimeData())) {
            e->setDropAction(Qt::CopyAction);
            e->accept();
        }
    }

  private:
    FilePane* owner_;
};
} // namespace
FileListModel::FileListModel(QObject* parent, bool remote)
    : QAbstractTableModel(parent), iconProvider_(new FileIconProvider(this)), remote_(remote)
{
    const QStyle::StandardPixmap symbols[] = {QStyle::SP_FileIcon,
                                              QStyle::SP_DirIcon,
                                              QStyle::SP_DirLinkIcon,
                                              QStyle::SP_FileLinkIcon,
                                              QStyle::SP_MediaVolume,
                                              QStyle::SP_FileDialogContentsView,
                                              QStyle::SP_FileDialogDetailedView,
                                              QStyle::SP_ComputerIcon};
    for (int i = 0; i < 8; ++i)
        icons_[i] = QApplication::style()->standardIcon(symbols[i]);
    iconProvider_->changed = [this] {
        if (rowCount())
            emit dataChanged(index(0, 0), index(rowCount() - 1, 0), {Qt::DecorationRole});
    };
}
int FileListModel::rowCount(const QModelIndex& p) const
{
    return p.isValid() ? 0 : int(entries_.size());
}
int FileListModel::columnCount(const QModelIndex& p) const
{
    return p.isValid() ? 0 : 4;
}
const testtools::FileEntry& FileListModel::entry(int row) const
{
    return entries_.at(std::size_t(row));
}
void FileListModel::replace(std::vector<testtools::FileEntry> e, const QString& directory)
{
    beginResetModel();
    entries_ = std::move(e);
    directory_ = directory;
    endResetModel();
}
QVariant FileListModel::headerData(int column, Qt::Orientation o, int role) const
{
    if (o != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    return QStringList{QStringLiteral("名称"),
                       QStringLiteral("大小"),
                       QStringLiteral("类型"),
                       QStringLiteral("修改时间")}
        .value(column);
}
QVariant FileListModel::data(const QModelIndex& i, int role) const
{
    if (!i.isValid())
        return {};
    const auto& e = entry(i.row());
    if (role == Qt::UserRole)
        return text(e.name);
    if (role == Qt::ToolTipRole)
        return text(e.name) +
               (e.linkTarget.empty() ? QString()
                                     : QStringLiteral("\n链接到：") + text(e.linkTarget)) +
               (e.metadataError.empty() ? QString() : "\n" + text(e.metadataError));
    if (role == Qt::DecorationRole && i.column() == 0) {
        int icon = 0;
        if (e.kind == testtools::FileKind::Symlink)
            icon = e.browsable() ? 2 : 3;
        else if (e.browsable())
            icon = 1;
        else {
            const auto category = type(e);
            if (category == QStringLiteral("音频文件"))
                icon = 4;
            else if (category == QStringLiteral("图像文件"))
                icon = 5;
            else if (category == QStringLiteral("文本文档") ||
                     category == QStringLiteral("日志文件") ||
                     category == QStringLiteral("配置文件"))
                icon = 6;
            else if (category == QStringLiteral("Android 安装包"))
                icon = 7;
        }
        const auto name = text(e.name);
        auto extension = QFileInfo(name).suffix().toLower();
        QString requestPath = QDir(directory_).filePath(name);
        bool byAttributes = remote_;
        const bool link = e.kind == testtools::FileKind::Symlink;
        if (remote_) {
            // No download/content sniffing for remote icons; use common Shell file types.
            if (icon == 4)
                extension = "wav";
            else if (icon == 5)
                extension = "png";
            else if (icon == 6)
                extension = "txt";
            else if (type(e) == QStringLiteral("脚本文件"))
                extension = "bat";
            else if (type(e) == QStringLiteral("库文件"))
                extension = "dll";
            else if (type(e) == QStringLiteral("视频文件"))
                extension = "mp4";
            else if (extension == "apk")
                extension = "exe";
            else if (type(e) == QStringLiteral("压缩文件"))
                extension = "zip";
            requestPath = "file." + extension;
        } else if (!e.browsable() && !link && extension != "exe" && extension != "lnk" &&
                   extension != "ico") {
            byAttributes = true;
            requestPath = "file." + extension;
        }
        const auto key = (remote_ ? "remote:" : "local:") +
                         (byAttributes ? (e.browsable() ? QStringLiteral("folder") : extension)
                                       : requestPath + ":" + text(e.modified)) +
                         (link ? ":link" : "");
        return iconProvider_->icon(
            key, requestPath, byAttributes, e.browsable(), link, icons_[icon]);
    }
    if (role == Qt::TextAlignmentRole && i.column() == 1)
        return int(Qt::AlignRight | Qt::AlignVCenter);
    if (role != Qt::DisplayRole)
        return {};
    switch (i.column()) {
    case 0:
        return text(e.name);
    case 1:
        if (e.kind != testtools::FileKind::Regular)
            return QStringLiteral("—");
        if (e.size >= 1048576)
            return QStringLiteral("%1 MB").arg(double(e.size) / 1048576, 0, 'f', 1);
        if (e.size >= 1024)
            return QStringLiteral("%1 KB").arg(double(e.size) / 1024, 0, 'f', 1);
        return QStringLiteral("%1 B").arg(qulonglong(e.size));
    case 2:
        return type(e);
    case 3: {
        bool ok;
        const auto epoch = text(e.modified).toLongLong(&ok);
        return ok && epoch > 0 ? QDateTime::fromSecsSinceEpoch(epoch).toString("yyyy-MM-dd HH:mm")
                               : QStringLiteral("—");
    }
    }
    return {};
}

FilePane::FilePane(bool remote, QWidget* parent) : QFrame(parent), remote_(remote)
{
    setObjectName("filePane");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 10);
    layout->setSpacing(8);
    auto* heading = new QHBoxLayout;
    auto* label =
        new QLabel(remote ? QStringLiteral("设备文件") : QStringLiteral("本地文件"), this);
    label->setObjectName("sectionLabel");
    heading->addWidget(label);
    heading->addStretch();
    filter_ = new QLineEdit(this);
    filter_->setObjectName(remote ? "fileNameFilter" : "fileLocalFilter");
    filter_->setPlaceholderText(QStringLiteral("筛选文件名"));
    filter_->setClearButtonEnabled(true);
    filter_->setMaximumWidth(200);
    heading->addWidget(filter_);
    layout->addLayout(heading);
    auto* bar = new QHBoxLayout;
    bar->setSpacing(4);
    auto tool = [&](QStyle::StandardPixmap icon, const QString& name, auto callback) {
        auto* b = new QToolButton(this);
        b->setIcon(style()->standardIcon(icon));
        b->setToolTip(name);
        b->setAccessibleName(name);
        b->setAutoRaise(true);
        b->setFixedSize(30, 30);
        bar->addWidget(b);
        navigation_.push_back(b);
        connect(b, &QToolButton::clicked, this, callback);
        return b;
    };
    tool(QStyle::SP_ArrowBack, QStringLiteral("后退 · Alt+Left"), [this] {
        if (historyIndex_ > 0) {
            pendingHistory_ = historyIndex_ - 1;
            navigate(history_[pendingHistory_]);
        }
    });
    tool(QStyle::SP_ArrowForward, QStringLiteral("前进 · Alt+Right"), [this] {
        if (historyIndex_ + 1 < history_.size()) {
            pendingHistory_ = historyIndex_ + 1;
            navigate(history_[pendingHistory_]);
        }
    });
    tool(QStyle::SP_FileDialogToParent, QStringLiteral("上一级 · Backspace"), [this] {
        navigate(remote_ ? text(testtools::FileTransferService::normalizeRemoteDirectory(
                               pathEdit()->text().toUtf8().toStdString() + "/.."))
                         : QDir::cleanPath(pathEdit()->text() + "/.."));
    });
    tool(QStyle::SP_BrowserReload, QStringLiteral("刷新 · F5"), [this] {
        if (openRequested)
            openRequested();
    });
    address_ = new QComboBox(this);
    address_->setEditable(true);
    address_->setInsertPolicy(QComboBox::NoInsert);
    address_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    address_->setMinimumWidth(80);
    address_->setObjectName(remote ? "fileRemoteAddress" : "fileLocalAddress");
    address_->lineEdit()->setObjectName(remote ? "fileRemotePath" : "fileLocalPath");
    address_->lineEdit()->setPlaceholderText(remote ? QStringLiteral("输入设备绝对路径")
                                                    : QStringLiteral("本地保存目录"));
    navigation_.push_back(address_);
    bar->addWidget(address_, 1);
    tool(remote ? QStyle::SP_DialogOpenButton : QStyle::SP_DirOpenIcon,
         remote ? QStringLiteral("打开目录") : QStringLiteral("选择本地目录"),
         [this] {
             if (remote_) {
                 if (openRequested)
                     openRequested();
             } else {
                 auto dir = QFileDialog::getExistingDirectory(
                     this, QStringLiteral("选择本地保存目录"), pathEdit()->text());
                 if (!dir.isEmpty())
                     navigate(dir);
             }
         });
    layout->addLayout(bar);
    message_ = new QLabel(this);
    message_->setObjectName("filePaneMessage");
    message_->setWordWrap(true);
    message_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    message_->hide();
    layout->addWidget(message_);
    model_ = new FileListModel(this, remote);
    proxy_ = new FileProxy(this);
    proxy_->setSourceModel(model_);
    proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    proxy_->setFilterKeyColumn(0);
    view_ = new FileView(this);
    view_->setObjectName(remote ? "fileRemoteTable" : "fileLocalTable");
    view_->setModel(proxy_);
    view_->setRootIsDecorated(false);
    view_->setUniformRowHeights(true);
    view_->setAlternatingRowColors(true);
    view_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view_->setSelectionBehavior(QAbstractItemView::SelectRows);
    view_->setSortingEnabled(true);
    view_->sortByColumn(0, Qt::AscendingOrder);
    view_->setIconSize({20, 20});
    view_->setAllColumnsShowFocus(true);
    view_->setMinimumWidth(240);
    view_->header()->setSectionResizeMode(QHeaderView::Interactive);
    view_->header()->setStretchLastSection(false);
    view_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    view_->setColumnWidth(1, 72);
    view_->ensurePolished();
    view_->setColumnWidth(
        2,
        std::max(96,
                 view_->fontMetrics().horizontalAdvance(QStringLiteral("Android 安装包")) + 16));
    view_->setColumnWidth(
        3,
        std::max(132,
                 view_->fontMetrics().horizontalAdvance(QStringLiteral("2000-00-00 00:00")) + 20));
    view_->header()->setMinimumSectionSize(60);
    layout->addWidget(view_, 1);
    auto* footer = new QHBoxLayout;
    summary_ = new QLabel(this);
    summary_->setObjectName("mutedLabel");
    footer->addWidget(summary_, 1);
    transfer_ =
        new QPushButton(remote ? QStringLiteral("下载所选") : QStringLiteral("上传所选"), this);
    transfer_->setObjectName(remote ? "fileDownloadButton" : "fileUploadButton");
    transfer_->setIcon(style()->standardIcon(remote ? QStyle::SP_ArrowDown : QStyle::SP_ArrowUp));
    footer->addWidget(transfer_);
    layout->addLayout(footer);
    target_ = new QLabel(this);
    target_->setObjectName("mutedLabel");
    target_->setWordWrap(true);
    target_->setMinimumWidth(0);
    layout->addWidget(target_);
    connect(filter_, &QLineEdit::textChanged, this, [this](const QString& value) {
        proxy_->setFilterFixedString(value);
        updateSummary();
        if (selectionChanged)
            selectionChanged();
    });
    connect(pathEdit(), &QLineEdit::returnPressed, this, [this] {
        if (openRequested)
            openRequested();
    });
    connect(address_, &QComboBox::textActivated, this, [this](const QString&) {
        if (openRequested)
            openRequested();
    });
    connect(pathEdit(), &QLineEdit::textChanged, this, [this] {
        if (selectionChanged)
            selectionChanged();
    });
    connect(view_->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] {
        updateSummary();
        if (selectionChanged)
            selectionChanged();
    });
    connect(view_, &QTreeView::doubleClicked, this, [this](const QModelIndex& i) { openEntry(i); });
    connect(transfer_, &QPushButton::clicked, this, [this] {
        if (transferRequested)
            transferRequested();
    });
    auto shortcut = [this](const QKeySequence& key, auto callback) {
        auto* s = new QShortcut(key, this);
        s->setContext(Qt::WidgetWithChildrenShortcut);
        connect(s, &QShortcut::activated, this, callback);
    };
    shortcut(QKeySequence(Qt::Key_F5), [this] {
        if (openRequested)
            openRequested();
    });
    shortcut(QKeySequence(Qt::Key_Backspace), [this] {
        if (view_->hasFocus())
            navigate(remote_ ? text(testtools::FileTransferService::normalizeRemoteDirectory(
                                   loadedPath_.toUtf8().toStdString() + "/.."))
                             : QDir::cleanPath(loadedPath_ + "/.."));
    });
    shortcut(QKeySequence(Qt::ALT | Qt::Key_Left), [this] {
        if (historyIndex_ > 0) {
            pendingHistory_ = historyIndex_ - 1;
            navigate(history_[pendingHistory_]);
        }
    });
    shortcut(QKeySequence(Qt::ALT | Qt::Key_Right), [this] {
        if (historyIndex_ + 1 < history_.size()) {
            pendingHistory_ = historyIndex_ + 1;
            navigate(history_[pendingHistory_]);
        }
    });
    auto* enter = new QShortcut(QKeySequence(Qt::Key_Return), view_);
    enter->setContext(Qt::WidgetShortcut);
    connect(enter, &QShortcut::activated, this, [this] { openEntry(view_->currentIndex()); });
    view_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(view_, &QTreeView::customContextMenuRequested, this, [this](const QPoint& p) {
        QMenu menu;
        auto* copy = menu.addAction(style()->standardIcon(QStyle::SP_FileDialogContentsView),
                                    QStringLiteral("复制完整路径"));
        copy->setEnabled(!selectedEntries().empty());
        if (menu.exec(view_->viewport()->mapToGlobal(p)) == copy) {
            QStringList paths;
            for (const auto& e : selectedEntries())
                paths << QDir::cleanPath(loadedPath_ + "/" + text(e.name));
            QApplication::clipboard()->setText(paths.join('\n'));
        }
    });
    updateSummary();
}
QLineEdit* FilePane::pathEdit() const
{
    return address_->lineEdit();
}
void FilePane::navigate(const QString& dir)
{
    pathEdit()->setText(remote_ ? dir : QDir::toNativeSeparators(dir));
    if (openRequested)
        openRequested();
}
void FilePane::openEntry(const QModelIndex& i)
{
    if (!i.isValid())
        return;
    const auto& e = model_->entry(proxy_->mapToSource(i).row());
    if (e.browsable())
        navigate(QDir::cleanPath(loadedPath_ + "/" + text(e.name)));
}
std::vector<testtools::FileEntry> FilePane::selectedEntries() const
{
    std::vector<testtools::FileEntry> out;
    for (const auto& i : view_->selectionModel()->selectedRows())
        if (!view_->isRowHidden(i.row(), {}))
            out.push_back(model_->entry(proxy_->mapToSource(i).row()));
    return out;
}
void FilePane::selectNames(const QStringList& names)
{
    view_->selectionModel()->clearSelection();
    for (int row = 0; row < proxy_->rowCount(); ++row) {
        const auto i = proxy_->index(row, 0);
        if (names.contains(i.data(Qt::UserRole).toString()))
            view_->selectionModel()->select(
                i, QItemSelectionModel::Select | QItemSelectionModel::Rows);
    }
}
void FilePane::updateSummary()
{
    summary_->setText(
        QStringLiteral("%1 项 · 已选 %2 项").arg(proxy_->rowCount()).arg(selectedEntries().size()));
}
void FilePane::applyListing(const testtools::FileListing& result, const QString& requested)
{
    const bool same =
        QDir::fromNativeSeparators(loadedPath_) == QDir::fromNativeSeparators(text(result.path));
    QStringList names;
    for (const auto& e : selectedEntries())
        names << text(e.name);
    const int scroll = view_->verticalScrollBar()->value();
    model_->replace(result.error.empty() ? result.entries : std::vector<testtools::FileEntry>{},
                    text(result.path));
    loadedPath_ = result.error.empty() ? text(result.path) : QString();
    if (result.error.empty()) {
        if (pathEdit()->text() == requested)
            pathEdit()->setText(remote_ ? loadedPath_ : QDir::toNativeSeparators(loadedPath_));
        if (pendingHistory_ >= 0 && pendingHistory_ < history_.size() &&
            QDir::fromNativeSeparators(history_[pendingHistory_]) ==
                QDir::fromNativeSeparators(loadedPath_))
            historyIndex_ = pendingHistory_;
        else if (historyIndex_ < 0 || history_[historyIndex_] != loadedPath_) {
            while (history_.size() > historyIndex_ + 1)
                history_.removeLast();
            history_ << loadedPath_;
            if (history_.size() > 32)
                history_.removeFirst();
            historyIndex_ = history_.size() - 1;
        }
        const auto edit = pathEdit()->text();
        address_->blockSignals(true);
        address_->clear();
        auto recent = history_;
        recent.removeDuplicates();
        std::reverse(recent.begin(), recent.end());
        address_->addItems(recent);
        address_->setEditText(edit);
        address_->blockSignals(false);
        if (same) {
            selectNames(names);
            view_->verticalScrollBar()->setValue(scroll);
        }
    }
    pendingHistory_ = -1;
    message_->setText(result.error.empty()
                          ? (result.entries.empty() ? QStringLiteral("此目录为空") : QString())
                          : text(result.error));
    message_->setVisible(!message_->text().isEmpty());
    message_->setProperty("error", !result.error.empty());
    updateSummary();
}
void FilePane::setAvailable(bool enabled, bool loading)
{
    for (auto* w : navigation_)
        w->setEnabled(enabled);
    view_->setEnabled(enabled && !loading);
    filter_->setEnabled(enabled);
    if (loading) {
        message_->setText(QStringLiteral("正在读取目录…"));
        message_->show();
    }
}
void FilePane::clear(const QString& message)
{
    model_->replace({});
    loadedPath_.clear();
    history_.clear();
    historyIndex_ = -1;
    pendingHistory_ = -1;
    message_->setText(message);
    message_->show();
    updateSummary();
}
