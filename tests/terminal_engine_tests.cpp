#include "application/terminal/terminal_engine.h"
#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
using namespace testtools;
void check(bool value, const char* message)
{
    if (!value)
        throw std::runtime_error(message);
}
std::string rowText(const TerminalLinePtr& line)
{
    std::string text;
    for (const auto& cell : line->cells) {
        if (cell.width == 0)
            continue;
        text += cell.characters[0] && cell.characters[0] < 128 ? char(cell.characters[0]) : ' ';
    }
    while (!text.empty() && text.back() == ' ')
        text.pop_back();
    return text;
}
int main()
{
    try {
        {
            TerminalEngine timestamps;
            const auto blankTimes = [&] {
                const auto f = timestamps.snapshot();
                for (const auto& line : f->screen)
                    if (line->timestampMs != 0)
                        return false;
                return true;
            };
            check(blankTimes(), "unconnected blank grid has no receive timestamps");
            timestamps.resize(6, 40);
            timestamps.waitIdle();
            check(blankTimes(), "resize cannot timestamp blank rows");
            timestamps.rawInput(1, {'x'});
            timestamps.waitIdle();
            check(blankTimes(), "TX without echo cannot timestamp rows");
            timestamps.receive(1, {'h', 'e', 'l', 'l', 'o'});
            timestamps.waitIdle();
            const auto receivedAt = timestamps.snapshot()->screen[0]->timestampMs;
            check(receivedAt > 0 && timestamps.snapshot()->screen[1]->timestampMs == 0,
                  "only received text gets a timestamp");
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
            timestamps.resize(8, 48);
            timestamps.waitIdle();
            check(timestamps.snapshot()->screen[0]->timestampMs == receivedAt,
                  "resize preserves receive time");
            const std::string cursor = "\x1b[2D";
            timestamps.receive(1, {cursor.begin(), cursor.end()});
            timestamps.waitIdle();
            check(timestamps.snapshot()->screen[0]->timestampMs == receivedAt,
                  "cursor-only RX does not retime text");
            timestamps.clear();
            timestamps.waitIdle();
            check(blankTimes(), "clear removes timestamps from blank screen");
        }
        std::mutex mutex;
        std::string output;
        TerminalEngine engine({}, [&](std::uint64_t, const std::vector<std::uint8_t>& b) {
            std::lock_guard<std::mutex> lock(mutex);
            output.append(b.begin(), b.end());
        });
        const auto rx = [&](const std::string& text, std::uint64_t generation = 1) {
            check(engine.receive(generation, {text.begin(), text.end()}), "RX accepted");
            engine.waitIdle();
        };
        const auto take = [&] {
            std::lock_guard<std::mutex> lock(mutex);
            auto s = output;
            output.clear();
            return s;
        };
        engine.resize(4, 12);
        rx("hello\x1b[2D!");
        auto f = engine.snapshot();
        check(rowText(f->screen[0]) == "hel!o" && f->cursorColumn == 4, "cursor overwrite");
        const auto retained = f;
        rx("\x1b[?1049h\x1b[2J\x1b[Halternate");
        check(engine.snapshot()->alternateScreen &&
                  rowText(engine.snapshot()->screen[0]) == "alternate",
              "alternate screen");
        rx("\x1b[?1049l");
        check(!engine.snapshot()->alternateScreen &&
                  rowText(engine.snapshot()->screen[0]) == "hel!o",
              "restore primary");
        check(rowText(retained->screen[0]) == "hel!o", "immutable old frame");
        rx("\x1b[2J\x1b[Htop\r\nA\r\nB\r\nbottom\x1b[2;3r\x1b[3;1H\n");
        f = engine.snapshot();
        check(rowText(f->screen[0]) == "top" && rowText(f->screen[1]) == "B" &&
                  rowText(f->screen[3]) == "bottom",
              "scroll region preserves outer rows");
        engine.clear();
        rx("\xe4\xb8");
        rx("\xad\xcc\x81X");
        f = engine.snapshot();
        check(f->screen[0]->cells[0].characters[0] == 0x4e2d && f->screen[0]->cells[0].width == 2 &&
                  f->screen[0]->cells[2].characters[0] == 'X',
              "fragmented UTF8 wide cell");
        check(f->screen[0]->cells[0].characters[1] == 0x301, "combining mark");
        engine.clear();
        rx("\x1b[38;2;12;34;56;48;5;123;1;3;4;7mX");
        auto cell = engine.snapshot()->screen[0]->cells[0];
        check(cell.foreground.red == 12 && cell.foreground.green == 34 &&
                  cell.foreground.blue == 56 && !cell.foreground.defaultColor && cell.bold &&
                  cell.italic && cell.underline && cell.reverse,
              "truecolor and attributes");
        engine.clear();
        rx("\x1b[?1h");
        engine.key(1, TerminalInputKey::Up);
        engine.waitIdle();
        check(take() == "\x1bOA", "application cursor mode");
        rx("\x1b[?1l");
        engine.key(1, TerminalInputKey::Up, 5);
        engine.waitIdle();
        check(take() == "\x1b[1;6A", "modified cursor key");
        rx("\x1b[?2004h");
        engine.paste(1, "a\rb");
        engine.waitIdle();
        check(take() == "\x1b[200~a\rb\x1b[201~", "bracketed paste");
        rx("\x1b[6n");
        check(take() == "\x1b[1;1R", "fixed cursor report");
        rx("\x1b]52;c;Zm9v\a");
        check(take().empty(), "OSC52 has no output or clipboard callback");
        engine.clear();
        rx("123456789012XYZ");
        check(engine.snapshot()->screen[1]->continuation, "soft wrap metadata");
        engine.resize(4, 8);
        engine.waitIdle();
        f = engine.snapshot();
        check(rowText(f->screen[0]) == "12345678" && rowText(f->screen[1]) == "9012XYZ",
              "live resize reflows");
        engine.clear();
        engine.resize(3, 12);
        rx("123456789012abcdefghij12XYZ\r\nhard\r\nend\r\n");
        engine.resize(3, 8);
        engine.waitIdle();
        f = engine.snapshot();
        std::string historyText;
        for (const auto& line : *f->history)
            historyText += rowText(line);
        check(historyText.find("123456789012abcdefghij12XYZ") != std::string::npos,
              "history reflows without losing wrapped cells");
        rx("\x1b[");
        engine.resetStream();
        rx("NEW", 3);
        const auto parsed = engine.snapshot()->parsedBytes;
        rx("OLD", 1);
        check(engine.snapshot()->parsedBytes == parsed, "stale generation ignored");
        engine.key(1, TerminalInputKey::Up);
        engine.waitIdle();
        check(take().empty(), "stale key ignored");
        engine.resize(4, 12);
        engine.clear();
        TerminalEngine::Limits limits;
        limits.historyLines = 20;
        limits.historyCells = 512;
        limits.queuedBytes = 4096;
        TerminalEngine bounded(limits);
        bounded.resize(3, 12);
        for (int i = 0; i < 200; ++i) {
            std::string s = "line" + std::to_string(i) + "\r\n";
            bounded.receive(1, {s.begin(), s.end()});
        }
        bounded.waitIdle();
        f = bounded.snapshot();
        check(f->history->size() <= 20 && f->trimmedLines > 0, "bounded history");
        check(!bounded.receive(1, std::vector<std::uint8_t>(8192, 'x')), "oversized RX rejected");
        bounded.receive(1, {'O', 'K'});
        bounded.waitIdle();
        f = bounded.snapshot();
        check(f->droppedBytes == 8192 && rowText(f->screen[0]) == "OK",
              "ordered overflow recovery and accounting");
        bounded.receive(3, {'N'});
        bounded.waitIdle();
        bounded.receive(1, std::vector<std::uint8_t>(8192, 'x'));
        bounded.waitIdle();
        check(bounded.snapshot()->droppedBytes == 8192 &&
                  rowText(bounded.snapshot()->screen[0]) == "N",
              "stale overflow marker cannot reset a newer connection");
        bounded.rawInput(3, std::vector<std::uint8_t>(8192, 'y'));
        bounded.waitIdle();
        check(bounded.snapshot()->rejectedInputs == 1,
              "oversized input rejected separately from RX loss");
        engine.waitIdle();
        const auto idleRevision = engine.snapshot()->revision;
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        check(engine.snapshot()->revision == idleRevision, "idle engine does not publish");
        std::cout << "PASS terminal: "
                     "alternate/scroll/UTF8/combining/colors/input/reflow/generation/bounds/"
                     "overflow/idle\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
