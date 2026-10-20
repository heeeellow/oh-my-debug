#include "application/serial/serial_terminal.h"

#include "core/serial/codec.h"

#include <algorithm>

namespace testtools {
namespace {
void appendUtf8(std::string& text, char32_t cp)
{
    if (cp <= 0x7f) text.push_back(static_cast<char>(cp));
    else if (cp <= 0x7ff) {
        text.push_back(static_cast<char>(0xc0 | (cp >> 6)));
        text.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else if (cp <= 0xffff) {
        text.push_back(static_cast<char>(0xe0 | (cp >> 12)));
        text.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        text.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
        text.push_back(static_cast<char>(0xf0 | (cp >> 18)));
        text.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
        text.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        text.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
}

std::uint32_t indexedColor(int index)
{
    static constexpr std::uint32_t palette[] = {
        0x252525, 0xb33d32, 0x398044, 0x997014, 0x315fba, 0x9c4293, 0x227b87, 0xc4c4c4,
        0x737373, 0xdb5346, 0x53a55f, 0xb39122, 0x597fe0, 0xba62af, 0x36a0ac, 0xf4f4f4
    };
    index = std::clamp(index, 0, 255);
    if (index < 16) return palette[index];
    if (index >= 232) {
        const auto level = static_cast<std::uint32_t>(8 + (index - 232) * 10);
        return level * 0x010101;
    }
    index -= 16;
    const auto component = [](int value) { return value == 0 ? 0 : 55 + value * 40; };
    return static_cast<std::uint32_t>((component(index / 36) << 16)
        | (component((index / 6) % 6) << 8) | component(index % 6));
}
} // namespace

bool TerminalStyle::operator==(const TerminalStyle& other) const noexcept
{
    return foreground == other.foreground && background == other.background
        && bold == other.bold && underline == other.underline && inverse == other.inverse;
}

std::string TerminalLine::text() const
{
    std::string result;
    for (const auto& cell : cells) appendUtf8(result, cell.character);
    return result;
}

SerialTerminalBuffer::SerialTerminalBuffer(std::size_t maxLines, std::size_t maxCells)
    : maxLines_(std::max<std::size_t>(2, maxLines)),
      maxCells_(std::max<std::size_t>(MaximumColumns, maxCells))
{
    lines_.emplace_back();
}

void SerialTerminalBuffer::markChanged(std::size_t row)
{
    firstChangedLine_ = dirty_ ? std::min(firstChangedLine_, row) : row;
    dirty_ = true;
}

void SerialTerminalBuffer::acknowledgeChanges() noexcept
{
    dirty_ = fullRedraw_ = false;
    firstChangedLine_ = 0;
}

void SerialTerminalBuffer::resizeLine(std::size_t row, std::size_t size)
{
    auto& line = lines_[row];
    totalCells_ -= line.cells.size();
    line.cells.resize(std::min(size, MaximumColumns), {U' ', style_});
    totalCells_ += line.cells.size();
    markChanged(row);
}

void SerialTerminalBuffer::ensureRow(std::size_t row)
{
    while (lines_.size() <= row) {
        lines_.push_back({{}, timestamp_});
        markChanged(lines_.size() - 1);
    }
}

void SerialTerminalBuffer::trim()
{
    while (lines_.size() > maxLines_ || (totalCells_ > maxCells_ && lines_.size() > 1)) {
        totalCells_ -= lines_.front().cells.size();
        lines_.pop_front();
        row_ = row_ ? row_ - 1 : 0;
        savedRow_ = savedRow_ ? savedRow_ - 1 : 0;
        truncated_ = fullRedraw_ = dirty_ = true;
        firstChangedLine_ = 0;
    }
}

void SerialTerminalBuffer::put(char32_t character)
{
    // Bound a line without silently overwriting its final character on overflow.
    if (column_ >= MaximumColumns) lineFeed();
    auto& line = lines_[row_];
    if (line.cells.empty()) line.timestamp = timestamp_;
    if (column_ >= line.cells.size()) resizeLine(row_, column_ + 1);
    line.cells[column_++] = {character, style_};
    markChanged(row_);
}

void SerialTerminalBuffer::lineFeed()
{
    column_ = 0;
    ++row_;
    ensureRow(row_);
    trim();
}

void SerialTerminalBuffer::finishUtf8()
{
    if (utf8Remaining_) put(0xfffd);
    utf8Remaining_ = 0;
}

void SerialTerminalBuffer::textByte(std::uint8_t byte)
{
    if (utf8Remaining_) {
        if ((byte & 0xc0) == 0x80) {
            utf8Value_ = (utf8Value_ << 6) | (byte & 0x3f);
            if (--utf8Remaining_ == 0) {
                put(utf8Value_ < utf8Minimum_ || utf8Value_ > 0x10ffff
                            || (utf8Value_ >= 0xd800 && utf8Value_ <= 0xdfff) ? 0xfffd : utf8Value_);
            }
            return;
        }
        finishUtf8();
    }
    if (byte < 0x80) put(byte);
    else if (byte >= 0xc2 && byte <= 0xdf) {
        utf8Value_ = byte & 0x1f; utf8Remaining_ = 1; utf8Minimum_ = 0x80;
    } else if (byte >= 0xe0 && byte <= 0xef) {
        utf8Value_ = byte & 0x0f; utf8Remaining_ = 2; utf8Minimum_ = 0x800;
    } else if (byte >= 0xf0 && byte <= 0xf4) {
        utf8Value_ = byte & 0x07; utf8Remaining_ = 3; utf8Minimum_ = 0x10000;
    } else put(0xfffd);
}

void SerialTerminalBuffer::push(const serialdemo::ByteBuffer& bytes, const std::string& timestamp)
{
    const auto previousRow = row_;
    const auto previousColumn = column_;
    timestamp_ = timestamp;
    for (const auto byte : bytes) {
        consume(byte);
        trim();
    }
    if (row_ != previousRow || column_ != previousColumn) {
        // Cursor-only echoes (BS / CSI D / CR) must also reach the renderer.
        if (column_ > lines_[row_].cells.size()) resizeLine(row_, column_);
        trim();
        markChanged(row_);
    }
}

void SerialTerminalBuffer::consume(std::uint8_t byte)
{
    if (state_ == State::Osc || state_ == State::OscEscape) {
        if (byte == 7 || (state_ == State::OscEscape && byte == '\\') || ++ignoredBytes_ > 4096)
            state_ = State::Text;
        else state_ = byte == 0x1b ? State::OscEscape : State::Osc;
        return; // OSC titles/clipboard requests are consumed, never acted on.
    }
    if (byte == 0x1b) { finishUtf8(); state_ = State::Escape; return; }
    if (state_ == State::Charset) { state_ = State::Text; return; }
    if (state_ == State::Escape) {
        state_ = State::Text;
        if (byte == '[') { state_ = State::Csi; sequence_.clear(); }
        else if (byte == ']' || byte == 'P' || byte == 'X' || byte == '^' || byte == '_') {
            state_ = State::Osc; ignoredBytes_ = 0;
        }
        else if (byte == '(' || byte == ')') state_ = State::Charset;
        else if (byte == '7') { savedRow_ = row_; savedColumn_ = column_; }
        else if (byte == '8') { row_ = std::min(savedRow_, lines_.size()-1); column_ = savedColumn_; }
        else if (byte == 'c') clear();
        return;
    }
    if (state_ == State::Csi) {
        if (byte >= 0x40 && byte <= 0x7e) { controlSequence(static_cast<char>(byte)); state_ = State::Text; }
        else if (byte >= 0x20 && byte <= 0x3f && sequence_.size() < 96) sequence_.push_back(static_cast<char>(byte));
        else { state_ = State::Text; sequence_.clear(); }
        return;
    }
    if (byte < 0x20 || byte == 0x7f) {
        finishUtf8();
        switch (byte) {
        case '\r': column_ = 0; break;
        case '\n': lineFeed(); break;
        case '\b': if (column_) --column_; break;
        case '\t': {
            const auto target = std::min(MaximumColumns, (column_ / 8 + 1) * 8);
            if (lines_[row_].cells.empty()) lines_[row_].timestamp = timestamp_;
            if (target > lines_[row_].cells.size()) resizeLine(row_, target);
            column_ = target;
            break;
        }
        default: break;
        }
        return;
    }
    textByte(byte);
}

void SerialTerminalBuffer::controlSequence(char final)
{
    if (sequence_.find_first_not_of("0123456789;") != std::string::npos) return;
    std::vector<int> params(1, 0);
    for (const char ch : sequence_) {
        if (ch == ';') params.push_back(0);
        else params.back() = std::min(100000, params.back() * 10 + ch - '0');
    }
    const auto argument = [&params](std::size_t i, int fallback = 1) {
        return i < params.size() && params[i] ? params[i] : fallback;
    };
    const auto boundedRow = [this](std::size_t row) { return std::min(row, maxLines_ - 1); };
    switch (final) {
    case 'm': setGraphics(params); break;
    case 'A': row_ -= std::min(row_, static_cast<std::size_t>(argument(0))); break;
    case 'B': row_ = boundedRow(row_ + argument(0)); ensureRow(row_); break;
    case 'C': column_ = std::min(MaximumColumns - 1, column_ + argument(0)); break;
    case 'D': column_ -= std::min(column_, static_cast<std::size_t>(argument(0))); break;
    case 'G': column_ = std::min(MaximumColumns - 1, static_cast<std::size_t>(argument(0) - 1)); break;
    case 'H': case 'f':
        row_ = boundedRow(static_cast<std::size_t>(argument(0) - 1)); ensureRow(row_);
        column_ = std::min(MaximumColumns - 1, static_cast<std::size_t>(argument(1) - 1)); break;
    case 'K': {
        auto& cells = lines_[row_].cells;
        if (params[0] == 0) resizeLine(row_, std::min(column_, cells.size()));
        else if (params[0] == 1) {
            for (std::size_t i = 0; i < std::min(column_ + 1, cells.size()); ++i) cells[i] = {U' ', style_};
            markChanged(row_);
        } else if (params[0] == 2) resizeLine(row_, 0);
        break;
    }
    case 'J':
        if (params[0] == 2 || params[0] == 3) {
            // This scrollback view has no separate alternate screen. Keep the next
            // prompt visible after the common clear-screen + home sequence.
            const auto graphics = style_;
            clear();
            style_ = graphics;
        } else if (params[0] == 0) {
            resizeLine(row_, std::min(column_, lines_[row_].cells.size()));
            while (lines_.size() > row_ + 1) {
                totalCells_ -= lines_.back().cells.size();
                lines_.pop_back();
            }
        }
        break;
    case 's': savedRow_ = row_; savedColumn_ = column_; break;
    case 'u': row_ = std::min(savedRow_, lines_.size()-1); column_ = savedColumn_; break;
    default: break;
    }
}

void SerialTerminalBuffer::setGraphics(const std::vector<int>& params)
{
    for (std::size_t i = 0; i < params.size(); ++i) {
        const int p = params[i];
        if (p == 0) style_ = {};
        else if (p == 1) style_.bold = true;
        else if (p == 4) style_.underline = true;
        else if (p == 7) style_.inverse = true;
        else if (p == 22) style_.bold = false;
        else if (p == 24) style_.underline = false;
        else if (p == 27) style_.inverse = false;
        else if (p == 39) style_.foreground = TerminalStyle::DefaultColor;
        else if (p == 49) style_.background = TerminalStyle::DefaultColor;
        else if (p >= 30 && p <= 37) style_.foreground = indexedColor(p - 30);
        else if (p >= 40 && p <= 47) style_.background = indexedColor(p - 40);
        else if (p >= 90 && p <= 97) style_.foreground = indexedColor(p - 90 + 8);
        else if (p >= 100 && p <= 107) style_.background = indexedColor(p - 100 + 8);
        else if (p == 38 || p == 48) {
            auto& color = p == 38 ? style_.foreground : style_.background;
            if (i + 2 < params.size() && params[i+1] == 5) {
                color = indexedColor(params[i+2]); i += 2;
            } else if (i + 4 < params.size() && params[i+1] == 2) {
                color = static_cast<std::uint32_t>((std::clamp(params[i+2],0,255) << 16)
                    | (std::clamp(params[i+3],0,255) << 8) | std::clamp(params[i+4],0,255)); i += 4;
            } else break;
        }
    }
}

void SerialTerminalBuffer::resetStream()
{
    finishUtf8();
    state_ = State::Text; style_ = {}; sequence_.clear();
    row_ = lines_.size() - 1;
    if (!lines_.back().cells.empty()) lineFeed();
    column_ = savedColumn_ = 0; savedRow_ = row_;
}

void SerialTerminalBuffer::clear()
{
    lines_.clear(); lines_.emplace_back(); totalCells_ = 0;
    row_ = column_ = savedRow_ = savedColumn_ = firstChangedLine_ = 0;
    state_ = State::Text; style_ = {}; sequence_.clear(); utf8Remaining_ = 0;
    truncated_ = false; dirty_ = fullRedraw_ = true;
}

std::string SerialTerminalBuffer::plainText(bool timestamps, const std::string& filter) const
{
    std::string result;
    bool inserted = false;
    for (std::size_t i = 0; i < lines_.size(); ++i) {
        const auto& line = lines_[i];
        std::string text = (timestamps && !line.timestamp.empty() ? "[" + line.timestamp + "] " : "") + line.text();
        if (!filter.empty() && text.find(filter) == std::string::npos) continue;
        if (inserted) result += '\n';
        result += text;
        inserted = true;
    }
    return result;
}

serialdemo::ByteBuffer terminalKeyBytes(TerminalKey key, serialdemo::LineEnding ending)
{
    std::string text;
    switch (key) {
    case TerminalKey::Enter: { serialdemo::ByteBuffer bytes; serialdemo::appendLineEnding(bytes, ending); return bytes; }
    case TerminalKey::Tab: text = "\t"; break;
    case TerminalKey::Backspace: text = "\x7f"; break;
    case TerminalKey::Up: text = "\x1b[A"; break;
    case TerminalKey::Down: text = "\x1b[B"; break;
    case TerminalKey::Right: text = "\x1b[C"; break;
    case TerminalKey::Left: text = "\x1b[D"; break;
    case TerminalKey::Home: text = "\x1b[H"; break;
    case TerminalKey::End: text = "\x1b[F"; break;
    case TerminalKey::Delete: text = "\x1b[3~"; break;
    case TerminalKey::PageUp: text = "\x1b[5~"; break;
    case TerminalKey::PageDown: text = "\x1b[6~"; break;
    case TerminalKey::Escape: text = "\x1b"; break;
    }
    return {text.begin(), text.end()};
}

serialdemo::ByteBuffer terminalPasteBytes(const std::string& text, serialdemo::LineEnding ending)
{
    serialdemo::ByteBuffer bytes;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' || text[i] == '\n') {
            if (text[i] == '\r' && i + 1 < text.size() && text[i+1] == '\n') ++i;
            serialdemo::appendLineEnding(bytes, ending);
        } else bytes.push_back(static_cast<std::uint8_t>(text[i]));
    }
    return bytes;
}

} // namespace testtools
