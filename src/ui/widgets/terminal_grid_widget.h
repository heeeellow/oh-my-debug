#pragma once
#include "application/terminal/terminal_engine.h"
#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtGui/QFont>
#include <QtGui/QGlyphRun>
#include <QtGui/QPixmap>
#include <QtWidgets/QAbstractScrollArea>
#include <array>
#include <functional>
#include <unordered_map>

class QInputMethodEvent;
class QTimer;
class QPainter;

// A fixed-cell viewport. No QTextDocument or VT parser runs in this widget.
class TerminalGridWidget final : public QAbstractScrollArea {
    Q_OBJECT
  public:
    explicit TerminalGridWidget(QWidget* parent = nullptr);
    void setFrame(std::shared_ptr<const testtools::TerminalFrame> frame);
    std::shared_ptr<const testtools::TerminalFrame> frame() const
    {
        return frame_;
    }
    QString toPlainText() const;
    QString selectedText() const;
    bool hasSelection() const;
    void clearSelection();
    void copy(bool clear = false);
    void selectRange(
        int firstLine, int firstColumn, int lastLine, int lastColumn, bool rectangular = false);
    void followOutput();
    bool following() const
    {
        return following_;
    }
    void setAppearance(int theme, const QString& family, int pixels, int spacing, int cursorShape);
    void setKeywordHighlight(bool enabled);
    void setTimestampVisible(bool enabled);
    int setSearch(const QString& query, bool advance = false, int direction = 1);
    int currentMatch() const
    {
        return searchCurrent_;
    }
    QRect terminalCursorRect() const;
    int gridRows() const;
    int gridColumns() const;
    std::uint64_t paintCount() const
    {
        return paintCount_;
    }
    qint64 maximumPaintMicroseconds() const
    {
        return maximumPaintUs_;
    }
    bool copyOnSelect = false;
    bool rightPaste = false;
    std::function<void(int, int)> gridResized;
    std::function<void(QKeyEvent*)> keyInput;
    std::function<void(const QString&)> textInput;
    std::function<void()> pasteRequested;
    std::function<void(bool)> followChanged;
    std::function<void(int, int, int, bool, Qt::KeyboardModifiers)> mouseInput;

  protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

  private:
    struct Position {
        std::uint64_t id = 0;
        int column = 0;
    };
    struct Match {
        int row = 0, column = 0, endRow = 0, endColumn = 0;
    };
    struct SearchLine {
        testtools::TerminalLinePtr source;
        QString text;
        std::vector<int> columns;
    };
    struct PaintedLine {
        testtools::TerminalLinePtr source;
        QPixmap image;
    };
    struct GlyphLayout {
        QList<QGlyphRun> runs;
        qreal ascent = 0;
    };
    int lineCount() const;
    testtools::TerminalLinePtr lineAt(int row) const;
    int rowOf(std::uint64_t id) const;
    Position positionAt(const QPointF& point) const;
    bool selected(int row, int column) const;
    bool searchSelected(int row, int column) const;
    void updateMetrics();
    void updateScrollRange();
    void rebuildSearch();
    void showContextMenu(const QPoint& point);
    void selectWord(const Position& pos);
    bool reportMouse(QMouseEvent* event, bool pressed, bool motion = false);
    QString cellText(const testtools::TerminalGridCell& cell) const;
    void
    paintGlyph(QPainter& painter, const QRectF& rect, const QString& text,
               std::uint32_t character, int style);
    void paintCell(QPainter& painter,
                   const QRectF& rect,
                   const testtools::TerminalGridCell& cell,
                   int keyword,
                   bool chosen = false,
                   bool search = false);
    const QPixmap& paintedLine(const testtools::TerminalLinePtr& line);
    std::shared_ptr<const testtools::TerminalFrame> frame_;
    std::unordered_map<std::uint64_t, int> rowIndices_;
    Position anchor_, extent_;
    bool rectangular_ = false, selecting_ = false, following_ = true;
    bool highlight_ = false, timestamps_ = false, cursorOn_ = true;
    bool metricsUpdating_ = false;
    int theme_ = 0, spacing_ = 3, cursorShape_ = 0;
    qreal cellWidth_ = 9, cellHeight_ = 20, ascent_ = 15;
    int margin_ = 12, gutterWidth_ = 0;
    static constexpr int verticalPadding_ = 6;
    QFont timestampFont_;
    QColor background_, foreground_;
    QTimer* blinkTimer_ = nullptr;
    QString preedit_, searchQuery_;
    std::vector<Match> matches_;
    std::unordered_map<std::uint64_t, SearchLine> searchLines_;
    std::unordered_map<std::uint64_t, PaintedLine> paintedLines_;
    std::array<QFont, 4> cellFonts_;
    std::array<QHash<QString, GlyphLayout>, 4> glyphLayouts_;
    qreal glyphDpr_ = 0;
    int searchCurrent_ = -1;
    QElapsedTimer clickClock_;
    QPoint lastClick_;
    std::uint64_t paintCount_ = 0;
    qint64 maximumPaintUs_ = 0;
};
