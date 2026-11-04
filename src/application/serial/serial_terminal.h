#pragma once
#include "core/serial/codec.h"
#include <string>

namespace testtools {

enum class TerminalKey {
    Enter,
    Tab,
    Backspace,
    Up,
    Down,
    Right,
    Left,
    Home,
    End,
    Delete,
    PageUp,
    PageDown,
    Escape
};
testtools::ByteBuffer terminalKeyBytes(TerminalKey key, testtools::LineEnding ending);
testtools::ByteBuffer terminalPasteBytes(const std::string& text, testtools::LineEnding ending);

} // namespace testtools
