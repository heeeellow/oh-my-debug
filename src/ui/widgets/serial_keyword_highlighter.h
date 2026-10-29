#pragma once
#include <QtGui/QSyntaxHighlighter>
#include <QtCore/QRegularExpression>

// 仅叠加文档布局格式；保留底层 ANSI 样式，搜索选区由接收视图单独覆盖在上层。
class SerialKeywordHighlighter final : public QSyntaxHighlighter {
  public:
    explicit SerialKeywordHighlighter(QTextDocument* document);
    void setEnabled(bool enabled);
  protected:
    void highlightBlock(const QString& text) override;
  private:
    bool enabled_ = false;
    QRegularExpression errors_, warnings_;
};
