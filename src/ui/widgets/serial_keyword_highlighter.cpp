#include "ui/widgets/serial_keyword_highlighter.h"
#include <QtGui/QTextCharFormat>

SerialKeywordHighlighter::SerialKeywordHighlighter(QTextDocument* document)
    : QSyntaxHighlighter(document),
      errors_(QStringLiteral("\\b(?:ERROR|FAIL|FAILED|panic|Exception|Segfault)\\b"),
              QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption),
      warnings_(QStringLiteral("\\b(?:WARN|WARNING|timeout)\\b"),
                QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption) {}

void SerialKeywordHighlighter::setEnabled(bool enabled)
{
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    rehighlight();
}

void SerialKeywordHighlighter::highlightBlock(const QString& text)
{
    if (!enabled_) return;
    const auto mark = [this, &text](const QRegularExpression& pattern, const char* foreground, const char* background) {
        QTextCharFormat format;
        format.setForeground(QColor(QLatin1String(foreground)));
        format.setBackground(QColor(QLatin1String(background)));
        auto matches = pattern.globalMatch(text);
        while (matches.hasNext()) {
            const auto match = matches.next();
            setFormat(int(match.capturedStart()), int(match.capturedLength()), format);
        }
    };
    mark(warnings_, "#87652F", "#F6EFE2");
    mark(errors_, "#A34A42", "#F7E9E8");
}
