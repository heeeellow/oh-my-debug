#pragma once
#include <QtCore/QTimer>
#include <QtGui/QPainter>
#include <QtGui/QTextCursor>
#include <QtWidgets/QApplication>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QScrollBar>
#include <algorithm>
#include <functional>

// 串口接收与交互终端视图控件（支持终端模式独立闪烁光标与滚轮跟随状态通知）。
class ReceiveConsole final : public QPlainTextEdit {
  public:
    explicit ReceiveConsole(QWidget* parent) : QPlainTextEdit(parent), cursorBlink_(this)
    {
        cursorBlink_.setInterval(std::max(250, QApplication::cursorFlashTime() / 2));
        connect(&cursorBlink_, &QTimer::timeout, this, [this] {
            if (property("terminalInput").toBool()) {
                cursorOn_ = !cursorOn_;
                viewport()->update();
            }
        });
    }
    void setTerminalCursor(const QTextCursor& cursor)
    {
        terminalCursor_ = cursor;
        cursorOn_ = true;
        viewport()->update();
    }
    QRect terminalCursorRect() const
    {
        return terminalCursor_.isNull() ? cursorRect() : cursorRect(terminalCursor_);
    }
    std::function<void(bool)> scrollFollowChanged;

  protected:
    void paintEvent(QPaintEvent* event) override
    {
        QPlainTextEdit::paintEvent(event);
        if (property("terminalInput").toBool() && hasFocus() && cursorOn_) {
            QPainter painter(viewport());
            auto caret = terminalCursorRect();
            caret.setWidth(2);
            painter.fillRect(caret, QColor(QStringLiteral("#706963")));
        }
    }
    void focusInEvent(QFocusEvent* event) override
    {
        QPlainTextEdit::focusInEvent(event);
        cursorOn_ = true;
        if (QApplication::cursorFlashTime() > 0)
            cursorBlink_.start();
        viewport()->update();
    }
    void focusOutEvent(QFocusEvent* event) override
    {
        cursorBlink_.stop();
        QPlainTextEdit::focusOutEvent(event);
        viewport()->update();
    }
    void wheelEvent(QWheelEvent* event) override
    {
        QPlainTextEdit::wheelEvent(event);
        if (property("terminalInput").toBool() && scrollFollowChanged)
            scrollFollowChanged(verticalScrollBar()->value() >= verticalScrollBar()->maximum());
    }

  private:
    QTextCursor terminalCursor_;
    QTimer cursorBlink_;
    bool cursorOn_ = true;
};
