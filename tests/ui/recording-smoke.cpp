#include "application/serial/serial_session.h"
#include "application/serial/serial_receive_queue.h"
#include "application/serial/serial_terminal.h"
#include "application/serial/serial_workspace_state.h"
#include "application/device/file_transfer_service.h"
#include "ui/widgets/serial_keyword_highlighter.h"
#include "test_paths.h"
#include <QtTest/QTest>
#include <QtWidgets>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QCryptographicHash>
#include <atomic>
#include <iostream>
#define private public
#include "ui/main_window.h"
#include "ui/pages/serial_workspace.h"
#include "ui/pages/file_transfer_workspace.h"
#undef private

namespace {
bool highDpiOnly = false;
void check(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
bool await(const std::function<bool()>& predicate, int timeout = 6000)
{
    QElapsedTimer timer; timer.start();
    while (timer.elapsed() < timeout) { if (predicate()) return true; QTest::qWait(5); }
    return predicate();
}
class Transport final : public serialdemo::ISerialTransport {
  public:
    std::atomic<bool> opened{false};
    void open(const serialdemo::SerialConfig&) override
    { opened = true; publishStatus(serialdemo::TransportState::Connected, "已连接（离线夹具）"); }
    void close() override { opened = false; }
    bool write(const serialdemo::ByteBuffer&) override { return opened; }
    bool isOpen() const override { return opened; }
    void receive(const std::string& text) { publishData({text.begin(), text.end()}); }
};
void capture(MainWindow& window, const QString& name, const QString& scenario)
{
    if (!qEnvironmentVariableIsSet("TESTTOOLS_RECORDING_SNAPSHOTS")) return;
    if (highDpiOnly && name != QStringLiteral("recording-terminal-high-dpi")) return;
    QTest::qWait(100);
    const auto pixmap = window.grab();
    const auto path = testOutputPath(name + ".png");
    check(pixmap.save(path), "save screenshot");
    QJsonObject info{{"scenario", scenario}, {"fixture", true},
        {"logical_width", window.width()}, {"logical_height", window.height()},
        {"pixel_width", pixmap.width()}, {"pixel_height", pixmap.height()},
        {"device_pixel_ratio", pixmap.devicePixelRatio()},
        {"requested_scale", qEnvironmentVariable("QT_SCALE_FACTOR")}};
    QFile metadata(testOutputPath(name + ".json"));
    check(metadata.open(QIODevice::WriteOnly) && metadata.write(QJsonDocument(info).toJson()) > 0, "save metadata");
}
int highlightCount(QPlainTextEdit* edit, const QColor& background)
{
    int count = 0;
    for (auto block = edit->document()->begin(); block.isValid(); block = block.next())
        for (const auto& range : block.layout()->formats())
            if (range.format.background().color() == background) ++count;
    return count;
}
QByteArray read(const std::filesystem::path& path)
{
    QFile file(QString::fromStdWString(path.wstring()));
    check(file.open(QIODevice::ReadOnly), "read recording"); return file.readAll();
}
class Files final : public testtools::IFileTransferBackend {
  public:
    testtools::FileListing list(const std::string&, const std::string& directory, const std::atomic<bool>&) override
    { return {directory, {}, {}}; }
    testtools::FileTransferResult transfer(testtools::TransferDirection, const std::string&, const std::filesystem::path&,
        const std::string&, testtools::FileKind, bool, const std::atomic<bool>&, const testtools::FileProgress&) override
    { return {testtools::TransferOutcome::Completed, "fixture"}; }
};
}
int main(int argc, char** argv)
{
    try {
        qputenv("TESTTOOLS_OFFLINE", "1");
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        QApplication app(argc, argv); app.setStyle("Fusion");
        highDpiOnly = argc > 1 && std::string(argv[1]) == "--high-dpi";
        app.setQuitOnLastWindowClosed(false);
        QTemporaryDir storage;
        check(storage.isValid(), "isolated storage");
        qputenv("TESTTOOLS_DATA_DIR", storage.path().toUtf8());
        QTimer::singleShot(50000, [] { std::cerr << "recording fixture timed out\n"; std::exit(3); });
        MainWindow window; window.resize(1366, 768); window.show();
        QApplication::setActiveWindow(&window);
        auto* normal = window.serialWorkspace_;
        auto& channel = normal->channels_[1];
        channel.portCombo->setEditText("COM_FIXTURE");
        auto owned = std::make_unique<Transport>(); auto* transport = owned.get();
        serialdemo::SerialConfig config; config.endpoint = "COM_FIXTURE";
        const auto generation = channel.session.open(std::move(owned), config);
        check(await([&] { return channel.recordButton->isEnabled(); }), "record enabled after connection");
        const auto rawPath = std::filesystem::path(storage.path().toStdWString()) / L"连续录制.bin";
        bool selectedDialog = false;
        QTimer::singleShot(50, [&] {
            for (auto* widget : QApplication::topLevelWidgets()) if (auto* dialog = qobject_cast<QFileDialog*>(widget)) {
                dialog->selectNameFilter(QStringLiteral("原始 RX 字节 (*.bin)"));
                dialog->selectFile(QString::fromStdWString(rawPath.wstring()));
                selectedDialog = true; QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
            }
        });
        channel.recordButton->click();
        check(selectedDialog && await([&] { return channel.session.recordingSnapshot().state == testtools::SerialRecordState::Recording; }),
              "record button opens format/path dialog and starts");
        const std::string initial = "Board boot complete\nERROR sensor init failed\nWARN temperature high\npanic trace saved\n";
        transport->receive(initial);
        check(await([&] { return channel.receiveEdit->toPlainText().contains("ERROR"); }), "render received text");
        const auto source = channel.receiveEdit->toPlainText();
        const auto originalBytes = channel.logBuffer.totalBytes();
        channel.highlightButton->click();
        check(await([&] { return highlightCount(channel.receiveEdit, QColor("#F7E9E8")) == 3; }), "error keywords overlay including lowercase failed");
        check(highlightCount(channel.receiveEdit, QColor("#F6EFE2")) == 1, "warning keywords overlay");
        channel.searchButton->click(); channel.searchEdit->setText("ERROR");
        check(await([&] { return !channel.receiveEdit->extraSelections().empty(); }) &&
              highlightCount(channel.receiveEdit, QColor("#F7E9E8")) == 3, "search and keyword layers coexist");
        check(channel.receiveEdit->toPlainText() == source && channel.logBuffer.totalBytes() == originalBytes &&
              channel.session.snapshot().sentBytes == 0,
              "highlight leaves source and plain export unchanged");
        check(await([&] { return window.taskButton_->isEnabled() && window.taskButton_->text().contains('1'); }), "global task snapshot");
        capture(window, "recording-search", "单串口录制中，关键字与搜索叠加");
        window.resize(1120, 768); QTest::qWait(100);
        check(channel.recordButton->isVisible() && channel.recordButton->isEnabled() &&
              channel.recordButton->geometry().right() < channel.recordButton->parentWidget()->width(), "narrow stop remains visible");
        capture(window, "recording-narrow", "最窄窗口保留停止录制，侧栏自动收起");
        window.resize(1366, 768);
        channel.clearReceiveButton->click();
        check(channel.session.recordingSnapshot().busy, "clear retains recording");
        std::string flood;
        const std::string chunk(768, 'r');
        for (int i = 0; i < 2000; ++i) { transport->receive(chunk); flood += chunk; }
        check(await([&] { return channel.displayDroppedBytes > 0 && channel.truncatedLabel->isVisible(); }), "bounded delivery loss visible");
        check(channel.session.recordingSnapshot().busy, "display overflow does not stop recorder");
        const auto oldTask = normal->activeTasks().front();
        check(window.navigateTo("device"), "switch page while recording");
        window.taskMenu_->popup(window.taskButton_->mapToGlobal(QPoint(0, 0)));
        check(!window.taskMenu_->actions().empty() && window.taskMenu_->actions().front()->text().contains(QStringLiteral("停止录制")),
              "global menu explicitly stops recording");
        window.taskMenu_->actions().front()->trigger(); window.taskMenu_->close();
        check(await([&] { return !channel.session.recordingSnapshot().busy; }) && channel.session.isOpen(), "global stop flushes, keeps connection");
        const auto actual = read(rawPath);
        const auto expected = QByteArray::fromStdString(initial + flood);
        check(actual == expected, "all raw bytes exact despite clear/filter/display loss");
        if (qEnvironmentVariableIsSet("TESTTOOLS_RECORDING_SNAPSHOTS")) {
            QFile output(testOutputPath("recording-fixture-raw.bin"));
            check(output.open(QIODevice::WriteOnly) && output.write(actual) == actual.size(), "retain raw evidence");
            QFile metadata(testOutputPath("recording-fixture-raw.bin.json"));
            const auto sourceMeta = read(testtools::SerialRecorder::metadataPath(rawPath));
            check(metadata.open(QIODevice::WriteOnly) && metadata.write(sourceMeta) == sourceMeta.size(), "retain raw metadata");
            QFile verified(testOutputPath("recording-fixture-verification.json"));
            QJsonObject result{{"fixture", true}, {"bytes", double(actual.size())},
                {"expected_sha256", QString::fromLatin1(QCryptographicHash::hash(expected, QCryptographicHash::Sha256).toHex())},
                {"actual_sha256", QString::fromLatin1(QCryptographicHash::hash(actual, QCryptographicHash::Sha256).toHex())},
                {"display_dropped_bytes", double(channel.displayDroppedBytes)}, {"exact_match", actual == expected}};
            check(verified.open(QIODevice::WriteOnly) && verified.write(QJsonDocument(result).toJson()) > 0, "retain byte check");
        }

        testtools::SerialRecordOptions options{std::filesystem::path(storage.path().toStdWString()) / L"missing" / L"fail.bin",
                                              testtools::SerialRecordFormat::RawRx};
        std::string error;
        check(channel.session.startRecording(options, generation, error), "start bad path asynchronously");
        check(await([&] { return !channel.session.recordingSnapshot().busy && channel.recordButton->text().contains(QStringLiteral("失败")); }), "persistent failure UI");
        window.navigateTo("serial");
        capture(window, "recording-failure", "打开文件失败，串口仍连接，错误入口保持可见");
        options.path = std::filesystem::path(storage.path().toStdWString()) / L"next.bin";
        check(channel.session.startRecording(options, generation, error), "restart after failure");
        normal->stopTask(oldTask);
        check(channel.session.recordingSnapshot().busy, "old menu action cannot stop next recording");
        const auto reusedId = channel.session.recordingSnapshot().id;
        check(normal->selectWorkspaceMode("dual") && channel.session.recordingSnapshot().id == reusedId &&
              channel.session.recordingSnapshot().busy, "same B recording survives single to dual");
        check(await([&] { return channel.recordButton->text().contains(QStringLiteral("停止录制")); }), "dual active stop layout");
        capture(window, "recording-dual", "双端录制中与高亮控件，计数独立一行");
        channel.recordButton->click();
        check(await([&] { return !channel.session.recordingSnapshot().busy; }), "toolbar stop drains");

        options.path = std::filesystem::path(storage.path().toStdWString()) / L"dual.bin";
        check(channel.session.startRecording(options, generation, error), "dual record");
        serialdemo::PeriodicSendConfig scheduled;
        check(channel.session.startScheduledSend({'Q'}, scheduled), "scheduled task for aggregation");
        auto tasks = normal->activeTasks();
        check(tasks.size() == 2, "record and scheduled send independently counted");
        const auto scheduledTask = tasks.back(); normal->stopTask(scheduledTask);
        check(!channel.session.snapshot().scheduledSend.running && channel.session.recordingSnapshot().busy, "named stop only affects target task");
        check(channel.session.startScheduledSend({'Q'}, scheduled), "new scheduled task"); normal->stopTask(scheduledTask);
        check(channel.session.snapshot().scheduledSend.running, "stale scheduled stop refused");
        normal->stopTask(normal->activeTasks().back());

        auto* terminal = window.terminalWorkspace_;
        auto& term = terminal->channels_[1];
        auto termOwned = std::make_unique<Transport>(); auto* termTransport = termOwned.get();
        config.endpoint = "COM_TERMINAL";
        const auto termGeneration = term.session.open(std::move(termOwned), config);
        term.portCombo->setEditText("COM_TERMINAL");
        check(window.setSerialModeForDiagnostics("terminal"), "terminal page");
        termTransport->receive("\x1b[32mERROR\x1b[0m boot warning\nERROR second block\n");
        check(await([&] { return term.receiveEdit->toPlainText().contains("ERROR"); }), "ANSI fixture rendered");
        QTextCursor ansi(term.receiveEdit->document()); ansi.setPosition(1);
        const auto ansiColor = ansi.charFormat().foreground().color();
        const auto termSource = terminal->terminalBuffer_.plainText();
        term.highlightButton->setChecked(true);
        check(await([&] { return highlightCount(term.receiveEdit, QColor("#F7E9E8")) == 2; }), "terminal incremental keywords");
        term.searchButton->click(); term.searchEdit->setText("ERROR");
        check(!term.receiveEdit->extraSelections().empty(), "terminal search overlay");
        term.highlightButton->setChecked(false);
        check(await([&] { return highlightCount(term.receiveEdit, QColor("#F7E9E8")) == 0; }) &&
              ansi.charFormat().foreground().color() == ansiColor && terminal->terminalBuffer_.plainText() == termSource,
              "off restores ANSI, parser text unchanged");
        term.highlightButton->setChecked(true);
        testtools::SerialWorkspacePreferences prefs;
        check(testtools::SerialPreferencesStore::load(terminal->preferencesStorePath_, prefs, error) && prefs.channels[1].highlightKeywords,
              "highlight preference persisted independently");
        options.path = std::filesystem::path(storage.path().toStdWString()) / L"terminal.bin";
        check(term.session.startRecording(options, termGeneration, error), "terminal record");
        check(await([&] { return term.recordButton->text().contains(QStringLiteral("停止录制")) &&
            window.taskButton_->text().contains('2'); }), "terminal recording controls reflect snapshot");
        if (argc > 1 && std::string(argv[1]) == "--high-dpi") {
            window.resize(1920, 1080);
            capture(window, "recording-terminal-high-dpi", "宽屏和高 DPI，ANSI、搜索与关键字叠加");
        }
        // 使用真实服务快照（后端为离线夹具）验证文件任务与串口任务同栏出现、单独取消。
        auto files = std::make_shared<testtools::FileTransferService>(std::make_shared<Files>());
        window.fileWorkspace_->service_ = files;
        testtools::FileTransferRequest request; request.device = {"FILE_FIXTURE", "device", ""};
        request.localDirectory = std::filesystem::path(storage.path().toStdWString());
        request.remoteDirectory = "/tmp"; request.entries.push_back({"a.log", testtools::FileKind::Regular});
        check(files->prepare(request, error), "file fixture prepared");
        check(await([&] { return window.taskButton_->text().contains('3'); }), "three independent tasks aggregated");
        window.taskMenu_->popup(window.taskButton_->mapToGlobal(QPoint()));
        for (auto* action : window.taskMenu_->actions()) if (action->text().contains(QStringLiteral("取消传输"))) action->trigger();
        window.taskMenu_->close();
        check(files->snapshot().cancelling && channel.session.recordingSnapshot().busy && term.session.recordingSnapshot().busy,
              "file cancel does not stop recordings");
        files->executePrepared();
        termTransport->receive("tail before close\n");
        window.close();
        check(await([&] { return !window.isVisible(); }) && !normal->hasPendingRecordings() && !terminal->hasPendingRecordings(),
              "window close waits asynchronously for all recording files");
        check(read(options.path).endsWith("tail before close\n"), "close persists terminal tail");
        std::cout << "PASS recording dialog/stop/clear/overflow/failure/tasks/exit; keyword/search/ANSI/read-only preferences\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
