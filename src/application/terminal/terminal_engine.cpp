#include "terminal_engine.h"
#include <vterm.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>

namespace testtools {
namespace {
using Clock = std::chrono::steady_clock;
std::int64_t timestamp()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
TerminalColor color(VTermScreen* screen, VTermColor c)
{
    const bool def = VTERM_COLOR_IS_DEFAULT_FG(&c) || VTERM_COLOR_IS_DEFAULT_BG(&c);
    const int index = VTERM_COLOR_IS_INDEXED(&c) ? c.indexed.idx : -1;
    vterm_screen_convert_color_to_rgb(screen, &c);
    return {c.rgb.red, c.rgb.green, c.rgb.blue, def, index};
}
} // namespace

struct TerminalEngine::Impl {
    enum class Kind { Rx, Resize, Reset, Clear, Key, Text, Paste, Raw, Mouse, Gap, Barrier };
    struct Command {
        Kind kind;
        std::uint64_t generation = 0;
        std::uint64_t lostBytes = 0;
        std::vector<std::uint8_t> bytes;
        std::u32string text;
        int a = 0, b = 0, c = 0, modifiers = 0;
        bool pressed = false;
        std::shared_ptr<std::promise<void>> barrier;
    };
    struct HistoryLine {
        TerminalLinePtr view;
        std::vector<VTermScreenCell> raw;
    };
    Limits limits;
    Changed changed;
    Output output;
    mutable std::mutex mutex;
    std::condition_variable ready;
    std::deque<Command> commands;
    std::size_t queued = 0;
    std::uint64_t pendingRejected = 0, rejectedInputs = 0;
    bool stopping = false;
    std::thread worker;
    std::shared_ptr<const TerminalFrame> published;
    VTerm* vt = nullptr;
    VTermScreen* screen = nullptr;
    VTermState* state = nullptr;
    int rows = 24, columns = 80, pushRow = 0;
    bool resizingColumns = false;
    std::deque<HistoryLine> history;
    std::size_t historyCells = 0;
    std::shared_ptr<const std::vector<TerminalLinePtr>> historyView;
    std::vector<TerminalLinePtr> screenView;
    std::vector<bool> dirtyRows;
    std::vector<std::int64_t> rowTimes, primaryTimes, alternateTimes;
    std::int64_t lastReceivedAt = 0;
    bool receiving = false;
    bool dirty = true, historyDirty = true, alternate = false, visible = true, blink = true;
    int cursorRow = 0, cursorColumn = 0, cursorShape = 1, mouseMode = 0;
    std::uint64_t generation = 0, revision = 0, nextId = 1, dropped = 0, trimmed = 0;
    std::uint64_t parsed = 0, frames = 0;
    Clock::time_point lastPublish{};

