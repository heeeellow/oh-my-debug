#include "ui/widgets/serial_shortcut_sidebar.h"
#include "ui/theme.h"

#include <QtGui/QPainter>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMenu>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>
#include <algorithm>

namespace {
// 长名称和原始指令保留在提示中，绘制时按实际剩余宽度省略。
class PreviewLabel final : public QLabel {
  public:
    PreviewLabel(const QString& value, QWidget* parent) : QLabel(value, parent)
    {
        setTextFormat(Qt::PlainText);
        setToolTip(value);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }
  protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setPen(palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled,
                                      QPalette::WindowText));
        painter.drawText(contentsRect(), Qt::AlignLeft | Qt::AlignVCenter,
                         fontMetrics().elidedText(text(), Qt::ElideRight, contentsRect().width()));
    }
};
}

SerialShortcutSidebar::SerialShortcutSidebar(QWidget* parent) : QFrame(parent)
{
    setObjectName(QStringLiteral("shortcutSidebar"));
    setMinimumWidth(220);
    setMaximumWidth(480);
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 8, 10, 8);
    root->setSpacing(6);
    auto* header = new QHBoxLayout;
    auto* title = new QLabel(QStringLiteral("快捷指令"), this);
    title->setObjectName(QStringLiteral("sectionLabel"));
    header->addWidget(title);
    header->addStretch();
    auto* manage = new QToolButton(this);
    manage->setText(QStringLiteral("指令库"));
    manage->setPopupMode(QToolButton::InstantPopup);
    manage->setMinimumHeight(32);
    auto* menu = new QMenu(manage);
    menu->addAction(QStringLiteral("管理指令…"), this, [this] { if (manageRequested) manageRequested(); });
    menu->addAction(QStringLiteral("导入 JSON…"), this, [this] { if (importRequested) importRequested(); });
    menu->addAction(QStringLiteral("导出 JSON…"), this, [this] { if (exportRequested) exportRequested(); });
    manage->setMenu(menu);
    header->addWidget(manage);
    auto* collapse = new QToolButton(this);
    collapse->setObjectName(QStringLiteral("collapseShortcuts"));
    collapse->setIcon(testtools::ui::navigationChevron(false));
    collapse->setFixedSize(32, 32);
    collapse->setToolTip(QStringLiteral("收起快捷指令"));
    collapse->setAccessibleName(QStringLiteral("收起快捷指令"));
    connect(collapse, &QToolButton::clicked, this, [this] { if (collapseRequested) collapseRequested(); });
    header->addWidget(collapse);
    root->addLayout(header);
    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("shortcutList"));
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setSpacing(2);
    root->addWidget(list_, 1);
}

void SerialShortcutSidebar::setCommands(const std::vector<testtools::AtCommand>& commands)
{
    const int previous = currentIndex();
    const int scroll = list_->verticalScrollBar()->value();
    fillButtons_.clear();
    sendButtons_.clear();
    list_->clear();
    for (int index = 0; index < static_cast<int>(commands.size()); ++index) {
        const auto& command = commands[static_cast<std::size_t>(index)];
        auto* item = new QListWidgetItem(list_);
        auto* row = new QWidget(list_);
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(6, 8, 6, 8);
        layout->setSpacing(4);
        auto* labels = new QVBoxLayout;
        labels->setSpacing(3);
        auto* name = new PreviewLabel(testtools::ui::fromUtf8(command.name), row);
        name->setObjectName(QStringLiteral("shortcutName"));
        const auto raw = testtools::ui::fromUtf8(command.command);
        auto* preview = new PreviewLabel((command.hex ? QStringLiteral("[HEX] ") : QString()) + raw.simplified(), row);
        preview->setToolTip(raw);
        preview->setObjectName(QStringLiteral("shortcutPreview"));
        labels->addWidget(name);
        labels->addWidget(preview);
        layout->addLayout(labels, 1);
        auto button = [&](const QString& text, const QString& id) {
            auto* value = new QPushButton(text, row);
            value->setObjectName(id + QString::number(index));
            value->setProperty("buttonRole", QStringLiteral("tool"));
            value->setFixedSize(44, 32);
            value->setCursor(Qt::PointingHandCursor);
            value->setAccessibleName(testtools::ui::fromUtf8(command.name) + QStringLiteral(" · ") + text);
            layout->addWidget(value);
            return value;
        };
        auto* fill = button(QStringLiteral("填入"), QStringLiteral("shortcutFill-"));
        auto* send = button(QStringLiteral("发送"), QStringLiteral("shortcutSend-"));
        fill->setToolTip(QStringLiteral("填入发送框，使用当前共享行尾"));
        send->setToolTip(QStringLiteral("立即向当前串口发送此指令"));
        connect(fill, &QPushButton::clicked, this, [this, index] {
            list_->setCurrentRow(index);
            if (fillRequested) fillRequested(index);
        });
        connect(send, &QPushButton::clicked, this, [this, index] {
            list_->setCurrentRow(index);
            if (sendRequested) sendRequested(index);
        });
        fillButtons_.push_back(fill);
        sendButtons_.push_back(send);
        item->setSizeHint(QSize(180, 64));
        list_->setItemWidget(item, row);
    }
    if (!commands.empty()) list_->setCurrentRow(std::clamp(previous, 0, static_cast<int>(commands.size()) - 1));
    else {
        auto* empty = new QListWidgetItem(QStringLiteral("暂无指令，可从指令库添加"), list_);
        empty->setFlags(Qt::NoItemFlags);
    }
    list_->verticalScrollBar()->setValue(scroll);
}

void SerialShortcutSidebar::setActionsEnabled(bool fill, bool send)
{
    for (auto* button : fillButtons_) button->setEnabled(fill);
    for (auto* button : sendButtons_) button->setEnabled(send);
}

int SerialShortcutSidebar::currentIndex() const
{
    return fillButtons_.empty() ? -1 : list_->currentRow();
}
