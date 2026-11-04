#pragma once
#include <QtWidgets/QPlainTextEdit>

// Ordinary RX/TX log view. Interactive terminal drawing belongs to TerminalGridWidget.
class ReceiveConsole final : public QPlainTextEdit {
  public:
    explicit ReceiveConsole(QWidget* parent) : QPlainTextEdit(parent) {}
};
