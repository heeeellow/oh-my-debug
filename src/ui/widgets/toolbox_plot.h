#pragma once
#include <QtWidgets/QWidget>
#include <QtCore/QPointF>
#include <array>
#include <vector>
class QCheckBox;
class ToolboxPlot final : public QWidget {
  public:
    explicit ToolboxPlot(QWidget* parent = nullptr);
    void setSamples(int direction, const std::vector<QPointF>& samples);
    void clear(int seconds = 10);
    int sampleCount(int direction) const { return direction >= 0 && direction < 2 ? static_cast<int>(points_[direction].size()) : 0; }
  protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;
  private:
    std::array<std::vector<QPointF>, 2> points_;
    int seconds_ = 10;
    QCheckBox* zoom_ = nullptr;
};
