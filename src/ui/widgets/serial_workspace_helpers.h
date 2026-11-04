#pragma once
#include "application/serial/at_command_library.h"
#include "application/serial/serial_workspace_state.h"
#include "core/serial/codec.h"
#include "ui/theme.h"
#include <QtCore/QStandardPaths>
#include <QtGui/QTextCharFormat>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QStyle>
#include <filesystem>
using testtools::LineEnding;
namespace testtools::ui::serial {

using testtools::ui::fromUtf8;
using testtools::ui::toUtf8;

inline QString channelName(int index)
{
    return index == 0 ? QStringLiteral("端点 A") : QStringLiteral("端点 B");
}

inline QString selectedEndpoint(const QComboBox* combo)
{
    // 已手动编辑的端点文本不得继承旧选项隐藏的绑定数据。
    const auto text = combo->currentText().trimmed();
    const auto data = combo->currentData().toString();
    return !data.isEmpty() && combo->currentText() == combo->itemText(combo->currentIndex()) ? data
                                                                                             : text;
}

inline LineEnding lineEndingFromIndex(int index)
{
    switch (index) {
    case 1:
        return LineEnding::Lf;
    case 2:
        return LineEnding::Cr;
    case 3:
        return LineEnding::CrLf;
    default:
        return LineEnding::None;
    }
}

inline testtools::AtCommandLineEnding storedLineEndingFromIndex(int index)
{
    switch (index) {
    case 1:
        return testtools::AtCommandLineEnding::Lf;
    case 2:
        return testtools::AtCommandLineEnding::Cr;
    case 3:
        return testtools::AtCommandLineEnding::CrLf;
    default:
        return testtools::AtCommandLineEnding::None;
    }
}

inline int storedLineEndingIndex(testtools::AtCommandLineEnding ending)
{
    switch (ending) {
    case testtools::AtCommandLineEnding::Lf:
        return 1;
    case testtools::AtCommandLineEnding::Cr:
        return 2;
    case testtools::AtCommandLineEnding::CrLf:
        return 3;
    default:
        return 0;
    }
}

inline void setVisualRole(QLabel* label, const QString& objectName)
{
    label->setObjectName(objectName);
    label->style()->unpolish(label);
    label->style()->polish(label);
}

inline QTextCharFormat logEntryFormat(testtools::SerialLogDirection direction)
{
    QTextCharFormat format;
    switch (direction) {
    case testtools::SerialLogDirection::Rx:
        format.setForeground(QColor(QStringLiteral("#286878")));
        break;
    case testtools::SerialLogDirection::Tx:
        format.setForeground(QColor(QStringLiteral("#965636")));
        break;
    case testtools::SerialLogDirection::Notice:
        format.setForeground(QColor(QStringLiteral("#706963")));
        break;
    }
    return format;
}

inline QString recordingSize(std::uint64_t bytes)
{
    if (bytes >= 1024 * 1024)
        return QStringLiteral("%1 MiB").arg(double(bytes) / (1024 * 1024), 0, 'f', 1);
    if (bytes >= 1024)
        return QStringLiteral("%1 KiB").arg(double(bytes) / 1024, 0, 'f', 1);
    return QStringLiteral("%1 B").arg(bytes);
}

inline std::filesystem::path storageDirectory()
{
    const auto configured = qEnvironmentVariable("TESTTOOLS_DATA_DIR");
    return std::filesystem::path((configured.isEmpty() ? QStandardPaths::writableLocation(
                                                             QStandardPaths::AppLocalDataLocation)
                                                       : configured)
                                     .toStdWString());
}

} // namespace testtools::ui::serial