    Impl(Limits l, Changed c, Output o) : limits(l), changed(std::move(c)), output(std::move(o))
    {
        limits.historyLines = std::max<std::size_t>(1, limits.historyLines);
        limits.historyCells = std::max<std::size_t>(512, limits.historyCells);
        limits.queuedBytes = std::max<std::size_t>(256, limits.queuedBytes);
        limits.queuedCommands = std::max<std::size_t>(16, limits.queuedCommands);
        create();
        publish();
        worker = std::thread([this] { run(); });
    }
    ~Impl()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }
        ready.notify_one();
        worker.join();
        vterm_free(vt);
    }
    void markAll()
    {
        dirtyRows.assign(rows, true);
        dirty = true;
    }
    std::shared_ptr<TerminalGridLine> line(const VTermScreenCell* cells,
                                           int count,
                                           bool continuation,
                                           std::uint64_t id = 0,
                                           std::int64_t time = 0)
    {
        auto result = std::make_shared<TerminalGridLine>();
        result->id = id ? id : nextId++;
        result->continuation = continuation;
        result->cells.reserve(count);
        for (int i = 0; i < count; ++i) {
            const auto& src = cells[i];
            TerminalGridCell dst;
            std::copy(std::begin(src.chars), std::end(src.chars), dst.characters.begin());
            // libvterm represents the second half of a wide cell with UINT32_MAX.
            if (dst.characters[0] == UINT32_MAX) {
                dst.characters = {};
                dst.width = 0;
            } else {
                dst.width = src.width;
            }
            dst.foreground = color(screen, src.fg);
            dst.background = color(screen, src.bg);
            dst.bold = src.attrs.bold;
            dst.italic = src.attrs.italic;
            dst.underline = src.attrs.underline;
            dst.reverse = src.attrs.reverse;
            dst.strike = src.attrs.strike;
            dst.conceal = src.attrs.conceal;
            result->cells.push_back(dst);
        }
        const bool hasText = std::any_of(
            result->cells.begin(), result->cells.end(), [](const TerminalGridCell& cell) {
                return cell.width > 0 && cell.characters[0] != 0 && cell.characters[0] != U' ';
            });
        result->timestampMs = hasText ? (time ? time : lastReceivedAt) : 0;
        return result;
    }
    void trim()
    {
        while (history.size() > limits.historyLines || historyCells > limits.historyCells) {
            historyCells -= history.front().raw.size();
            history.pop_front();
            ++trimmed;
        }
    }
    void reflowHistory()
    {
        std::deque<HistoryLine> replacement;
        std::vector<VTermScreenCell> logical;
        std::int64_t logicalTime = 0;
        const auto blank = [] {
            VTermScreenCell cell{};
            cell.width = 1;
            vterm_color_rgb(&cell.fg, 220, 220, 220);
            vterm_color_rgb(&cell.bg, 24, 26, 31);
            cell.fg.type |= VTERM_COLOR_DEFAULT_FG;
            cell.bg.type |= VTERM_COLOR_DEFAULT_BG;
            return cell;
        };
        const auto flush = [&] {
            std::size_t at = 0;
            bool continuation = false;
            do {
                HistoryLine h;
                h.raw.assign(columns, blank());
                int column = 0;
                while (at < logical.size() && column < columns) {
                    const int width = std::max(1, int(logical[at].width));
                    if (width == 2 && column + 1 == columns)
                        break;
                    h.raw[column] = logical[at];
                    if (width == 2 && at + 1 < logical.size())
                        h.raw[column + 1] = logical[at + 1];
                    column += width;
                    at += width;
                }
                h.view = line(h.raw.data(), columns, continuation, 0, logicalTime);
                replacement.push_back(std::move(h));
                continuation = true;
            } while (at < logical.size());
            logical.clear();
        };
        for (std::size_t row = 0; row < history.size(); ++row) {
            const auto& h = history[row];
            if (row && !h.view->continuation)
                flush();
            if (logical.empty())
                logicalTime = h.view->timestampMs;
            std::size_t count = h.raw.size();
            if (row + 1 == history.size() || !history[row + 1].view->continuation)
                while (count &&
                       (h.raw[count - 1].chars[0] == 0 || h.raw[count - 1].chars[0] == ' '))
                    --count;
            logical.insert(logical.end(), h.raw.begin(), h.raw.begin() + count);
        }
        if (!history.empty())
            flush();
        history = std::move(replacement);
        historyCells = history.size() * columns;
        trim();
        historyDirty = true;
    }
    void create()
    {
        rowTimes.assign(rows, 0);
        primaryTimes.assign(rows, 0);
        alternateTimes.assign(rows, 0);
        vt = vterm_new(rows, columns);
        vterm_set_utf8(vt, 1);
        screen = vterm_obtain_screen(vt);
        state = vterm_obtain_state(vt);
        static const VTermScreenCallbacks callbacks = {
            [](VTermRect r, void* p) {
                auto& s = *static_cast<Impl*>(p);
                for (int row = std::max(0, r.start_row); row < std::min(s.rows, r.end_row); ++row) {
                    s.dirtyRows[row] = true;
                    if (s.receiving)
                        s.rowTimes[row] = s.lastReceivedAt;
                }
                s.dirty = true;
                return 1;
            },
            [](VTermRect dest, VTermRect src, void* p) {
                auto& s = *static_cast<Impl*>(p);
                if (src.start_col == 0 && src.end_col == s.columns &&
                    s.screenView.size() == std::size_t(s.rows)) {
                    const auto old = s.screenView;
                    for (int row = src.start_row; row < src.end_row; ++row) {
                        const int target = row + dest.start_row - src.start_row;
                        if (target >= 0 && target < s.rows)
                            s.screenView[target] = old[row];
                    }
                    for (int row = src.start_row; row < src.end_row; ++row)
                        if (row < dest.start_row || row >= dest.end_row)
                            s.screenView[row].reset();
                }
                const auto previousTimes = s.rowTimes;
                if (src.start_col == 0 && src.end_col == s.columns) {
                    for (int row = src.start_row;
                         row < src.end_row && row < int(previousTimes.size());
                         ++row) {
                        const int target = row + dest.start_row - src.start_row;
                        if (target >= 0 && target < int(s.rowTimes.size()))
                            s.rowTimes[target] = previousTimes[row];
                    }
                    for (int row = src.start_row; row < src.end_row && row < int(s.rowTimes.size());
                         ++row)
                        if (row < dest.start_row || row >= dest.end_row)
                            s.rowTimes[row] = 0;
                }
                s.pushRow = 0;
                s.markAll();
                return 0;
            },
            [](VTermPos pos, VTermPos, int vis, void* p) {
                auto& s = *static_cast<Impl*>(p);
                s.cursorRow = pos.row;
                s.cursorColumn = pos.col;
                s.visible = vis != 0;
                s.dirty = true;
                return 1;
            },
            [](VTermProp prop, VTermValue* value, void* p) {
                auto& s = *static_cast<Impl*>(p);
                switch (prop) {
                case VTERM_PROP_ALTSCREEN:
                    (s.alternate ? s.alternateTimes : s.primaryTimes) = s.rowTimes;
                    s.alternate = value->boolean != 0;
                    s.rowTimes = s.alternate ? s.alternateTimes : s.primaryTimes;
                    s.rowTimes.resize(s.rows, 0);
                    s.screenView.clear();
                    s.markAll();
                    break;
                case VTERM_PROP_CURSORVISIBLE:
                    s.visible = value->boolean != 0;
                    break;
                case VTERM_PROP_CURSORBLINK:
                    s.blink = value->boolean != 0;
                    break;
                case VTERM_PROP_CURSORSHAPE:
                    s.cursorShape = value->number;
                    break;
                case VTERM_PROP_MOUSE:
                    s.mouseMode = value->number;
                    break;
                default:
                    break; // No OSC clipboard, title changes, process or shell callbacks.
                }
                s.dirty = true;
                return 1;
            },
            [](void*) { return 1; },
            [](int, int, void* p) {
                auto& s = *static_cast<Impl*>(p);
                s.pushRow = 0;
                s.markAll();
                return 1;
            },
            [](int cols, const VTermScreenCell* cells, void* p) {
                auto& s = *static_cast<Impl*>(p);
                const int row = std::min(s.pushRow++, s.rows - 1);
                const auto* info = vterm_state_get_lineinfo(s.state, row);
                HistoryLine h;
                h.raw.assign(cells, cells + cols);
                h.view = s.line(
                    cells,
                    cols,
                    info && info->continuation,
                    row < int(s.screenView.size()) && s.screenView[row] ? s.screenView[row]->id : 0,
                    row < int(s.rowTimes.size()) ? s.rowTimes[row] : 0);
                s.historyCells += cols;
                s.history.push_back(std::move(h));
                s.trim();
                s.historyDirty = true;
                return 1;
            },
            [](int cols, VTermScreenCell* cells, void* p) {
                auto& s = *static_cast<Impl*>(p);
                if (s.history.empty())
                    return 0;
                const auto& h = s.history.back();
                // Upstream's pop callback has no continuation metadata and accepts old_cols.
                // Keep wrapped history outside the live grid rather than lose cells on resize.
                if (s.resizingColumns || h.view->continuation)
                    return 0;
                for (int i = 0; i < cols; ++i) {
                    cells[i] = {};
                    cells[i].width = 1;
                    vterm_color_rgb(&cells[i].fg, 220, 220, 220);
                    vterm_color_rgb(&cells[i].bg, 24, 26, 31);
                    cells[i].fg.type |= VTERM_COLOR_DEFAULT_FG;
                    cells[i].bg.type |= VTERM_COLOR_DEFAULT_BG;
                    if (i < int(h.raw.size()))
                        cells[i] = h.raw[i];
                }
                s.historyCells -= h.raw.size();
                s.history.pop_back();
                s.historyDirty = true;
                return 1;
            },
            [](void* p) {
                auto& s = *static_cast<Impl*>(p);
                s.history.clear();
                s.historyCells = 0;
                s.historyDirty = true;
                return 1;
            }};
        markAll();
        vterm_screen_set_callbacks(screen, &callbacks, this);
        vterm_screen_enable_altscreen(screen, 1);
        vterm_screen_enable_reflow(screen, true);
        vterm_screen_set_damage_merge(screen, VTERM_DAMAGE_ROW);
        vterm_output_set_callback(
            vt,
            [](const char* bytes, size_t len, void* p) {
                auto& s = *static_cast<Impl*>(p);
                if (s.output && len)
                    s.output(s.generation, {bytes, bytes + len});
            },
            this);
        vterm_screen_reset(screen, 1);
    }
    void reset(bool preserve)
    {
        if (preserve && dirty)
            publish();
        // A transport boundary must not carry a partial UTF-8/CSI sequence into its successor.
        if (preserve) {
            std::size_t usedRows = screenView.size();
            while (usedRows && (!screenView[usedRows - 1] ||
                                std::all_of(screenView[usedRows - 1]->cells.begin(),
                                            screenView[usedRows - 1]->cells.end(),
                                            [](const TerminalGridCell& c) {
                                                return c.characters[0] == 0 ||
                                                       c.characters[0] == ' ';
                                            })))
                --usedRows;
            for (std::size_t row = 0; row < usedRows; ++row) {
                const auto& view = screenView[row];
                if (!view)
                    continue;
                HistoryLine h;
                h.view = view;
                h.raw.resize(view->cells.size());
                for (std::size_t i = 0; i < view->cells.size(); ++i) {
                    auto& raw = h.raw[i];
                    const auto& cell = view->cells[i];
                    std::copy(cell.characters.begin(), cell.characters.end(), raw.chars);
                    raw.width = char(cell.width);
                    raw.attrs.bold = cell.bold;
                    raw.attrs.italic = cell.italic;
                    raw.attrs.underline = cell.underline;
                    raw.attrs.reverse = cell.reverse;
                    vterm_color_rgb(
                        &raw.fg, cell.foreground.red, cell.foreground.green, cell.foreground.blue);
                    vterm_color_rgb(
                        &raw.bg, cell.background.red, cell.background.green, cell.background.blue);
                    if (cell.foreground.defaultColor)
                        raw.fg.type |= VTERM_COLOR_DEFAULT_FG;
                    if (cell.background.defaultColor)
                        raw.bg.type |= VTERM_COLOR_DEFAULT_BG;
                }
                historyCells += h.raw.size();
                history.push_back(std::move(h));
            }
            trim();
        }
        vterm_free(vt);
        screenView.clear();
        if (!preserve) {
            history.clear();
            historyCells = 0;
            trimmed = 0;
        }
        historyDirty = true;
        alternate = false;
        pushRow = 0;
        lastReceivedAt = 0;
        create();
    }
    bool enqueue(Command c)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping)
            return false;
        if (c.kind == Kind::Rx && (queued + c.bytes.size() > limits.queuedBytes ||
                                   commands.size() >= limits.queuedCommands)) {
            // One ordered gap marker preserves the boundary even if later fragments fit again.
            if (!commands.empty() && commands.back().kind == Kind::Gap &&
                commands.back().generation == c.generation)
                commands.back().lostBytes += c.bytes.size();
            else {
                Command gap{Kind::Gap};
                gap.generation = c.generation;
                gap.lostBytes = c.bytes.size();
                commands.push_back(std::move(gap));
            }
            ready.notify_one();
            return false;
        }
        if (c.kind != Kind::Rx && c.kind != Kind::Barrier &&
            (commands.size() >= limits.queuedCommands + 64 ||
             queued + c.bytes.size() + c.text.size() * sizeof(char32_t) > limits.queuedBytes)) {
            ++pendingRejected;
            ready.notify_one();
            return false;
        }
        // Reserve bounded space for input/control commands when RX occupies its own limit.
        if (c.kind == Kind::Resize && !commands.empty() && commands.back().kind == Kind::Resize)
            commands.back() = std::move(c);
        else {
            queued += c.bytes.size() + c.text.size() * sizeof(char32_t);
            commands.push_back(std::move(c));
        }
        ready.notify_one();
        return true;
    }
    void publish()
    {
        vterm_screen_flush_damage(screen);
        if (historyDirty) {
            auto h = std::make_shared<std::vector<TerminalLinePtr>>();
            h->reserve(history.size());
            for (const auto& item : history)
                h->push_back(item.view);
            historyView = std::move(h);
            historyDirty = false;
        }
        screenView.resize(rows);
        std::vector<VTermScreenCell> cells(columns);
        for (int row = 0; row < rows; ++row) {
            if (screenView[row] && !dirtyRows[row])
                continue;
            for (int col = 0; col < columns; ++col)
                vterm_screen_get_cell(screen, {row, col}, &cells[col]);
            const auto* info = vterm_state_get_lineinfo(state, row);
            const auto previous = screenView[row];
            auto updated = line(cells.data(),
                                columns,
                                info && info->continuation,
                                previous ? previous->id : 0,
                                rowTimes[row]);
            if (previous && previous->cells.size() == updated->cells.size()) {
                bool sameText = true;
                for (std::size_t col = 0; col < updated->cells.size(); ++col)
                    if (previous->cells[col].characters != updated->cells[col].characters ||
                        previous->cells[col].width != updated->cells[col].width) {
                        sameText = false;
                        break;
                    }
                if (sameText)
                    updated->timestampMs = previous->timestampMs;
            }
            screenView[row] = std::move(updated);
        }
        auto frame = std::make_shared<TerminalFrame>();
        frame->history = historyView;
        frame->screen = screenView;
        frame->rows = rows;
        frame->columns = columns;
        frame->cursorRow = cursorRow;
        frame->cursorColumn = cursorColumn;
        frame->cursorVisible = visible;
        frame->cursorBlink = blink;
        frame->cursorShape = cursorShape;
        frame->alternateScreen = alternate;
        frame->mouseMode = mouseMode;
        frame->generation = generation;
        frame->revision = ++revision;
        frame->droppedBytes = dropped;
        frame->trimmedLines = trimmed;
        frame->parsedBytes = parsed;
        frame->publishedFrames = ++frames;
        frame->rejectedInputs = rejectedInputs;
        {
            std::lock_guard<std::mutex> lock(mutex);
            frame->queuedBytes = queued;
        }
        std::atomic_store(&published, std::shared_ptr<const TerminalFrame>(frame));
        dirty = false;
        std::fill(dirtyRows.begin(), dirtyRows.end(), false);
        lastPublish = Clock::now();
        if (changed)
            changed();
    }
    void process(Command& c)
    {
        const bool transportCommand = c.kind == Kind::Rx || c.kind == Kind::Key ||
                                      c.kind == Kind::Text || c.kind == Kind::Paste ||
                                      c.kind == Kind::Raw || c.kind == Kind::Mouse ||
                                      c.kind == Kind::Gap;
        if (transportCommand) {
            if (c.generation < generation)
                return;
            if (c.generation > generation) {
                generation = c.generation;
                reset(true);
            }
        }
        switch (c.kind) {
        case Kind::Rx:
            lastReceivedAt = timestamp();
            receiving = true;
            vterm_input_write(vt, reinterpret_cast<const char*>(c.bytes.data()), c.bytes.size());
            vterm_screen_flush_damage(screen);
            receiving = false;
            parsed += c.bytes.size();
            break;
        case Kind::Resize:
            if (rows != c.a || columns != c.b) {
                // libvterm reflows the live screen and calls the bounded scrollback callbacks.
                resizingColumns = columns != c.b;
                rows = c.a;
                columns = c.b;
                rowTimes.resize(rows, 0);
                pushRow = 0;
                markAll();
                vterm_set_size(vt, rows, columns);
                if (resizingColumns)
                    reflowHistory();
                resizingColumns = false;
                screenView.clear();
                markAll();
            }
            break;
        case Kind::Reset:
            reset(true);
            break;
        case Kind::Clear:
            reset(false);
            break;
        case Kind::Key: {
            static const VTermKey keys[] = {VTERM_KEY_ENTER,
                                            VTERM_KEY_TAB,
                                            VTERM_KEY_BACKSPACE,
                                            VTERM_KEY_ESCAPE,
                                            VTERM_KEY_UP,
                                            VTERM_KEY_DOWN,
                                            VTERM_KEY_LEFT,
                                            VTERM_KEY_RIGHT,
                                            VTERM_KEY_INS,
                                            VTERM_KEY_DEL,
                                            VTERM_KEY_HOME,
                                            VTERM_KEY_END,
                                            VTERM_KEY_PAGEUP,
                                            VTERM_KEY_PAGEDOWN};
            const int k = c.a;
            const auto key = k < 14 ? keys[k] : VTermKey(VTERM_KEY_FUNCTION(k - 13));
            vterm_keyboard_key(vt, key, VTermModifier(c.modifiers));
            break;
        }
        case Kind::Text:
            for (const auto ch : c.text)
                vterm_keyboard_unichar(vt, ch, VTermModifier(c.modifiers));
            break;
        case Kind::Paste:
            vterm_keyboard_start_paste(vt);
            if (output && !c.bytes.empty())
                output(generation, c.bytes);
            vterm_keyboard_end_paste(vt);
            break;
        case Kind::Raw:
            if (output && !c.bytes.empty())
                output(generation, c.bytes);
            break;
        case Kind::Mouse:
            vterm_mouse_move(vt, c.a, c.b, VTermModifier(c.modifiers));
            if (c.c)
                vterm_mouse_button(vt, c.c, c.pressed, VTermModifier(c.modifiers));
            break;
        case Kind::Gap:
            dropped += c.lostBytes;
            reset(true);
            break;
        case Kind::Barrier:
            publish();
            c.barrier->set_value();
            break;
        }
    }
    void run()
    {
        for (;;) {
            Command c{Kind::Reset};
            std::uint64_t rejected = 0;
            bool haveCommand = false;
            {
                std::unique_lock<std::mutex> lock(mutex);
                if (commands.empty() && !pendingRejected && !stopping) {
                    if (dirty)
                        ready.wait_until(lock, lastPublish + std::chrono::milliseconds(8));
                    else
                        ready.wait(lock, [this] {
                            return stopping || !commands.empty() || pendingRejected;
                        });
                }
                if (!commands.empty()) {
                    c = std::move(commands.front());
                    commands.pop_front();
                    queued -= c.bytes.size() + c.text.size() * sizeof(char32_t);
                    haveCommand = true;
                } else if (stopping && !pendingRejected)
                    break;
                rejected = pendingRejected;
                pendingRejected = 0;
            }
            if (rejected) {
                rejectedInputs += rejected;
                dirty = true;
            }
            if (haveCommand)
                process(c);
            if (dirty && Clock::now() - lastPublish >= std::chrono::milliseconds(8))
                publish();
        }
        if (dirty)
            publish();
    }
};

