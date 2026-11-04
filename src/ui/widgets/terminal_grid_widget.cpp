#include "terminal_grid_widget.h"
#include <QtCore/QDateTime>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimer>
#include <QtGui/QClipboard>
#include <QtGui/QFontDatabase>
#include <QtGui/QInputMethod>
#include <QtGui/QInputMethodEvent>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPaintEvent>
#include <QtGui/QPainter>
#include <QtGui/QResizeEvent>
#include <QtGui/QTextLayout>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMenu>
#include <QtWidgets/QScrollBar>
#include <algorithm>
#include <cmath>

TerminalGridWidget::TerminalGridWidget(QWidget* parent) : QAbstractScrollArea(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    viewport()->setMouseTracking(true);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    setMinimumHeight(100);
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        following_ = value == verticalScrollBar()->maximum();
        if (followChanged)
            followChanged(following_);
        viewport()->update();
    });
    blinkTimer_ = new QTimer(this);
    blinkTimer_->setInterval(500);
    connect(blinkTimer_, &QTimer::timeout, this, [this] {
        cursorOn_ = !cursorOn_;
        viewport()->update(terminalCursorRect().adjusted(-2, -2, 2, 2));
    });
    setAppearance(0, QStringLiteral("Cascadia Mono"), 15, 3, 0);
}
int TerminalGridWidget::lineCount() const
{
    if (!frame_)
        return 0;
    return int(frame_->screen.size() + (frame_->alternateScreen ? 0 : frame_->history->size()));
}
testtools::TerminalLinePtr TerminalGridWidget::lineAt(int row) const
{
    if (!frame_ || row < 0)
        return {};
    const int history = frame_->alternateScreen ? 0 : int(frame_->history->size());
    if (row < history)
        return (*frame_->history)[row];
    row -= history;
    return row < int(frame_->screen.size()) ? frame_->screen[row] : nullptr;
}
int TerminalGridWidget::rowOf(std::uint64_t id) const
{
    if (!id)
        return -1;
    const auto found = rowIndices_.find(id);
    return found == rowIndices_.end() ? -1 : found->second;
}
void TerminalGridWidget::setFrame(std::shared_ptr<const testtools::TerminalFrame> frame)
{
    if (!frame || (frame_ && frame_->revision == frame->revision))
        return;
    const auto old = frame_;
    const int top = verticalScrollBar()->value();
    const auto oldTop = lineAt(top);
    const auto oldCursor = terminalCursorRect();
    const bool structural = !old || old->columns != frame->columns || old->rows != frame->rows ||
                            old->alternateScreen != frame->alternateScreen;
    const bool keepBottom = following_;
    frame_ = std::move(frame);
    // Cursor-only frames retain the history index instead of hashing all scrollback again.
    const bool historyChanged = !old || old->history != frame_->history ||
                                old->alternateScreen != frame_->alternateScreen;
    if (historyChanged) {
        rowIndices_.clear();
        for (int row = 0; row < lineCount(); ++row)
            rowIndices_[lineAt(row)->id] = row;
    } else {
        for (const auto& line : old->screen)
            rowIndices_.erase(line->id);
        const int history = frame_->alternateScreen ? 0 : int(frame_->history->size());
        for (int row = 0; row < int(frame_->screen.size()); ++row)
            rowIndices_[frame_->screen[row]->id] = history + row;
    }
    if (structural)
        clearSelection();
    if (anchor_.id && (rowOf(anchor_.id) < 0 || rowOf(extent_.id) < 0))
        clearSelection();
    updateScrollRange();
    if (keepBottom)
        followOutput();
    else if (oldTop) {
        const int preserved = rowOf(oldTop->id);
        if (preserved >= 0)
            verticalScrollBar()->setValue(preserved);
    }
    if (structural || top != verticalScrollBar()->value() || !old)
        viewport()->update();
    else {
        const int oldHistory = old->alternateScreen ? 0 : int(old->history->size());
        QRegion dirty;
        for (int y = 0; y <= gridRows(); ++y) {
            const int row = verticalScrollBar()->value() + y;
            testtools::TerminalLinePtr oldLine;
            if (row < oldHistory)
                oldLine = (*old->history)[row];
            else if (row - oldHistory >= 0 && row - oldHistory < int(old->screen.size()))
                oldLine = old->screen[row - oldHistory];
            if (oldLine != lineAt(row))
                dirty += QRect(0,
                               int(verticalPadding_ + y * cellHeight_),
                               viewport()->width(),
                               int(std::ceil(cellHeight_)));
        }
        dirty += oldCursor.adjusted(-2, -2, 2, 2);
        dirty += terminalCursorRect().adjusted(-2, -2, 2, 2);
        viewport()->update(dirty);
    }
    cursorOn_ = true;
    if (hasFocus() && frame_->cursorVisible && frame_->cursorBlink)
        blinkTimer_->start();
    else
        blinkTimer_->stop();
    QGuiApplication::inputMethod()->update(Qt::ImCursorRectangle);
}
QString TerminalGridWidget::cellText(const testtools::TerminalGridCell& cell) const
{
    if (cell.width == 0)
        return {};
    QString result;
    for (auto c : cell.characters) {
        if (!c || c == UINT32_MAX)
            break;
        const char32_t ch = char32_t(c);
        result += QString::fromUcs4(&ch, 1);
    }
    return result.isEmpty() ? QStringLiteral(" ") : result;
}
QString TerminalGridWidget::toPlainText() const
{
    QString text;
    for (int row = 0; row < lineCount(); ++row) {
        const auto line = lineAt(row);
        QString value;
        for (const auto& cell : line->cells)
            value += cellText(cell);
        if (row + 1 == lineCount() || !lineAt(row + 1)->continuation)
            while (value.endsWith(' '))
                value.chop(1);
        if (row && !line->continuation)
            text += '\n';
        text += value;
    }
    while (text.endsWith('\n'))
        text.chop(1);
    return text;
}
bool TerminalGridWidget::hasSelection() const
{
    return anchor_.id && extent_.id &&
           (anchor_.id != extent_.id || anchor_.column != extent_.column);
}
void TerminalGridWidget::clearSelection()
{
    const bool changed = hasSelection();
    anchor_ = {};
    extent_ = {};
    selecting_ = false;
    if (changed)
        viewport()->update();
}
void TerminalGridWidget::selectRange(
    int firstLine, int firstColumn, int lastLine, int lastColumn, bool rectangular)
{
    auto a = lineAt(firstLine), b = lineAt(lastLine);
    if (!a || !b)
        return;
    anchor_ = {a->id, std::clamp(firstColumn, 0, int(a->cells.size()))};
    extent_ = {b->id, std::clamp(lastColumn, 0, int(b->cells.size()))};
    rectangular_ = rectangular;
    following_ = false;
    if (followChanged)
        followChanged(false);
    viewport()->update();
}
QString TerminalGridWidget::selectedText() const
{
    if (!hasSelection())
        return {};
    int ar = rowOf(anchor_.id), br = rowOf(extent_.id);
    int ac = anchor_.column, bc = extent_.column;
    if (ar < 0 || br < 0)
        return {};
    if (std::pair{ar, ac} > std::pair{br, bc}) {
        std::swap(ar, br);
        std::swap(ac, bc);
    }
    const int left = std::min(ac, bc), right = std::max(ac, bc);
    QString text;
    for (int row = ar; row <= br; ++row) {
        const auto line = lineAt(row);
        const int start = rectangular_ ? left : (row == ar ? ac : 0);
        const int end = rectangular_ ? right : (row == br ? bc : int(line->cells.size()));
        QString value;
        for (int col = start; col < std::min(end, int(line->cells.size())); ++col)
            value += cellText(line->cells[col]);
        if (!rectangular_ && (row < br || end == int(line->cells.size())) &&
            (row + 1 == lineCount() || !lineAt(row + 1)->continuation))
            while (value.endsWith(' '))
                value.chop(1);
        if (row > ar && (rectangular_ || !line->continuation))
            text += '\n';
        text += value;
    }
    return text;
}
void TerminalGridWidget::copy(bool clear)
{
    const auto text = selectedText();
    if (!text.isEmpty())
        QApplication::clipboard()->setText(text);
    if (clear)
        clearSelection();
}
bool TerminalGridWidget::selected(int row, int col) const
{
    if (!hasSelection())
        return false;
    int ar = rowOf(anchor_.id), br = rowOf(extent_.id);
    int ac = anchor_.column, bc = extent_.column;
    if (ar < 0 || br < 0)
        return false;
    if (std::pair{ar, ac} > std::pair{br, bc}) {
        std::swap(ar, br);
        std::swap(ac, bc);
    }
    if (row < ar || row > br)
        return false;
    if (rectangular_)
        return col >= std::min(ac, bc) && col < std::max(ac, bc);
    return (row > ar || col >= ac) && (row < br || col < bc);
}
void TerminalGridWidget::followOutput()
{
    following_ = true;
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
}
void TerminalGridWidget::setAppearance(
    int theme, const QString& family, int pixels, int spacing, int shape)
{
    paintedLines_.clear();
    theme_ = std::clamp(theme, 0, 2);
    spacing_ = std::clamp(spacing, 0, 6);
    cursorShape_ = shape;
    background_ =
        theme_ == 1 ? QColor("#faf7f0") : (theme_ == 2 ? QColor("#000000") : QColor("#1e222a"));
    foreground_ =
        theme_ == 1 ? QColor("#34322e") : (theme_ == 2 ? QColor("#ffffff") : QColor("#d5dbe5"));
    QFont font(family);
    if (!QFontDatabase::families().contains(family))
        font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    font.setWeight(QFont::Normal);
    font.setKerning(false);
    font.setStyleStrategy(QFont::PreferAntialias);
    font.setPixelSize(std::clamp(pixels, 11, 18));
    setFont(font);
    QString familyName = font.family();
    familyName.replace('"', QStringLiteral("\\\""));
    setStyleSheet(QStringLiteral("TerminalGridWidget { font-family: \"%1\"; font-size: %2px; }")
                      .arg(familyName)
                      .arg(font.pixelSize()));
    ensurePolished();
    updateMetrics();
    viewport()->update();
}
void TerminalGridWidget::updateMetrics()
{
    if (metricsUpdating_)
        return;
    metricsUpdating_ = true;
    const auto previousFont = cellFonts_[0];
    const qreal previousWidth = cellWidth_, previousHeight = cellHeight_;
    const QFontMetricsF metrics(font());
    cellWidth_ = std::ceil(metrics.horizontalAdvance('M'));
    cellHeight_ = std::ceil(metrics.height()) + spacing_;
    ascent_ = metrics.ascent() + spacing_ / 2.0;
    for (int style = 0; style < int(cellFonts_.size()); ++style) {
        cellFonts_[style] = font();
        cellFonts_[style].setBold(style & 1);
        cellFonts_[style].setItalic(style & 2);
    }
    if (previousFont != cellFonts_[0] || previousWidth != cellWidth_ ||
        previousHeight != cellHeight_) {
        paintedLines_.clear();
        for (auto& cache : glyphLayouts_)
            cache.clear();
    }
    timestampFont_ = font();
    timestampFont_.setPixelSize(std::clamp(font().pixelSize() - 2, 11, 14));
    timestampFont_.setBold(false);
    timestampFont_.setItalic(false);
    const QFontMetricsF timeMetrics(timestampFont_);
    gutterWidth_ =
        timestamps_
            ? int(std::ceil(timeMetrics.horizontalAdvance(QStringLiteral("00:00:00.000")))) + 16
            : 0;
    margin_ = gutterWidth_ + 12;
    updateScrollRange();
    if (gridResized)
        gridResized(gridRows(), gridColumns());
    metricsUpdating_ = false;
}
int TerminalGridWidget::gridRows() const
{
    return std::clamp(int((viewport()->height() - verticalPadding_ * 2) / cellHeight_), 2, 256);
}
int TerminalGridWidget::gridColumns() const
{
    return std::clamp(int((viewport()->width() - margin_ - 12) / cellWidth_), 8, 512);
}
void TerminalGridWidget::updateScrollRange()
{
    const bool follow = following_;
    verticalScrollBar()->setPageStep(gridRows());
    verticalScrollBar()->setRange(0, std::max(0, lineCount() - gridRows()));
    following_ = follow;
}
void TerminalGridWidget::setKeywordHighlight(bool value)
{
    if (highlight_ == value)
        return;
    highlight_ = value;
    paintedLines_.clear();
    viewport()->update();
}
void TerminalGridWidget::setTimestampVisible(bool value)
{
    if (timestamps_ == value)
        return;
    timestamps_ = value;
    updateMetrics();
    viewport()->update();
}
QRect TerminalGridWidget::terminalCursorRect() const
{
    if (!frame_)
        return QRect(margin_, verticalPadding_, int(cellWidth_), int(cellHeight_));
    const int history = frame_->alternateScreen ? 0 : int(frame_->history->size());
    return QRect(int(margin_ + frame_->cursorColumn * cellWidth_),
                 int(verticalPadding_ +
                     (history + frame_->cursorRow - verticalScrollBar()->value()) * cellHeight_),
                 int(cellWidth_),
                 int(cellHeight_));
}
void TerminalGridWidget::paintGlyph(QPainter& painter,
                                    const QRectF& rect,
                                    const QString& text,
                                   std::uint32_t c,
                                   int style)
{
    if (c >= 0x2500 && c <= 0x257f) {
        // The generated connection table covers every Unicode box-drawing codepoint.
        static constexpr unsigned char weights[128] = {
            5,   10,  80,  160, 5,   10,  80,  160, 5,   10,  80,  160, 68,  72,  132, 136,
            65,  66,  129, 130, 20,  24,  36,  40,  17,  18,  33,  34,  84,  88,  100, 148,
            164, 104, 152, 168, 81,  82,  97,  145, 161, 98,  146, 162, 69,  70,  73,  74,
            133, 134, 137, 138, 21,  22,  25,  26,  37,  38,  41,  42,  85,  86,  89,  90,
            101, 149, 165, 102, 105, 150, 153, 106, 154, 166, 169, 170, 5,   10,  80,  160,
            15,  240, 76,  196, 204, 67,  193, 195, 28,  52,  60,  19,  49,  51,  92,  244,
            252, 83,  241, 243, 79,  197, 207, 31,  53,  63,  95,  245, 255, 68,  65,  17,
            20,  5,   5,   0,   1,   16,  4,   64,  2,   32,  8,   128, 9,   144, 6,   96};
        const auto arms = weights[c - 0x2500];
        const qreal x = rect.center().x(), y = rect.center().y();
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, false);
        if (c >= 0x2571 && c <= 0x2573) {
            if (c != 0x2572)
                painter.drawLine(rect.bottomLeft(), rect.topRight());
            if (c != 0x2571)
                painter.drawLine(rect.topLeft(), rect.bottomRight());
        }
        for (int arm = 0; arm < 4; ++arm) {
            const int weight = (arms >> (arm * 2)) & 3;
            if (!weight)
                continue;
            auto pen = painter.pen();
            pen.setWidthF(weight == 2 ? 2.0 : 1.0);
            painter.setPen(pen);
            const int count = weight == 3 ? 2 : 1;
            for (int stroke = 0; stroke < count; ++stroke) {
                const qreal offset = count == 2 ? (stroke ? 1.5 : -1.5) : 0;
                if (arm == 0)
                    painter.drawLine(QPointF(rect.left(), y + offset), QPointF(x, y + offset));
                if (arm == 1)
                    painter.drawLine(QPointF(x, y + offset), QPointF(rect.right(), y + offset));
                if (arm == 2)
                    painter.drawLine(QPointF(x + offset, rect.top()), QPointF(x + offset, y));
                if (arm == 3)
                    painter.drawLine(QPointF(x + offset, y), QPointF(x + offset, rect.bottom()));
            }
        }
        painter.restore();
    } else if (c >= 0x2580 && c <= 0x259f) {
        if (c >= 0x2596) {
            static constexpr int quadrants[] = {4, 8, 1, 13, 9, 7, 11, 2, 6, 14};
            const int mask = quadrants[c - 0x2596];
            for (int q = 0; q < 4; ++q)
                if (mask & (1 << q))
                    painter.fillRect(QRectF(rect.left() + (q % 2) * rect.width() / 2,
                                            rect.top() + (q / 2) * rect.height() / 2,
                                            rect.width() / 2,
                                            rect.height() / 2),
                                     painter.pen().color());
            return;
        }
        QRectF block = rect;
        if (c == 0x2580)
            block.setHeight(rect.height() / 2);
        else if (c >= 0x2581 && c <= 0x2587)
            block.setTop(rect.bottom() - rect.height() * (c - 0x2580) / 8);
        else if (c >= 0x2589 && c <= 0x258f)
            block.setWidth(rect.width() * (0x2590 - c) / 8);
        else if (c == 0x2590)
            block.setLeft(rect.center().x());
        else if (c == 0x2594)
            block.setHeight(rect.height() / 8);
        else if (c == 0x2595)
            block.setLeft(rect.right() - rect.width() / 8);
        QColor ink = painter.pen().color();
        if (c >= 0x2591 && c <= 0x2593)
            ink.setAlpha(64 * (c - 0x2590));
        painter.fillRect(block, ink);
    } else {
        const auto dpr = viewport()->devicePixelRatioF();
        if (glyphDpr_ != dpr) {
            for (auto& cache : glyphLayouts_)
                cache.clear();
            glyphDpr_ = dpr;
        }
        auto& cache = glyphLayouts_[style];
        auto found = cache.constFind(text);
        if (found == cache.cend()) {
            if (cache.size() >= 512)
                cache.clear();
            QTextLayout layout(text, cellFonts_[style], painter.device());
            QTextOption options;
            options.setWrapMode(QTextOption::NoWrap);
            layout.setTextOption(options);
            layout.beginLayout();
            auto line = layout.createLine();
            if (line.isValid())
                line.setLineWidth(1024);
            layout.endLayout();
            cache.insert(text, {layout.glyphRuns(), line.isValid() ? line.ascent() : 0});
            found = cache.constFind(text);
        }
        const QPointF origin(rect.left(), rect.top() + ascent_ - found->ascent);
        for (const auto& run : found->runs)
            painter.drawGlyphRun(origin, run);
    }
}
void TerminalGridWidget::paintCell(QPainter& painter,
                                   const QRectF& rect,
                                   const testtools::TerminalGridCell& cell,
                                   int keyword,
                                   bool chosen,
                                   bool search)
{
    const auto resolve = [this](const testtools::TerminalColor& color, bool fg) {
        if (color.defaultColor)
            return fg ? foreground_ : background_;
        static const QColor palette[] = {QColor("#0c0c0c"), QColor("#c50f1f"),
                                         QColor("#13a10e"), QColor("#c19c00"),
                                         QColor("#3b78d6"), QColor("#b4009e"),
                                         QColor("#3a96dd"), QColor("#cccccc"),
                                         QColor("#767676"), QColor("#e74856"),
                                         QColor("#16c60c"), QColor("#f9f1a5"),
                                         QColor("#81a9ff"), QColor("#e47bdf"),
                                         QColor("#61d6d6"), QColor("#f2f2f2")};
        if (color.paletteIndex >= 0 && color.paletteIndex < 16)
            return palette[color.paletteIndex];
        return QColor(color.red, color.green, color.blue);
    };
    QColor fg = resolve(cell.foreground, true), bg = resolve(cell.background, false);
    if (cell.reverse)
        std::swap(fg, bg);
    if (keyword) {
        fg = QColor(theme_ == 1 ? (keyword == 2 ? "#a34a42" : "#87652f")
                                : (keyword == 2 ? "#f0b3aa" : "#dac496"));
        bg = QColor(theme_ == 1 ? (keyword == 2 ? "#f7e9e8" : "#f6efe2")
                                : (keyword == 2 ? "#412c31" : "#3a352b"));
    }
    if (chosen) {
        bg = theme_ == 1 ? QColor("#c9dbea") : QColor("#425b78");
        fg = theme_ == 1 ? QColor("#172c3f") : QColor("#ffffff");
    } else if (search) {
        bg = QColor("#e6c66b");
        fg = QColor("#252321");
    }
    // The row and viewport already have the default background. Blank cells need
    // neither text shaping nor a font change, including on an entirely empty screen.
    if (bg != background_ || chosen || search)
        painter.fillRect(rect, bg);
    painter.setPen(fg);
    const auto& cellFont = cellFonts_[(cell.bold ? 1 : 0) | (cell.italic ? 2 : 0)];
    if (painter.font() != cellFont)
        painter.setFont(cellFont);
    if (!cell.conceal && cell.characters[0] && cell.characters[0] != UINT32_MAX &&
        (cell.characters[0] != ' ' || cell.characters[1]))
        paintGlyph(painter, rect, cellText(cell), cell.characters[0],
                   (cell.bold ? 1 : 0) | (cell.italic ? 2 : 0));
    if (cell.underline)
        painter.drawLine(QPointF(rect.left(), rect.bottom() - 2),
                         QPointF(rect.right(), rect.bottom() - 2));
    if (cell.strike)
        painter.drawLine(QPointF(rect.left(), rect.center().y()),
                         QPointF(rect.right(), rect.center().y()));
}
const QPixmap& TerminalGridWidget::paintedLine(const testtools::TerminalLinePtr& line)
{
    auto& cached = paintedLines_[line->id];
    const int columns = std::min(int(line->cells.size()), gridColumns());
    const qreal dpr = viewport()->devicePixelRatioF();
    const QSize pixels(std::max(1, int(std::ceil(columns * cellWidth_ * dpr))),
                       std::max(1, int(std::ceil(cellHeight_ * dpr))));
    if (cached.source == line && cached.image.size() == pixels &&
        cached.image.devicePixelRatioF() == dpr)
        return cached.image;
    cached.image = QPixmap(pixels);
    cached.image.setDevicePixelRatio(dpr);
    cached.image.fill(background_);
    QPainter painter(&cached.image);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setFont(cellFonts_[0]);
    std::vector<int> marks(line->cells.size(), 0);
    if (highlight_) {
        static const QRegularExpression keywords(
            QStringLiteral("\\b(ERROR|FAIL|FAILED|panic|Exception|Segfault|WARN|WARNING|timeout)\\b"),
            QRegularExpression::CaseInsensitiveOption);
        QString value;
        std::vector<int> offsets;
        for (int col = 0; col < int(line->cells.size()); ++col) {
            const auto text = cellText(line->cells[col]);
            for (int i = 0; i < text.size(); ++i)
                offsets.push_back(col);
            value += text;
        }
        auto matches = keywords.globalMatch(value);
        while (matches.hasNext()) {
            const auto match = matches.next();
            const int kind = match.captured().startsWith("warn", Qt::CaseInsensitive) ||
                                     match.captured().compare("timeout", Qt::CaseInsensitive) == 0
                                 ? 1 : 2;
            for (int i = int(match.capturedStart());
                 i < int(match.capturedEnd()) && i < int(offsets.size()); ++i)
                marks[offsets[i]] = kind;
        }
    }
    for (int col = 0; col < columns; ++col) {
        const auto& cell = line->cells[col];
        if (cell.width == 0)
            continue;
        paintCell(painter,
                  QRectF(col * cellWidth_, 0, cellWidth_ * std::max(1, cell.width), cellHeight_),
                  cell, marks[col]);
    }
    cached.source = line;
    return cached.image;
}
void TerminalGridWidget::paintEvent(QPaintEvent* event)
{
    QElapsedTimer elapsed;
    elapsed.start();
    QPainter painter(viewport());
    painter.setClipRegion(event->region());
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.fillRect(event->rect(), background_);
    if (timestamps_) {
        const QColor gutter =
            theme_ == 1 ? QColor("#f2eee6") : (theme_ == 2 ? QColor("#080808") : QColor("#1a1e25"));
        painter.fillRect(QRect(0, 0, gutterWidth_, viewport()->height()), gutter);
        painter.setPen(theme_ == 1 ? QColor("#e0dbd2") : QColor("#303640"));
        painter.drawLine(QPointF(gutterWidth_ - .5, 0),
                         QPointF(gutterWidth_ - .5, viewport()->height()));
    }
    const int top = verticalScrollBar()->value();
    // Retain only visible rasterized rows, never the complete history.
    for (auto it = paintedLines_.begin(); it != paintedLines_.end();) {
        const int row = rowOf(it->first);
        if (row < top || row >= top + gridRows())
            it = paintedLines_.erase(it);
        else
            ++it;
    }
    const bool selection = hasSelection();
    for (int row = top + std::max(0, int((event->rect().top() - verticalPadding_) / cellHeight_));
         row < std::min({lineCount(),
                         top + gridRows(),
                         top + int((event->rect().bottom() - verticalPadding_) / cellHeight_) + 1});
         ++row) {
        const auto line = lineAt(row);
        const qreal y = verticalPadding_ + (row - top) * cellHeight_;
        if (timestamps_ && line->timestampMs > 0) {
            painter.setFont(timestampFont_);
            painter.setPen(theme_ == 1 ? QColor("#8b8377") : QColor("#7b8493"));
            painter.drawText(
                QRectF(4, y, gutterWidth_ - 12, cellHeight_),
                Qt::AlignRight | Qt::AlignVCenter,
                QDateTime::fromMSecsSinceEpoch(line->timestampMs).toString("HH:mm:ss.zzz"));
        }
        painter.drawPixmap(QPointF(margin_, y), paintedLine(line));
        // Selection and search stay live overlays; cursor blinking and typing reuse
        // the underlying row pixels without repeating glyph layout or keyword matching.
        if (selection || searchCurrent_ >= 0) {
            for (int col = 0; col < int(line->cells.size()) && col < gridColumns(); ++col) {
                const auto& cell = line->cells[col];
                if (cell.width == 0)
                    continue;
                const bool chosen = selection && selected(row, col);
                const bool search = searchSelected(row, col);
                if (chosen || search)
                    paintCell(painter,
                              QRectF(margin_ + col * cellWidth_, y,
                                     cellWidth_ * std::max(1, cell.width), cellHeight_),
                              cell, 0, chosen, search);
            }
        }
    }
    painter.setFont(font());
    if (frame_ && frame_->cursorVisible && (cursorOn_ || !hasFocus() || !frame_->cursorBlink)) {
        const auto rect = terminalCursorRect();
        painter.setPen(foreground_);
        const int shape = cursorShape_ ? cursorShape_ : frame_->cursorShape;
        if (!hasFocus())
            painter.drawRect(rect.adjusted(0, 0, -1, -1));
        else if (shape == 2)
            painter.fillRect(QRect(rect.left(), rect.bottom() - 2, rect.width(), 2), foreground_);
        else if (shape == 3)
            painter.fillRect(QRect(rect.left(), rect.top(), 2, rect.height()), foreground_);
        else {
            QColor ink = foreground_;
            ink.setAlpha(120);
            painter.fillRect(rect, ink);
        }
    }
    if (!preedit_.isEmpty()) {
        auto rect = terminalCursorRect();
        rect.setWidth(
            std::max(rect.width(), int(QFontMetricsF(font()).horizontalAdvance(preedit_))));
        painter.fillRect(rect, background_);
        painter.setPen(foreground_);
        painter.drawText(QPointF(rect.left(), rect.top() + ascent_), preedit_);
        painter.drawLine(rect.bottomLeft(), rect.bottomRight());
    }
    ++paintCount_;
    maximumPaintUs_ = std::max(maximumPaintUs_, elapsed.nsecsElapsed() / 1000);
}
void TerminalGridWidget::resizeEvent(QResizeEvent* event)
{
    QAbstractScrollArea::resizeEvent(event);
    updateMetrics();
    if (following_)
        followOutput();
}
bool TerminalGridWidget::event(QEvent* event)
{
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange ||
        event->type() == QEvent::Show || event->type() == QEvent::DevicePixelRatioChange) {
        const bool result = QAbstractScrollArea::event(event);
        updateMetrics();
        return result;
    }
    if (event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) {
            keyPressEvent(key);
            return true;
        }
    }
    if (event->type() == QEvent::ShortcutOverride) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (!(key->key() == Qt::Key_F &&
              key->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier)) &&
            key->key() != Qt::Key_F3) {
            event->accept();
            return true;
        }
    }
    return QAbstractScrollArea::event(event);
}
void TerminalGridWidget::keyPressEvent(QKeyEvent* event)
{
    cursorOn_ = true;
    if (event->modifiers().testFlag(Qt::ShiftModifier) &&
        (event->key() == Qt::Key_PageUp || event->key() == Qt::Key_PageDown)) {
        verticalScrollBar()->setValue(verticalScrollBar()->value() +
                                      (event->key() == Qt::Key_PageUp ? -1 : 1) * gridRows());
        event->accept();
        return;
    }
    if (keyInput)
        keyInput(event);
    event->accept();
}
void TerminalGridWidget::inputMethodEvent(QInputMethodEvent* event)
{
    preedit_ = event->preeditString();
    if (!event->commitString().isEmpty() && textInput)
        textInput(event->commitString());
    viewport()->update(terminalCursorRect().adjusted(0, 0, int(cellWidth_ * 32), 0));
    event->accept();
}
QVariant TerminalGridWidget::inputMethodQuery(Qt::InputMethodQuery query) const
{
    if (query == Qt::ImEnabled)
        return true;
    if (query == Qt::ImCursorRectangle) {
        auto rect = terminalCursorRect();
        rect.translate(viewport()->pos());
        return rect;
    }
    if (query == Qt::ImFont)
        return font();
    if (query == Qt::ImSurroundingText)
        return QString{};
    if (query == Qt::ImCursorPosition || query == Qt::ImAnchorPosition)
        return 0;
    return QAbstractScrollArea::inputMethodQuery(query);
}
TerminalGridWidget::Position TerminalGridWidget::positionAt(const QPointF& p) const
{
    if (!lineCount())
        return {};
    const int row =
        std::clamp(int((p.y() - verticalPadding_) / cellHeight_) + verticalScrollBar()->value(),
                   0,
                   lineCount() - 1);
    const auto line = lineAt(row);
    int col = std::clamp(int((p.x() - margin_) / cellWidth_), 0, int(line->cells.size()));
    if (col < int(line->cells.size()) && col > 0 && line->cells[col].width == 0)
        --col;
    return {line->id, col};
}
bool TerminalGridWidget::reportMouse(QMouseEvent* event, bool pressed, bool motion)
{
    if (!frame_ || !frame_->mouseMode || event->modifiers().testFlag(Qt::ShiftModifier) ||
        !mouseInput)
        return false;
    const auto pos = positionAt(event->position());
    const int row = rowOf(pos.id) - (frame_->alternateScreen ? 0 : int(frame_->history->size()));
    if (row < 0)
        return false;
    const int button = motion ? 0
                              : (event->button() == Qt::LeftButton     ? 1
                                 : event->button() == Qt::MiddleButton ? 2
                                                                       : 3);
    mouseInput(row, pos.column, button, pressed, event->modifiers());
    return true;
}
void TerminalGridWidget::mousePressEvent(QMouseEvent* event)
{
    setFocus();
    if (reportMouse(event, true))
        return;
    if (event->button() == Qt::RightButton) {
        if (rightPaste && !hasSelection()) {
            if (pasteRequested)
                pasteRequested();
        } else
            showContextMenu(event->position().toPoint());
        return;
    }
    if (event->button() != Qt::LeftButton)
        return;
    following_ = false;
    if (followChanged)
        followChanged(false);
    const auto pos = positionAt(event->position());
    if (clickClock_.isValid() && clickClock_.elapsed() < QApplication::doubleClickInterval() &&
        (event->position().toPoint() - lastClick_).manhattanLength() < 5) {
        const int row = rowOf(pos.id);
        const auto line = lineAt(row);
        if (line)
            selectRange(row, 0, row, int(line->cells.size()));
        clickClock_.invalidate();
    } else {
        anchor_ = extent_ = pos;
        rectangular_ = event->modifiers().testFlag(Qt::AltModifier);
        selecting_ = true;
    }
    viewport()->update();
}
void TerminalGridWidget::selectWord(const Position& pos)
{
    const int row = rowOf(pos.id);
    const auto line = lineAt(row);
    if (!line || pos.column >= int(line->cells.size()))
        return;
    const auto word = [this, &line](int col) {
        const auto value = cellText(line->cells[col]);
        return !value.isEmpty() && (value[0].isLetterOrNumber() || value[0] == '_' ||
                                    value[0] == '-' || value[0] == '/' || value[0] == '.');
    };
    int left = pos.column, right = left + 1;
    const bool category = word(left);
    while (left > 0 && word(left - 1) == category)
        --left;
    while (right < int(line->cells.size()) && word(right) == category)
        ++right;
    selectRange(row, left, row, right);
}
void TerminalGridWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (reportMouse(event, true))
        return;
    selectWord(positionAt(event->position()));
    clickClock_.start();
    lastClick_ = event->position().toPoint();
    if (copyOnSelect)
        copy();
}
void TerminalGridWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (reportMouse(event, true, true))
        return;
    if (!selecting_)
        return;
    if (event->position().y() < 0)
        verticalScrollBar()->setValue(verticalScrollBar()->value() - 1);
    else if (event->position().y() > viewport()->height())
        verticalScrollBar()->setValue(verticalScrollBar()->value() + 1);
    extent_ = positionAt(event->position());
    viewport()->update();
}
void TerminalGridWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (reportMouse(event, false))
        return;
    if (selecting_) {
        extent_ = positionAt(event->position());
        selecting_ = false;
    }
    if (copyOnSelect)
        copy();
    viewport()->update();
}
void TerminalGridWidget::wheelEvent(QWheelEvent* event)
{
    if (frame_ && frame_->mouseMode && !event->modifiers().testFlag(Qt::ShiftModifier) &&
        mouseInput) {
        const auto pos = positionAt(event->position());
        const int row =
            rowOf(pos.id) - (frame_->alternateScreen ? 0 : int(frame_->history->size()));
        const int button = event->angleDelta().y() > 0 ? 4 : 5;
        mouseInput(std::max(0, row), pos.column, button, true, event->modifiers());
        mouseInput(std::max(0, row), pos.column, button, false, event->modifiers());
        event->accept();
        return;
    }
    QAbstractScrollArea::wheelEvent(event);
}
void TerminalGridWidget::showContextMenu(const QPoint& point)
{
    QMenu menu(this);
    auto* copyAction =
        menu.addAction(QStringLiteral("复制\tCtrl+Shift+C"), this, [this] { copy(); });
    copyAction->setEnabled(hasSelection());
    menu.addAction(QStringLiteral("粘贴\tCtrl+Shift+V"), this, [this] {
        if (pasteRequested)
            pasteRequested();
    });
    menu.addSeparator();
    menu.addAction(QStringLiteral("回到最新输出"), this, [this] { followOutput(); });
    menu.exec(viewport()->mapToGlobal(point));
}
void TerminalGridWidget::focusInEvent(QFocusEvent* event)
{
    QAbstractScrollArea::focusInEvent(event);
    cursorOn_ = true;
    if (frame_ && frame_->cursorVisible && frame_->cursorBlink)
        blinkTimer_->start();
    viewport()->update(terminalCursorRect());
}
void TerminalGridWidget::focusOutEvent(QFocusEvent* event)
{
    QAbstractScrollArea::focusOutEvent(event);
    blinkTimer_->stop();
    cursorOn_ = true;
    viewport()->update(terminalCursorRect());
}
void TerminalGridWidget::rebuildSearch()
{
    matches_.clear();
    searchCurrent_ = -1;
    for (auto it = searchLines_.begin(); it != searchLines_.end();)
        if (rowOf(it->first) < 0)
            it = searchLines_.erase(it);
        else
            ++it;
    if (searchQuery_.isEmpty())
        return;
    const auto cached = [this](int row) -> const SearchLine& {
        const auto line = lineAt(row);
        auto& cache = searchLines_[line->id];
        if (cache.source != line) {
            cache.source = line;
            cache.text.clear();
            cache.columns.clear();
            for (int col = 0; col < int(line->cells.size()); ++col) {
                const auto text = cellText(line->cells[col]);
                cache.text += text;
                for (int i = 0; i < text.size(); ++i)
                    cache.columns.push_back(col);
            }
        }
        return cache;
    };
    for (int row = 0; row < lineCount() && matches_.size() < 10000;) {
        int end = row + 1;
        while (end < lineCount() && lineAt(end)->continuation)
            ++end;
        if (end == row + 1) {
            const auto& line = cached(row);
            qsizetype at = 0;
            while ((at = line.text.indexOf(searchQuery_, at)) >= 0 && matches_.size() < 10000) {
                const int first = line.columns[at],
                          last = line.columns[at + searchQuery_.size() - 1];
                matches_.push_back(
                    {row, first, row, last + std::max(1, line.source->cells[last].width)});
                at += searchQuery_.size();
            }
        } else {
            QString logical;
            std::vector<std::pair<int, int>> positions;
            for (int r = row; r < end; ++r) {
                const auto& line = cached(r);
                logical += line.text;
                for (const auto col : line.columns)
                    positions.emplace_back(r, col);
            }
            qsizetype at = 0;
            while ((at = logical.indexOf(searchQuery_, at)) >= 0 && matches_.size() < 10000) {
                const auto first = positions[at], last = positions[at + searchQuery_.size() - 1];
                matches_.push_back(
                    {first.first,
                     first.second,
                     last.first,
                     last.second + std::max(1, lineAt(last.first)->cells[last.second].width)});
                at += searchQuery_.size();
            }
        }
        row = end;
    }
}
int TerminalGridWidget::setSearch(const QString& query, bool advance, int direction)
{
    const int previous = searchCurrent_;
    searchQuery_ = query;
    rebuildSearch();
    if (!matches_.empty()) {
        const int count = int(matches_.size());
        searchCurrent_ = advance ? (previous < 0 ? (direction > 0 ? 0 : count - 1)
                                                 : (previous + direction + count) % count)
                                 : std::min(previous, count - 1);
        if (advance) {
            following_ = false;
            verticalScrollBar()->setValue(matches_[searchCurrent_].row);
        }
    }
    viewport()->update();
    return int(matches_.size());
}
bool TerminalGridWidget::searchSelected(int row, int col) const
{
    if (searchCurrent_ < 0 || searchCurrent_ >= int(matches_.size()))
        return false;
    const auto& match = matches_[searchCurrent_];
    return std::pair{row, col} >= std::pair{match.row, match.column} &&
           std::pair{row, col} < std::pair{match.endRow, match.endColumn};
}
