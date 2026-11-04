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
serialdemo::ByteBuffer terminalKeyBytes(TerminalKey key, serialdemo::LineEnding ending);
serialdemo::ByteBuffer terminalPasteBytes(const std::string& text, serialdemo::LineEnding ending);

} // namespace testtools
