#pragma once

#include "core/serial/codec.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace testtools {

struct TerminalStyle {
    static constexpr std::uint32_t DefaultColor = 0xffffffff;
    std::uint32_t foreground = DefaultColor;
    std::uint32_t background = DefaultColor;
    bool bold = false;
    bool underline = false;
    bool inverse = false;
    bool operator==(const TerminalStyle& other) const noexcept;
};

struct TerminalCell {
    char32_t character = U' ';
    TerminalStyle style;
};

struct TerminalLine {
    std::vector<TerminalCell> cells;
    std::string timestamp;
    std::string text() const;
};

// 有界、面向行的 ANSI/VT 串口终端屏幕缓冲区：仅消费接收流（RX），绝不自动回发终端应答。
class SerialTerminalBuffer {
public:
    explicit SerialTerminalBuffer(std::size_t maxLines = 10000,
                                  std::size_t maxCells = 1024 * 1024);
    void push(const serialdemo::ByteBuffer& bytes, const std::string& timestamp = {});
    void resetStream();
    void clear();
    const std::deque<TerminalLine>& lines() const noexcept { return lines_; }
    bool truncated() const noexcept { return truncated_; }
    bool dirty() const noexcept { return dirty_; }
    std::size_t cursorRow() const noexcept { return row_; }
    std::size_t cursorColumn() const noexcept { return column_; }
    bool needsFullRedraw() const noexcept { return fullRedraw_; }
    std::size_t firstChangedLine() const noexcept { return firstChangedLine_; }
    std::size_t removedLines() const noexcept { return removedLines_; }
    void acknowledgeChanges() noexcept;
    std::string plainText(bool timestamps = false, const std::string& filter = {}) const;

private:
    enum class State { Text, Escape, Csi, Osc, OscEscape, Charset };
    void consume(std::uint8_t byte);
    void textByte(std::uint8_t byte);
    void put(char32_t character);
    void lineFeed();
    void ensureRow(std::size_t row);
    void markChanged(std::size_t row);
    void trim();
    void finishUtf8();
    void controlSequence(char final);
    void setGraphics(const std::vector<int>& parameters);
    void resizeLine(std::size_t row, std::size_t size);

    static constexpr std::size_t MaximumColumns = 4096;
    std::deque<TerminalLine> lines_;
    std::size_t maxLines_, maxCells_, totalCells_ = 0;
    std::size_t row_ = 0, column_ = 0, savedRow_ = 0, savedColumn_ = 0;
    std::size_t firstChangedLine_ = 0, ignoredBytes_ = 0;
    std::size_t removedLines_ = 0;
    bool truncated_ = false, dirty_ = true, fullRedraw_ = true;
    State state_ = State::Text;
    TerminalStyle style_;
    std::string sequence_, timestamp_;
    char32_t utf8Value_ = 0, utf8Minimum_ = 0;
    int utf8Remaining_ = 0;
};

enum class TerminalKey { Enter, Tab, Backspace, Up, Down, Right, Left, Home, End,
                         Delete, PageUp, PageDown, Escape };
serialdemo::ByteBuffer terminalKeyBytes(TerminalKey key, serialdemo::LineEnding ending);
serialdemo::ByteBuffer terminalPasteBytes(const std::string& text, serialdemo::LineEnding ending);

} // namespace testtools
