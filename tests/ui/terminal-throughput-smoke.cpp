#include "application/serial/serial_session.h"
#include "application/terminal/terminal_engine.h"
#include "test_paths.h"
#include "ui/widgets/terminal_grid_widget.h"
#include <QtTest/QTest>
#include <QtWidgets>
#include <Windows.h>

#include <Psapi.h>
#include <atomic>
#include <iostream>
#include <thread>
#define private public
#define protected public
#include "ui/main_window.h"
#include "ui/pages/serial_terminal_workspace.h"
#undef protected
#undef private

class Stream final : public serialdemo::ISerialTransport {
  public:
    std::atomic<bool> open_{false};
    std::atomic<int> writes{0};
    std::atomic<bool> echo{false};
    std::atomic<qint64> writtenAtUs{0};
    static qint64 nowUs()
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    void open(const serialdemo::SerialConfig&) override
    {
        open_ = true;
        publishStatus(serialdemo::TransportState::Connected, "replay");
    }
    void close() override
    {
        open_ = false;
    }
    bool isOpen() const override
    {
        return open_;
    }
    bool write(const serialdemo::ByteBuffer& bytes) override
    {
        writtenAtUs = nowUs();
        ++writes;
        if (echo)
            publishData(bytes);
        return open_;
    }
    void receive(const std::string& s)
    {
        publishData({s.begin(), s.end()});
    }
};
class PaintObserver final : public QObject {
    TerminalGridWidget* grid_;
  public:
    std::uint64_t paintedRevision = 0;
    qint64 paintedAtUs = 0;
    explicit PaintObserver(TerminalGridWidget* grid) : grid_(grid)
    {
        grid_->viewport()->installEventFilter(this);
    }
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == grid_->viewport() && event->type() == QEvent::Paint) {
            const auto revision = grid_->frame()->revision;
            QTimer::singleShot(0, this, [this, revision] {
                if (revision > paintedRevision) {
                    paintedRevision = revision;
                    paintedAtUs = Stream::nowUs();
                }
            });
        }
        return false;
    }
};
void check(bool v, const char* m)
{
    if (!v)
        throw std::runtime_error(m);
}
int main(int argc, char** argv)
{
    qputenv("TESTTOOLS_OFFLINE", "1");
    QApplication app(argc, argv);
    app.setStyle("Fusion");
    QTemporaryDir preferences;
    qputenv("TESTTOOLS_DATA_DIR", preferences.path().toUtf8());
    try {
        MainWindow window;
        window.resize(1366, 768);
        window.show();
        QApplication::setActiveWindow(&window);
        window.setSerialModeForDiagnostics("terminal");
        auto* terminal = window.terminalWorkspace_;
        auto* grid = terminal->terminalGrid_;
        auto owned = std::make_unique<Stream>();
        auto* stream = owned.get();
        serialdemo::SerialConfig config;
        config.endpoint = "REPLAY";
        terminal->channels_[0].session.open(std::move(owned), config);
        QTest::qWait(100);
        if (app.arguments().contains("--echo-latency")) {
            PaintObserver observer(grid);
            QJsonObject report;
            stream->echo = true;
            const auto measure = [&](const QString& name) {
                std::vector<qint64> writes, published, painted;
                for (int i = 0; i < 40; ++i) {
                    const auto expected = terminal->terminalEngine_->snapshot()->parsedBytes + 1;
                    const auto started = Stream::nowUs();
                    qint64 publishedAt = 0;
                    std::uint64_t targetRevision = 0;
                    QTest::keyClick(grid, Qt::Key_X);
                    while (Stream::nowUs() - started < 3000000) {
                        QApplication::processEvents(QEventLoop::AllEvents, 1);
                        const auto frame = terminal->terminalEngine_->snapshot();
                        if (!targetRevision && frame->parsedBytes >= expected) {
                            targetRevision = frame->revision;
                            publishedAt = Stream::nowUs();
                        }
                        if (targetRevision && observer.paintedRevision >= targetRevision &&
                            grid->frame()->parsedBytes >= expected)
                            break;
                        std::this_thread::yield();
                    }
                    check(targetRevision && observer.paintedRevision >= targetRevision &&
                              grid->frame()->parsedBytes >= expected,
                          "mock echo reaches a completed viewport paint");
                    writes.push_back(stream->writtenAtUs - started);
                    published.push_back(publishedAt - started);
                    painted.push_back(observer.paintedAtUs - started);
                    QTest::qWait(10);
                }
                const auto distribution = [](std::vector<qint64> values) {
                    std::sort(values.begin(), values.end());
                    return QJsonObject{{"p50_us", values[values.size()/2]},
                                       {"p95_us", values[values.size()*95/100]},
                                       {"max_us", values.back()}};
                };
                report[name] = QJsonObject{{"samples", 40},
                                           {"input_to_mock_write", distribution(writes)},
                                           {"input_to_snapshot", distribution(published)},
                                           {"input_to_completed_paint", distribution(painted)}};
            };
            measure("prompt_echo");
            stream->echo = false;
            std::string history;
            for (int i = 0; i < 4000; ++i)
                history += "\r\n[history] 中文 0123456789012345678901234567890123456789 ERROR warning";
            const auto expected = terminal->terminalEngine_->snapshot()->parsedBytes + history.size();
            stream->receive(history);
            QElapsedTimer ready;
            ready.start();
            while (grid->frame()->parsedBytes < expected && ready.elapsed() < 6000)
                QTest::qWait(5);
            check(grid->frame()->parsedBytes >= expected, "retained history ready for echo");
            QTest::qWait(100);
            stream->echo = true;
            measure("history_echo");
            report["dpr"] = window.devicePixelRatioF();
            report["logical_width"] = window.width();
            report["logical_height"] = window.height();
#ifdef QT_DEBUG
            report["configuration"] = "Debug";
#else
            report["configuration"] = "Release";
#endif
            report["scope"] = "host mock echo, no driver/wire/board latency; paint observed after Paint event";
            report["sampling"] = "yielding event pump; no sleeping UI polling; first completed paint for revision";
            QFile output(testOutputPath("terminal-echo-latency.json"));
            check(output.open(QIODevice::WriteOnly), "echo latency evidence");
            output.write(QJsonDocument(report).toJson());
            std::cout << QJsonDocument(report).toJson(QJsonDocument::Compact).constData() << '\n';
            window.close();
            QTest::qWait(200);
            return 0;
        }
        if (app.arguments().contains("--interaction-only")) {
            // No RX during these samples: typing and exposing the page must stay cheap
            // even when a terminal already contains a full screen of retained output.
            QJsonObject report;
            const auto measure = [&](const QString& name) {
                std::vector<qint64> paints, inputs, pages;
                for (int i = 0; i < 32; ++i) {
                    QElapsedTimer clock;
                    clock.start();
                    grid->viewport()->repaint();
                    paints.push_back(clock.nsecsElapsed() / 1000);
                    const int before = stream->writes;
                    clock.restart();
                    const auto started = Stream::nowUs();
                    QTest::keyClick(grid, Qt::Key_X);
                    while (stream->writes == before && clock.elapsed() < 1000) {
                        QApplication::processEvents(QEventLoop::AllEvents, 1);
                        std::this_thread::yield();
                    }
                    check(stream->writes == before + 1, "idle key reaches mock transport");
                    inputs.push_back(stream->writtenAtUs - started);
                    if (i % 4 == 0) {
                        clock.restart();
                        window.navigateTo("adb");
                        window.navigateTo("terminal");
                        QApplication::processEvents();
                        pages.push_back(clock.nsecsElapsed() / 1000);
                    }
                    QTest::qWait(5);
                }
                std::sort(paints.begin(), paints.end());
                std::sort(inputs.begin(), inputs.end());
                std::sort(pages.begin(), pages.end());
                report[name] = QJsonObject{{"paint_p50_us", paints[paints.size()/2]},
                                           {"paint_max_us", paints.back()},
                                           {"input_to_mock_write_p50_us", inputs[inputs.size()/2]},
                                           {"input_to_mock_write_max_us", inputs.back()},
                                           {"page_roundtrip_max_us", pages.back()}};
            };
            measure("empty_idle");
            std::string retained;
            for (int i = 0; i < 50; ++i)
                retained += "\x1b[32m[retained]\x1b[0m 中文 0123456789012345678901234567890123456789 ERROR warning\r\n";
            stream->receive(retained);
            QElapsedTimer ready;
            ready.start();
            while (grid->frame()->parsedBytes != retained.size() && ready.elapsed() < 3000)
                QTest::qWait(5);
            check(grid->frame()->parsedBytes == retained.size(), "retained screen delivered");
            measure("retained_idle");
            report["dpr"] = window.devicePixelRatioF();
            report["font_pixel_size"] = grid->font().pixelSize();
            report["input_sampling"] = "mock write timestamp; yielding event pump with no sleeping input polling";
#ifdef QT_DEBUG
            report["configuration"] = "Debug";
#else
            report["configuration"] = "Release";
#endif
            QFile output(testOutputPath("terminal-interaction.json"));
            check(output.open(QIODevice::WriteOnly), "idle interaction evidence");
            output.write(QJsonDocument(report).toJson());
            std::cout << QJsonDocument(report).toJson(QJsonDocument::Compact).constData() << '\n';
            window.close();
            QTest::qWait(200);
            return 0;
        }
        std::string packet;
        for (int i = 0; i < 24; ++i)
            packet += "\x1b[32m[replay]\x1b[0m "
                      "0123456789012345678901234567890123456789012345678901234567890123456789 中文 "
                      "ERROR warning\r\n";
        const int durationMs = 15000;
        // 8N1: 1.5 Mbit/s wire rate == 150000 payload bytes/s.
        const auto delay = std::chrono::microseconds(packet.size() * 1000000 / 150000);
        std::atomic<bool> done{false};
        std::atomic<std::uint64_t> bytes{0};
        std::thread producer([&] {
            const auto end =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(durationMs);
            auto next = std::chrono::steady_clock::now();
            while (std::chrono::steady_clock::now() < end) {
                stream->receive(packet);
                bytes += packet.size();
                next += delay;
                std::this_thread::sleep_until(next);
            }
            done = true;
        });
        QElapsedTimer elapsed;
        elapsed.start();
        qint64 previous = 0;
        std::vector<qint64> gaps;
        std::uint64_t peakWorkingSet = 0;
        QTimer probe;
        probe.setTimerType(Qt::PreciseTimer);
        probe.setInterval(10);
        QObject::connect(&probe, &QTimer::timeout, [&] {
            const qint64 now = elapsed.nsecsElapsed() / 1000;
            if (previous)
                gaps.push_back(now - previous);
            previous = now;
            PROCESS_MEMORY_COUNTERS memory{};
            if (K32GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory)))
                peakWorkingSet = std::max(peakWorkingSet, std::uint64_t(memory.WorkingSetSize));
        });
        probe.start();
        int sent = 0;
        while (!done) {
            QTest::qWait(200);
            if (elapsed.elapsed() < 12000) {
                grid->setFocus();
                QTest::keyClick(grid, Qt::Key_X);
                ++sent;
            } else if (!terminal->channels_[0].searchButton->isChecked()) {
                terminal->channels_[0].searchButton->setChecked(true);
                terminal->channels_[0].searchEdit->setText("warning");
            }
            if (sent == 15)
                terminal->channels_[0].highlightButton->setChecked(true);
        }
        producer.join();
        const qint64 replayMs = elapsed.elapsed();
        QElapsedTimer settle;
        settle.start();
        while (settle.elapsed() < 4000 &&
               (grid->frame()->parsedBytes != bytes || stream->writes != sent))
            QTest::qWait(10);
        probe.stop();
        const auto frame = grid->frame();
        check(frame->parsedBytes == bytes && frame->droppedBytes == 0,
              "replay parsed completely without gaps");
        check(stream->writes == sent, "keyboard remains responsive during replay");
        check(frame->history->size() <= 20000 && frame->trimmedLines > 0,
              "bounded scrollback eviction reached");
        std::sort(gaps.begin(), gaps.end());
        const auto percentile = [&](double p) {
            return gaps[std::min(gaps.size() - 1, std::size_t(p * gaps.size()))];
        };
        const auto maximum = gaps.back();
        QJsonObject report{{"font_family", grid->font().family()},
                           {"font_pixel_size", grid->font().pixelSize()},
                           {"grid_columns", grid->gridColumns()},
                           {"font_m_width", QFontMetricsF(grid->font()).horizontalAdvance('M')},
                           {"wire_bits_per_second", 1500000},
                           {"payload_bytes", qint64(bytes.load())},
                           {"replay_ms", replayMs},
                           {"probe_interval_ms", 10},
                           {"gui_gap_p50_us", percentile(.5)},
                           {"gui_gap_p95_us", percentile(.95)},
                           {"gui_gap_p99_us", percentile(.99)},
                           {"gui_gap_max_us", maximum},
                           {"maximum_paint_us", grid->maximumPaintMicroseconds()},
                           {"paint_count", qint64(grid->paintCount())},
                           {"published_frames", qint64(frame->publishedFrames)},
                           {"peak_working_set_bytes", qint64(peakWorkingSet)},
                           {"history_lines", qint64(frame->history->size())},
                           {"trimmed_lines", qint64(frame->trimmedLines)},
                           {"keyboard_events", sent},
                           {"search_open_during_last_3s", true},
                           {"dropped_bytes", qint64(frame->droppedBytes)}};
        QFile output(testOutputPath("terminal-throughput.json"));
        check(output.open(QIODevice::WriteOnly), "performance evidence");
        output.write(QJsonDocument(report).toJson());
        window.grab().save(testOutputPath("terminal-throughput-1366.png"));
        check(maximum < 500000, "GUI heartbeat remains below 500ms during bounded host replay");
        check(peakWorkingSet < 600ull * 1024 * 1024, "bounded process working set under 600MiB");
        std::cout << QJsonDocument(report).toJson(QJsonDocument::Compact).constData() << '\n';
        window.close();
        QTest::qWait(200);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
