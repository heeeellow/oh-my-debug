#pragma once

#include "application/serial/at_command_library.h"
#include <QtWidgets/QFrame>
#include <functional>
#include <vector>

class QListWidget;
class QPushButton;

// 快捷指令只提交选择、填入和发送意图，实际字节与连接门禁由工作区处理。
class SerialShortcutSidebar final : public QFrame {
  public:
    explicit SerialShortcutSidebar(QWidget* parent = nullptr);
    void setCommands(const std::vector<testtools::AtCommand>& commands);
    void setActionsEnabled(bool fill, bool send);
    int currentIndex() const;

    std::function<void(int)> fillRequested;
    std::function<void(int)> sendRequested;
    std::function<void()> collapseRequested;
    std::function<void()> manageRequested;
    std::function<void()> importRequested;
    std::function<void()> exportRequested;

  private:
    QListWidget* list_ = nullptr;
    std::vector<QPushButton*> fillButtons_;
    std::vector<QPushButton*> sendButtons_;
};
