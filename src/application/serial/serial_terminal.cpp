#include "serial_terminal.h"

namespace testtools {

testtools::ByteBuffer terminalKeyBytes(TerminalKey key, testtools::LineEnding ending)
{
    std::string text;
    switch (key) {
    case TerminalKey::Enter: {
        testtools::ByteBuffer bytes;
        testtools::appendLineEnding(bytes, ending);
        return bytes;
    }
    case TerminalKey::Tab:
        text = "\t";
        break;
    case TerminalKey::Backspace:
        text = "\x7f";
        break;
    case TerminalKey::Up:
        text = "\x1b[A";
        break;
    case TerminalKey::Down:
        text = "\x1b[B";
        break;
    case TerminalKey::Right:
        text = "\x1b[C";
        break;
    case TerminalKey::Left:
        text = "\x1b[D";
        break;
    case TerminalKey::Home:
        text = "\x1b[H";
        break;
    case TerminalKey::End:
        text = "\x1b[F";
        break;
    case TerminalKey::Delete:
        text = "\x1b[3~";
        break;
    case TerminalKey::PageUp:
        text = "\x1b[5~";
        break;
    case TerminalKey::PageDown:
        text = "\x1b[6~";
        break;
    case TerminalKey::Escape:
        text = "\x1b";
        break;
    }
    return {text.begin(), text.end()};
}

testtools::ByteBuffer terminalPasteBytes(const std::string& text, testtools::LineEnding ending)
{
    testtools::ByteBuffer bytes;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' || text[i] == '\n') {
            if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
                ++i;
            testtools::appendLineEnding(bytes, ending);
        } else
            bytes.push_back(static_cast<std::uint8_t>(text[i]));
    }
    return bytes;
}

} // namespace testtools