TerminalEngine::TerminalEngine(Changed c, Output o)
    : TerminalEngine(Limits{}, std::move(c), std::move(o))
{
}
TerminalEngine::TerminalEngine(Limits l, Changed c, Output o)
    : impl_(std::make_unique<Impl>(l, std::move(c), std::move(o)))
{
}
TerminalEngine::~TerminalEngine() = default;
bool TerminalEngine::receive(std::uint64_t g, const std::vector<std::uint8_t>& b)
{
    Impl::Command c{Impl::Kind::Rx};
    c.generation = g;
    c.bytes = b;
    return impl_->enqueue(std::move(c));
}
void TerminalEngine::resize(int r, int c)
{
    Impl::Command cmd{Impl::Kind::Resize};
    cmd.a = std::clamp(r, 2, 256);
    cmd.b = std::clamp(c, 8, 512);
    impl_->enqueue(std::move(cmd));
}
void TerminalEngine::resetStream()
{
    impl_->enqueue({Impl::Kind::Reset});
}
void TerminalEngine::clear()
{
    impl_->enqueue({Impl::Kind::Clear});
}
void TerminalEngine::key(std::uint64_t g, TerminalInputKey k, int m)
{
    Impl::Command c{Impl::Kind::Key};
    c.generation = g;
    c.a = int(k);
    c.modifiers = m;
    impl_->enqueue(std::move(c));
}
void TerminalEngine::text(std::uint64_t g, const std::u32string& t, int m)
{
    Impl::Command c{Impl::Kind::Text};
    c.generation = g;
    c.text = t;
    c.modifiers = m;
    impl_->enqueue(std::move(c));
}
void TerminalEngine::paste(std::uint64_t g, std::string t)
{
    Impl::Command c{Impl::Kind::Paste};
    c.generation = g;
    c.bytes.assign(t.begin(), t.end());
    impl_->enqueue(std::move(c));
}
void TerminalEngine::rawInput(std::uint64_t g, std::vector<std::uint8_t> b)
{
    Impl::Command c{Impl::Kind::Raw};
    c.generation = g;
    c.bytes = std::move(b);
    impl_->enqueue(std::move(c));
}
void TerminalEngine::mouse(std::uint64_t g, int r, int col, int b, bool pressed, int m)
{
    Impl::Command c{Impl::Kind::Mouse};
    c.generation = g;
    c.a = r;
    c.b = col;
    c.c = b;
    c.pressed = pressed;
    c.modifiers = m;
    impl_->enqueue(std::move(c));
}
std::shared_ptr<const TerminalFrame> TerminalEngine::snapshot() const
{
    return std::atomic_load(&impl_->published);
}
void TerminalEngine::waitIdle()
{
    Impl::Command c{Impl::Kind::Barrier};
    c.barrier = std::make_shared<std::promise<void>>();
    auto f = c.barrier->get_future();
    impl_->enqueue(std::move(c));
    f.wait();
}
} // namespace testtools
