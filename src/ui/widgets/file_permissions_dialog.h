#pragma once
#include "core/device/file_transfer.h"
#include <QtWidgets/QDialog>
#include <array>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QGroupBox;

// 只编辑权限意图；实际读取与写入由工作区持有的后台任务执行。
class FilePermissionsDialog final : public QDialog {
  public:
    FilePermissionsDialog(const QString& device, const QString& directory,
                          const std::vector<testtools::FileEntry>& entries, QWidget* parent);
    void setEntries(std::vector<testtools::FileEntry> entries);
    void setReadError(const QString& error);
    const std::vector<testtools::FileEntry>& entries() const { return entries_; }
    std::string permissions() const;
  private:
    void fromNumber();
    void fromChecks();
    std::vector<testtools::FileEntry> entries_;
    std::array<QCheckBox*, 12> bits_{};
    QLineEdit* number_ = nullptr;
    QLabel* current_ = nullptr;
    QGroupBox* editor_ = nullptr;
    QPushButton* apply_ = nullptr;
    bool updating_ = false, ready_ = false;
};
