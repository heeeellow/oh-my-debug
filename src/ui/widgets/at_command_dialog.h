#pragma once
#include "application/serial/at_command_library.h"
#include <functional>
class QWidget;
// 编辑动作仍由指令库校验，持久化与列表刷新交回调用页面。
class AtCommandDialog {
  public:
    static void open(QWidget* parent,
                     testtools::AtCommandLibrary& library,
                     int selected,
                     const std::function<void()>& changed);
};
