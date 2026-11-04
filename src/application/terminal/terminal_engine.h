#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace testtools {

struct TerminalColor {
    std::uint8_t red = 0, green = 0, blue = 0;
    bool defaultColor = true;
    int paletteIndex = -1;
};
struct TerminalGridCell {
    std::array<std::uint32_t, 6> characters{};
    int width = 1;
    TerminalColor foreground, background;
    bool bold = false, italic = false, reverse = false, strike = false, conceal = false;
    int underline = 0;
};
struct TerminalGridLine {
    std::uint64_t id = 0;
    std::vector<TerminalGridCell> cells;
    bool continuation = false;
    std::int64_t timestampMs = 0;
};
using TerminalLinePtr = std::shared_ptr<const TerminalGridLine>;
struct TerminalFrame {
    std::shared_ptr<const std::vector<TerminalLinePtr>> history;
    std::vector<TerminalLinePtr> screen;
    int rows = 24, columns = 80;
    int cursorRow = 0, cursorColumn = 0, cursorShape = 1;
    bool cursorVisible = true, cursorBlink = true, alternateScreen = false;
    int mouseMode = 0;
    std::uint64_t revision = 0, generation = 0, droppedBytes = 0, trimmedLines = 0;
    std::uint64_t parsedBytes = 0, publishedFrames = 0, rejectedInputs = 0;
    std::size_t queuedBytes = 0;
};
enum class TerminalInputKey {
    Enter,
    Tab,
    Backspace,
    Escape,
    Up,
    Down,
    Left,
    Right,
    Insert,
    Delete,
    Home,
    End,
    PageUp,
    PageDown,
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12
};

// All VT state belongs to one worker. Readers only hold immutable published frames.
// RX is bounded separately from SerialRecorder; overflow is counted and resynchronised.
class TerminalEngine final {
  public:
    struct Limits {
        std::size_t historyLines = 20000;
        std::size_t historyCells = 1024 * 1024;
        std::size_t queuedBytes = 1024 * 1024;
        std::size_t queuedCommands = 2048;
    };
    using Changed = std::function<void()>;
    using Output = std::function<void(std::uint64_t, const std::vector<std::uint8_t>&)>;
    explicit TerminalEngine(Changed changed = {}, Output output = {});
    TerminalEngine(Limits limits, Changed changed = {}, Output output = {});
    ~TerminalEngine();
    TerminalEngine(const TerminalEngine&) = delete;
    TerminalEngine& operator=(const TerminalEngine&) = delete;

    bool receive(std::uint64_t generation, const std::vector<std::uint8_t>& bytes);
    void resize(int rows, int columns);
    void resetStream();
    void clear();
    void key(std::uint64_t generation, TerminalInputKey key, int modifiers = 0);
    void text(std::uint64_t generation, const std::u32string& text, int modifiers = 0);
    void paste(std::uint64_t generation, std::string utf8);
    void rawInput(std::uint64_t generation, std::vector<std::uint8_t> bytes);
    void mouse(
        std::uint64_t generation, int row, int column, int button, bool pressed, int modifiers = 0);
    std::shared_ptr<const TerminalFrame> snapshot() const;
    // Explicit barrier for host verification/shutdown; never called by GUI rendering.
    void waitIdle();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace testtools
