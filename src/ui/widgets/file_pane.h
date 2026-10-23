#pragma once

#include "core/device/file_transfer.h"
#include <QtCore/QAbstractTableModel>
#include <QtCore/QSortFilterProxyModel>
#include <QtGui/QIcon>
#include <QtWidgets/QFrame>
#include <functional>

class FileIconProvider;
class QComboBox;
class QLineEdit;
class QTreeView;
class QLabel;
class QPushButton;
class QMimeData;

// UI-only model: stable entry names, no file system or ADB calls in data().
class FileListModel final : public QAbstractTableModel {
  public:
    explicit FileListModel(QObject* parent = nullptr, bool remote = false);
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex&, int role) const override;
    QVariant headerData(int, Qt::Orientation, int role) const override;
    void replace(std::vector<testtools::FileEntry> entries, const QString& directory = {});
    const testtools::FileEntry& entry(int row) const;

  private:
    std::vector<testtools::FileEntry> entries_;
    QIcon icons_[8];
    FileIconProvider* iconProvider_;
    QString directory_;
    bool remote_;
};

class FilePane final : public QFrame {
  public:
    explicit FilePane(bool remote, QWidget* parent = nullptr);
    QLineEdit* pathEdit() const;
    QTreeView* view() const
    {
        return view_;
    }
    QPushButton* transferButton() const
    {
        return transfer_;
    }
    QLabel* targetLabel() const
    {
        return target_;
    }
    std::vector<testtools::FileEntry> selectedEntries() const;
    void selectNames(const QStringList& names);
    void applyListing(const testtools::FileListing& result, const QString& requested);
    void setAvailable(bool enabled, bool loading);
    void clear(const QString& message);
    void navigate(const QString& directory);
    QString loadedPath() const
    {
        return loadedPath_;
    }
    std::function<void()> openRequested, selectionChanged, transferRequested;
    std::function<QMimeData*()> dragData;
    std::function<bool(const QMimeData*)> dropData;

  private:
    void openEntry(const QModelIndex& index);
    void updateSummary();
    bool remote_;
    FileListModel* model_;
    QSortFilterProxyModel* proxy_;
    QComboBox* address_;
    QLineEdit* filter_;
    QTreeView* view_;
    QLabel *message_, *summary_, *target_;
    QPushButton* transfer_;
    QString loadedPath_;
    QStringList history_;
    int historyIndex_ = -1, pendingHistory_ = -1;
    std::vector<QWidget*> navigation_;
};
