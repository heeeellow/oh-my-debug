#include "at_command_dialog.h"
#include "ui/icons.h"
#include "ui/theme.h"
#include "ui/widgets/serial_connection_bar.h"
#include "ui/widgets/serial_keyword_highlighter.h"
#include "ui/widgets/serial_receive_view.h"
#include "ui/widgets/serial_shortcut_sidebar.h"
#include "ui/widgets/serial_workspace_helpers.h"

#include "application/serial/serial_transport_service.h"
#include "core/serial/codec.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QEvent>
#include <QtCore/QFile>
#include <QtCore/QMetaObject>
#include <QtCore/QPointer>
#include <QtCore/QSignalBlocker>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtGui/QActionGroup>
#include <QtGui/QClipboard>
#include <QtGui/QInputMethodEvent>
#include <QtGui/QKeyEvent>
#include <QtGui/QPainter>
#include <QtGui/QResizeEvent>
#include <QtGui/QShortcut>
#include <QtGui/QTextBlock>
#include <QtGui/QTextCharFormat>
#include <QtGui/QTextCursor>
#include <QtWidgets/QApplication>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QStyle>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <functional>
#include <utility>

using namespace testtools;

using namespace testtools::ui::serial;

void AtCommandDialog::open(QWidget* parent,
                           testtools::AtCommandLibrary& library,
                           int selected,
                           const std::function<void()>& changed)
{
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("atCommandDialog"));
    dialog.setWindowTitle(QStringLiteral("管理快捷指令"));
    dialog.resize(660, 420);
    auto* root = new QVBoxLayout(&dialog);
    root->setContentsMargins(16, 16, 16, 14);
    root->setSpacing(12);

    auto* body = new QHBoxLayout;
    body->setSpacing(14);
    auto* commandList = new QListWidget(&dialog);
    commandList->setMinimumWidth(190);
    body->addWidget(commandList, 1);

    auto* editor = new QWidget(&dialog);
    auto* editorLayout = new QVBoxLayout(editor);
    editorLayout->setContentsMargins(0, 0, 0, 0);
    editorLayout->setSpacing(10);
    auto* form = new QFormLayout;
    form->setHorizontalSpacing(12);
    auto* nameEdit = new QLineEdit(editor);
    nameEdit->setMaxLength(40);
    nameEdit->setPlaceholderText(QStringLiteral("例如：信号质量 / 握手帧"));
    auto* hexCheck =
        new QCheckBox(QStringLiteral("HEX 十六进制格式（按原始字节发送，不附加行尾）"), editor);
    auto* commandEdit = new QPlainTextEdit(editor);
    commandEdit->setFixedHeight(96);
    commandEdit->setPlaceholderText(QStringLiteral("例如：AT+CSQ 或 55 AA 01 0D 0A"));
    form->addRow(QStringLiteral("名称"), nameEdit);
    form->addRow(QStringLiteral("格式"), hexCheck);
    form->addRow(QStringLiteral("指令内容"), commandEdit);
    editorLayout->addLayout(form);
    auto* hint = new QLabel(
        QStringLiteral("文本指令发送时使用主页发送数据区的行尾设置；HEX 指令直接发送原始字节。"),
        editor);
    hint->setObjectName(QStringLiteral("fieldLabel"));
    hint->setWordWrap(true);
    editorLayout->addWidget(hint);
    editorLayout->addStretch();

    auto* actions = new QHBoxLayout;
    auto* newButton = new QPushButton(QStringLiteral("新建"), editor);
    auto* deleteButton = new QPushButton(QStringLiteral("删除"), editor);
    auto* saveButton = new QPushButton(QStringLiteral("保存指令"), editor);
    saveButton->setObjectName(QStringLiteral("primaryButton"));
    actions->addWidget(newButton);
    actions->addWidget(deleteButton);
    actions->addStretch();
    actions->addWidget(saveButton);
    editorLayout->addLayout(actions);
    body->addWidget(editor, 2);
    root->addLayout(body, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("完成"));
    root->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    std::function<void(int)> populateForm = [&](int row) {
        const auto& commands = library.commands();
        if (row < 0 || row >= static_cast<int>(commands.size())) {
            nameEdit->clear();
            hexCheck->setChecked(false);
            commandEdit->clear();
            deleteButton->setEnabled(false);
            return;
        }
        const auto& command = commands[static_cast<std::size_t>(row)];
        nameEdit->setText(fromUtf8(command.name));
        hexCheck->setChecked(command.hex);
        commandEdit->setPlainText(fromUtf8(command.command));
        deleteButton->setEnabled(true);
    };
    std::function<void(int)> refreshList = [&](int selectedRow) {
        const QSignalBlocker blocker(commandList);
        commandList->clear();
        for (const auto& command : library.commands()) {
            const QString itemLabel = command.hex
                                          ? QStringLiteral("[HEX] %1").arg(fromUtf8(command.name))
                                          : fromUtf8(command.name);
            commandList->addItem(itemLabel);
        }
        if (selectedRow >= 0 && selectedRow < commandList->count())
            commandList->setCurrentRow(selectedRow);
        else
            commandList->setCurrentRow(commandList->count() > 0 ? 0 : -1);
        populateForm(commandList->currentRow());
    };

    QObject::connect(
        commandList, &QListWidget::currentRowChanged, &dialog, [&](int row) { populateForm(row); });
    QObject::connect(newButton, &QPushButton::clicked, &dialog, [&] {
        commandList->setCurrentRow(-1);
        populateForm(-1);
        nameEdit->setFocus();
    });
    QObject::connect(saveButton, &QPushButton::clicked, &dialog, [&] {
        testtools::AtCommand command;
        command.name = toUtf8(nameEdit->text().trimmed());
        command.command = toUtf8(commandEdit->toPlainText().trimmed());
        command.hex = hexCheck->isChecked();
        std::string error;
        const int row = commandList->currentRow();
        const bool saved =
            row >= 0 ? library.update(static_cast<std::size_t>(row), std::move(command), error)
                     : library.add(std::move(command), error);
        if (!saved) {
            QMessageBox::information(&dialog, QStringLiteral("无法保存指令"), fromUtf8(error));
            return;
        }
        const int selected = row >= 0 ? row : static_cast<int>(library.commands().size()) - 1;
        changed();
        refreshList(selected);
    });
    QObject::connect(deleteButton, &QPushButton::clicked, &dialog, [&] {
        const int row = commandList->currentRow();
        if (row < 0)
            return;
        if (QMessageBox::question(
                &dialog,
                QStringLiteral("删除快捷指令"),
                QStringLiteral("确定删除“%1”吗？").arg(commandList->currentItem()->text())) !=
            QMessageBox::Yes) {
            return;
        }
        library.remove(static_cast<std::size_t>(row));
        changed();
        refreshList(std::min(row, commandList->count() - 1));
    });

    refreshList(selected);
    dialog.exec();
}
