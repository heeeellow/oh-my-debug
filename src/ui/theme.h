#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtGui/QIcon>

#include <string>

namespace testtools::ui {

// 全局界面样式表入口。
QString applicationStyleSheet();
QIcon navigationChevron(bool expanded, bool selected = false);

// UTF-8 标准字符串与 Qt 字符串桥接转换工具（仅供 src/ui/ 层使用）。
inline QString fromUtf8(const std::string& text)
{
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

inline std::string toUtf8(const QString& text)
{
    const QByteArray bytes = text.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

} // namespace testtools::ui
