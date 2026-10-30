#include "ui/widgets/file_permissions_dialog.h"
#include "application/device/file_transfer_service.h"
#include "ui/theme.h"
#include <QtCore/QSignalBlocker>
#include <QtCore/QRegularExpression>
#include <QtCore/QStringList>
#include <QtGui/QRegularExpressionValidator>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>
#include <algorithm>

namespace {
constexpr std::array<unsigned, 12> masks{0400,0200,0100,0040,0020,0010,0004,0002,0001,04000,02000,01000};
QString modeText(unsigned mode) { return QStringLiteral("%1").arg(mode, 4, 8, QLatin1Char('0')); }
}

FilePermissionsDialog::FilePermissionsDialog(const QString& device, const QString& directory,
    const std::vector<testtools::FileEntry>& entries, QWidget* parent) : QDialog(parent)
{
    setObjectName("filePermissionsDialog");
    setWindowTitle(QStringLiteral("修改权限"));
    setMinimumWidth(440);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 18); layout->setSpacing(12);
    auto* target = new QLabel(this);
    target->setTextFormat(Qt::PlainText);
    target->setText(QStringLiteral("设备：%1\n目录：%2\n所选：%3")
        .arg(fontMetrics().elidedText(device, Qt::ElideMiddle, 410),
             fontMetrics().elidedText(directory, Qt::ElideMiddle, 410),
             entries.size() == 1 ? fontMetrics().elidedText(testtools::ui::fromUtf8(entries[0].name), Qt::ElideMiddle, 410)
                                 : QStringLiteral("%1 个项目").arg(entries.size())));
    QStringList names; for (const auto& e : entries) names.append(testtools::ui::fromUtf8(e.name));
    target->setToolTip(device + "\n" + directory + "\n" + names.join('\n'));
    layout->addWidget(target);
    current_ = new QLabel(QStringLiteral("正在读取当前权限…"), this);
    current_->setObjectName("filePermissionCurrent"); current_->setTextFormat(Qt::PlainText);
    current_->setWordWrap(true); layout->addWidget(current_);

    editor_ = new QGroupBox(QStringLiteral("新权限"), this);
    auto* grid = new QGridLayout(editor_); grid->setHorizontalSpacing(24); grid->setVerticalSpacing(10);
    const QStringList columns{QStringLiteral("读"), QStringLiteral("写"), QStringLiteral("执行")};
    const QStringList rows{QStringLiteral("所有者"), QStringLiteral("用户组"), QStringLiteral("其他人")};
    for (int c = 0; c < 3; ++c) grid->addWidget(new QLabel(columns[c], this), 0, c + 1);
    for (int r = 0; r < 3; ++r) {
        grid->addWidget(new QLabel(rows[r], this), r + 1, 0);
        for (int c = 0; c < 3; ++c) {
            auto* bit = new QCheckBox(this); bits_[r * 3 + c] = bit;
            bit->setObjectName(QStringLiteral("permissionBit%1").arg(r * 3 + c));
            bit->setAccessibleName(rows[r] + columns[c]); grid->addWidget(bit, r + 1, c + 1);
        }
    }
    auto* special = new QHBoxLayout;
    const QStringList specialNames{QStringLiteral("Set UID"), QStringLiteral("Set GID"), QStringLiteral("Sticky")};
    for (int i = 0; i < 3; ++i) {
        bits_[9 + i] = new QCheckBox(specialNames[i], this);
        bits_[9 + i]->setObjectName(QStringLiteral("permissionBit%1").arg(9 + i));
        special->addWidget(bits_[9 + i]);
    }
    auto* specialToggle = new QPushButton(QStringLiteral("特殊权限位…"), this);
    specialToggle->setFlat(true); specialToggle->setCheckable(true); specialToggle->setAutoDefault(false);
    grid->addWidget(specialToggle, 4, 0, 1, 4);
    auto* specialRow = new QWidget(this); specialRow->setLayout(special); specialRow->hide();
    grid->addWidget(specialRow, 5, 0, 1, 4);
    connect(specialToggle, &QPushButton::toggled, specialRow, &QWidget::setVisible);

    auto* numberLabel = new QLabel(QStringLiteral("八进制权限"), this);
    number_ = new QLineEdit(this); number_->setObjectName("filePermissionMode");
    number_->setMaximumWidth(125); number_->setMaxLength(4);
    number_->setPlaceholderText(QStringLiteral("如 644"));
    number_->setValidator(new QRegularExpressionValidator(QRegularExpression("[0-7]{3,4}"), number_));
    numberLabel->setBuddy(number_);
    grid->addWidget(numberLabel, 6, 0); grid->addWidget(number_, 6, 1, 1, 3);
    auto* presets = new QHBoxLayout;
    presets->addWidget(new QLabel(QStringLiteral("常用值"), this));
    for (const auto& mode : {QStringLiteral("644"), QStringLiteral("600"), QStringLiteral("755"), QStringLiteral("750")}) {
        auto* button = new QPushButton(mode, this); button->setAutoDefault(false);
        presets->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, mode] { number_->setText(mode); });
    }
    grid->addLayout(presets, 7, 0, 1, 4);
    layout->addWidget(editor_); editor_->setEnabled(false);
    auto* hint = new QLabel(QStringLiteral("仅修改所选文件或文件夹自身，不递归修改内容。\n多选时，应用会将所选项目统一设为指定权限。"), this);
    hint->setWordWrap(true); hint->setObjectName("fieldHint"); layout->addWidget(hint);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this);
    apply_ = buttons->button(QDialogButtonBox::Apply); apply_->setText(QStringLiteral("应用"));
    apply_->setObjectName("applyFilePermissions"); apply_->setEnabled(false); apply_->setDefault(true);
    apply_->setProperty("buttonRole", "primary");
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    connect(apply_, &QPushButton::clicked, this, [this] { if (ready_ && !permissions().empty()) accept(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject); layout->addWidget(buttons);
    connect(number_, &QLineEdit::textChanged, this, [this] { fromNumber(); });
    for (auto* bit : bits_) connect(bit, &QCheckBox::checkStateChanged, this, [this] { fromChecks(); });
}

std::string FilePermissionsDialog::permissions() const
{
    if (!ready_) return {};
    return testtools::FileTransferService::normalizePermissions(testtools::ui::toUtf8(number_->text()));
}

void FilePermissionsDialog::setEntries(std::vector<testtools::FileEntry> entries)
{
    if (entries.empty() || std::any_of(entries.begin(), entries.end(), [](const auto& e) {
        return testtools::FileTransferService::normalizePermissions(e.permissions).empty();
    })) { setReadError(QStringLiteral("无法读取所选项目的权限，请刷新后重试")); return; }
    entries_ = std::move(entries); ready_ = true; editor_->setEnabled(true); updating_ = true;
    const auto first = unsigned(std::stoul(entries_.front().permissions, nullptr, 8));
    const bool same = std::all_of(entries_.begin(), entries_.end(), [first](const auto& e) { return std::stoul(e.permissions, nullptr, 8) == first; });
    current_->setText(same ? QStringLiteral("当前权限：%1").arg(modeText(first)) : QStringLiteral("当前权限不一致；请指定统一的新权限。"));
    QStringList details;
    for (const auto& entry : entries_) details.append(testtools::ui::fromUtf8(entry.name + "  " + entry.permissions));
    current_->setToolTip(details.join('\n'));
    current_->setAccessibleName(current_->text());
    for (std::size_t i = 0; i < bits_.size(); ++i) {
        const bool on = (first & masks[i]) != 0;
        const bool mixed = std::any_of(entries_.begin(), entries_.end(), [i, on](const auto& e) { return ((std::stoul(e.permissions, nullptr, 8) & masks[i]) != 0) != on; });
        bits_[i]->setTristate(mixed); bits_[i]->setCheckState(mixed ? Qt::PartiallyChecked : on ? Qt::Checked : Qt::Unchecked);
    }
    if (same) number_->setText(modeText(first));
    else { number_->clear(); number_->setPlaceholderText(QStringLiteral("指定新权限")); }
    updating_ = false; apply_->setEnabled(same);
    number_->setFocus(); number_->selectAll();
}

void FilePermissionsDialog::setReadError(const QString& error)
{
    ready_ = false; current_->setText(error); editor_->setEnabled(false); apply_->setEnabled(false);
}

void FilePermissionsDialog::fromNumber()
{
    if (updating_ || !ready_) return;
    const auto value = permissions(); apply_->setEnabled(!value.empty());
    if (value.empty()) return;
    const auto mode = std::stoul(value, nullptr, 8); updating_ = true;
    for (std::size_t i = 0; i < bits_.size(); ++i) {
        bits_[i]->setTristate(false); bits_[i]->setChecked((mode & masks[i]) != 0);
    }
    updating_ = false;
}

void FilePermissionsDialog::fromChecks()
{
    if (updating_ || !ready_) return;
    unsigned mode = 0; bool mixed = false;
    for (std::size_t i = 0; i < bits_.size(); ++i) {
        mixed |= bits_[i]->checkState() == Qt::PartiallyChecked;
        if (bits_[i]->checkState() == Qt::Checked) mode |= masks[i];
    }
    updating_ = true; number_->setText(mixed ? QString() : modeText(mode)); updating_ = false;
    apply_->setEnabled(!mixed);
}
