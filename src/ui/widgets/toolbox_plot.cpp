#include "toolbox_plot.h"
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QMouseEvent>
#include <QtCore/QStringList>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QToolTip>
#include <algorithm>
#include <cmath>

namespace {
const QColor colors[]{QColor("#ad694e"), QColor("#4d746e")};
QString directionName(int direction) { return direction ? QStringLiteral("板卡 → 电脑") : QStringLiteral("电脑 → 板卡"); }
QString statistics(const std::vector<QPointF>& points)
{
    if (points.empty()) return QStringLiteral("等待采样");
    double low = points.front().y(), high = low, sum = 0;
    for (const auto& point : points) { low = std::min(low, point.y()); high = std::max(high, point.y()); sum += point.y(); }
    return QStringLiteral("平均 %1 · 最小 %2 · 最大 %3 Mbps").arg(sum / points.size(), 0, 'f', 1).arg(low, 0, 'f', 1).arg(high, 0, 'f', 1);
}
QRectF plotArea(const QWidget* widget) { return QRectF(widget->rect()).adjusted(66, 48, -20, -48); }
}
ToolboxPlot::ToolboxPlot(QWidget* parent) : QWidget(parent)
{
    setMinimumHeight(170); setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding); setMouseTracking(true);
    setObjectName("networkThroughputPlot");
    zoom_ = new QCheckBox(QStringLiteral("放大波动"), this); zoom_->setObjectName("networkPlotZoom");
    zoom_->setToolTip(QStringLiteral("纵轴按实际采样范围缩放；取消后从 0 开始显示。"));
    connect(zoom_, &QCheckBox::toggled, this, [this] { update(); });
}
void ToolboxPlot::clear(int seconds) { for (auto& series : points_) series.clear(); seconds_ = std::max(1, seconds); update(); }
void ToolboxPlot::setSamples(int direction, const std::vector<QPointF>& samples)
{
    if (direction < 0 || direction > 1) return;
    std::vector<QPointF> valid; valid.reserve(std::min<std::size_t>(samples.size(), 512));
    for (const auto& sample : samples) {
        if (valid.size() >= 512) break;
        if (!std::isfinite(sample.x()) || !std::isfinite(sample.y()) || sample.x() < 0 || sample.y() < 0 ||
            (!valid.empty() && sample.x() <= valid.back().x())) continue;
        valid.push_back(sample);
    }
    if (valid != points_[direction]) { points_[direction] = std::move(valid); update(); }
}
void ToolboxPlot::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event); zoom_->adjustSize(); zoom_->move(std::max(0, width() - zoom_->width() - 8), 0);
}
void ToolboxPlot::paintEvent(QPaintEvent*)
{
    QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
    const auto area = plotArea(this); if (area.width() < 20 || area.height() < 20) return;
    painter.setPen(QColor("#77716c")); painter.drawText(QRectF(4, 0, 150, 20), QStringLiteral("吞吐（Mbps）"));
    double low = 0, high = 1, smallest = 1e100, seconds = seconds_;
    bool hasData = false;
    for (int d = 0; d < 2; ++d) for (const auto& point : points_[d]) {
        hasData = true; high = std::max(high, point.y()); smallest = std::min(smallest, point.y()); seconds = std::max(seconds, point.x());
    }
    if (zoom_->isChecked() && hasData) {
        const double padding = std::max(1.0, (high - smallest) * 0.15); low = std::max(0.0, smallest - padding); high += padding;
    } else {
        high *= 1.1;
        const double step = std::pow(10.0, std::floor(std::log10(high)) - 1);
        high = std::ceil(high / step) * step;
    }
    for (int tick = 0; tick <= 4; ++tick) {
        const double fraction = tick / 4.0, y = area.bottom() - fraction * area.height();
        painter.setPen(QPen(QColor("#e3dfdb"), 1, Qt::DashLine)); painter.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
        painter.setPen(QColor("#77716c"));
        const int decimals = high - low < 4 ? 2 : (zoom_->isChecked() || high - low < 40 ? 1 : 0);
        painter.drawText(QRectF(0, y - 9, 58, 20), Qt::AlignRight | Qt::AlignVCenter, QString::number(low + fraction * (high - low), 'f', decimals));
        const double x = area.left() + fraction * area.width();
        painter.drawText(QRectF(x - 30, area.bottom() + 5, 60, 18), Qt::AlignCenter, QString::number(fraction * seconds, 'f', 1));
    }
    painter.drawText(QRectF(area.right() - 30, area.bottom() + 23, 30, 16), Qt::AlignRight, QStringLiteral("秒"));
    if (!hasData) {
        painter.drawText(area, Qt::AlignCenter, QStringLiteral("开始测速后显示实际采样；可悬停查看每个采样点。")); return;
    }
    for (int d = 0; d < 2; ++d) {
        if (points_[d].empty()) continue;
        const double legendX = 170 + d * std::max(210, (width() - 330) / 2);
        painter.setPen(QPen(colors[d], 2)); painter.drawLine(QPointF(legendX, 10), QPointF(legendX + 18, 10));
        painter.drawText(QRectF(legendX + 25, 0, 290, 20), directionName(d) + QStringLiteral("  %1 Mbps").arg(points_[d].back().y(), 0, 'f', 1));
        painter.drawText(QRectF(area.left() + d * area.width() / 2, 24, area.width() / 2, 20), statistics(points_[d]));
        QPainterPath path;
        painter.save(); painter.setClipRect(area.adjusted(-3, -3, 3, 3));
        for (std::size_t index = 0; index < points_[d].size(); ++index) {
            const auto& sample = points_[d][index];
            const QPointF position(area.left() + sample.x() / seconds * area.width(), area.bottom() - (sample.y() - low) / (high - low) * area.height());
            if (!index) path.moveTo(position); else path.lineTo(position);
            painter.drawEllipse(position, 2.5, 2.5);
        }
        painter.drawPath(path); painter.restore();
    }
}
void ToolboxPlot::mouseMoveEvent(QMouseEvent* event)
{
    const auto area = plotArea(this);
    if (!area.contains(event->position())) { QToolTip::hideText(); return; }
    double seconds = seconds_; for (const auto& series : points_) if (!series.empty()) seconds = std::max(seconds, series.back().x());
    const double time = (event->position().x() - area.left()) / area.width() * seconds;
    QStringList lines;
    for (int d = 0; d < 2; ++d) if (!points_[d].empty()) {
        const auto nearest = std::min_element(points_[d].begin(), points_[d].end(), [time](const auto& left, const auto& right) { return std::abs(left.x() - time) < std::abs(right.x() - time); });
        lines << QStringLiteral("%1 · %2 秒 · %3 Mbps").arg(directionName(d)).arg(nearest->x(), 0, 'f', 2).arg(nearest->y(), 0, 'f', 2);
    }
    QToolTip::showText(event->globalPosition().toPoint(), lines.join('\n'), this);
}
void ToolboxPlot::leaveEvent(QEvent* event) { QToolTip::hideText(); QWidget::leaveEvent(event); }
