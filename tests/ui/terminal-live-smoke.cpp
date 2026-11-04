// Explicit opt-in hardware check. Not registered in CTest.
#include "application/serial/serial_session.h"
#include "application/terminal/terminal_engine.h"
#include "ui/widgets/terminal_grid_widget.h"
#include <QtTest/QTest>
#include <QtWidgets>
#include <iostream>
#define private public
#define protected public
#include "ui/main_window.h"
#include "ui/pages/serial_terminal_workspace.h"
#undef protected
#undef private

void check(bool v, const char* m)
{
    if (!v)
        throw std::runtime_error(m);
}
bool await(const std::function<bool()>& predicate, int timeout = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeout) {
        if (predicate())
            return true;
        QTest::qWait(10);
    }
    return predicate();
}
int main(int argc, char** argv)
{
    if (argc != 4) {
        std::cerr << "Usage: terminal-live-smoke COM3 115200 evidence-directory\n";
        return 2;
    }
    qputenv("TESTTOOLS_OFFLINE", "1");
    QApplication app(argc, argv);
    app.setStyle("Fusion");
    QTemporaryDir preferences;
    qputenv("TESTTOOLS_DATA_DIR", preferences.path().toUtf8());
    const QString evidence = QString::fromLocal8Bit(argv[3]);
    QDir().mkpath(evidence);
    MainWindow window;
    window.resize(1366, 768);
    window.show();
    QApplication::setActiveWindow(&window);
    auto* terminal = window.terminalWorkspace_;
    auto& channel = terminal->channels_[0];
    auto* grid = terminal->terminalGrid_;
    bool sizeSaved = false, topChecked = false, viChecked = false;
    QStringList commands;
    const auto command = [&](const QString& text) {
        commands.push_back(text);
        grid->clearSelection();
        grid->setFocus();
        QTest::keyClicks(grid, text);
        QTest::keyClick(grid, Qt::Key_Return);
        QTest::qWait(150);
    };
    const auto marker = [&](const QString& value) {
        return await([&] { return grid->toPlainText().split('\n').contains(value); });
    };
    const auto restore = [&] {
        if (sizeSaved && channel.session.isOpen()) {
            command("stty \"$TT_T1_OLD\"; stty rows \"$TT_T1_ROWS\" cols \"$TT_T1_COLS\"; unset "
                    "TT_T1_OLD TT_T1_ROWS TT_T1_COLS; printf '__TT_RESTORED__\\n'");
            sizeSaved = false;
            check(marker("__TT_RESTORED__"), "restore terminal settings");
        }
    };
    int result = 1;
    try {
        check(window.setSerialModeForDiagnostics("terminal"), "terminal page");
        channel.sourceCombo->setCurrentIndex(0);
        channel.portCombo->setEditText(QString::fromLocal8Bit(argv[1]));
        channel.baudCombo->setCurrentText(QString::fromLocal8Bit(argv[2]));
        channel.connectButton->click();
        check(await([&] { return channel.session.isOpen(); }), "open COM");
        check(await([&] { return window.focusWidget() == grid; }), "connect autofocus");
        testtools::SerialRecordOptions recording;
        recording.path = std::filesystem::path(evidence.toStdWString()) / L"com3-rx.bin";
        recording.format = testtools::SerialRecordFormat::RawRx;
        recording.overwrite = true;
        std::string error;
        check(channel.session.startRecording(recording, channel.session.generation(), error),
              "start raw RX recorder");
        QTest::keyClick(grid, Qt::Key_Return);
        QTest::qWait(3000);
        std::cout << "OPEN TX=" << channel.session.snapshot().sentBytes
                  << " RX=" << channel.session.snapshot().receivedBytes
                  << " grid=" << grid->gridColumns() << "x" << grid->gridRows() << std::endl;
        QFile initial(evidence + "/initial-screen.txt");
        check(initial.open(QIODevice::WriteOnly), "initial transcript");
        initial.write(grid->toPlainText().toUtf8());
        initial.close();
        const QRegularExpression prompt(QStringLiteral("(?:^|\\n)[^\\n]{0,120}[#$] *$"));
        check(prompt.match(grid->toPlainText()).hasMatch(),
              "shell prompt observed before commands");
        command(
            "printf '__TT_READY__\\n'; uname -s; command -v stty; command -v top; command -v vi");
        check(marker("__TT_READY__"), "shell printf echo roundtrip");
        const auto capabilities = grid->toPlainText();
        command("TT_T1_OLD=$(stty -g); set -- $(stty size); TT_T1_ROWS=$1; TT_T1_COLS=$2; printf "
                "'__TT_SIZE__%s,%s\\n' \"$1\" \"$2\"");
        check(await([&] {
                  return QRegularExpression("(?:^|\\n)__TT_SIZE__[0-9]+,[0-9]+")
                      .match(grid->toPlainText())
                      .hasMatch();
              }),
              "read original tty size");
        sizeSaved = true;
        command(QStringLiteral("stty rows %1 cols %2; printf '__TT_RESIZED__\\n'")
                    .arg(grid->gridRows())
                    .arg(grid->gridColumns()));
        check(marker("__TT_RESIZED__"), "explicit tty resize");
        // A real UART-delivered alternate-screen fixture, including CJK and truecolor.
        command("printf '\\033[?1049h\\033[2J\\033[H\\033[38;2;100;210;180mT1 UART alternate "
                "screen\\033["
                "0m\\n\\342\\224\\214\\342\\224\\200\\342\\224\\200\\342\\224\\200\\342\\224\\220\\"
                "n\\344\\270\\255\\346\\226\\207\\n'");
        check(await([&] { return grid->frame()->alternateScreen; }),
              "real alternate screen entered");
        window.grab().save(evidence + "/com3-alternate-1366.png");
        command("printf '\\033[?1049l__TT_PRIMARY__\\n'");
        check(marker("__TT_PRIMARY__"), "real primary restored");
        if (QRegularExpression("(?:^|\\n)/[^\\n]*/top(?:\\n|$)").match(capabilities).hasMatch()) {
            command("TERM=xterm-256color top");
            QTest::qWait(1300);
            window.grab().save(evidence + "/com3-top-1366.png");
            QTest::keyClick(grid, Qt::Key_C, Qt::ControlModifier);
            QTest::qWait(400);
            command("printf '__TT_TOP_RETURN__\\n'");
            check(marker("__TT_TOP_RETURN__"), "top interrupt returns shell");
            topChecked = true;
        }
        if (QRegularExpression("(?:^|\\n)/[^\\n]*/vi(?:\\n|$)").match(capabilities).hasMatch()) {
            command("TERM=xterm-256color vi -R /proc/version");
            QTest::qWait(700);
            window.grab().save(evidence + "/com3-vi-1366.png");
            QTest::keyClick(grid, Qt::Key_Escape);
            command(":q!");
            command("printf '__TT_VI_RETURN__\\n'");
            check(marker("__TT_VI_RETURN__"), "vi read-only exit returns shell");
            viChecked = true;
        }
        command("sleep 10");
        QTest::qWait(150);
        QTest::keyClick(grid, Qt::Key_C, Qt::ControlModifier);
        QTest::qWait(250);
        command("printf '__TT_INTERRUPT__\\n'");
        check(marker("__TT_INTERRUPT__"), "Ctrl+C interrupts sleep");
        QTest::keyClicks(grid, "printf '__TT_");
        QInputMethodEvent preedit(QStringLiteral("zhongwen"), {});
        QApplication::sendEvent(grid, &preedit);
        QInputMethodEvent commit;
        commit.setCommitString(QStringLiteral("中文"));
        QApplication::sendEvent(grid, &commit);
        QTest::keyClicks(grid, "__\\n'");
        QTest::keyClick(grid, Qt::Key_Return);
        check(marker(QStringLiteral("__TT_中文__")), "programmatic IME commit roundtrip");
        restore();
        const auto snapshot = channel.session.snapshot();
        check(grid->frame()->droppedBytes == 0, "no display queue gaps");
        window.grab().save(evidence + "/com3-final-1366.png");
        QFile text(evidence + "/com3-screen.txt");
        check(text.open(QIODevice::WriteOnly), "save screen");
        text.write(grid->toPlainText().toUtf8());
        std::cout << "PASS COM " << argv[1] << " baud=" << argv[2]
                  << " RX=" << snapshot.receivedBytes << " TX=" << snapshot.sentBytes
                  << " drops=0; shell/alternate/interrupt/programmatic-IME-commit; top="
                  << topChecked << " vi=" << viChecked << "; settings restored\n";
        result = 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        window.grab().save(evidence + "/com3-failure.png");
        try {
            restore();
        } catch (...) {
            std::cerr << "TTY settings restore not confirmed\n";
        }
    }
    QFile script(evidence + "/com3-commands.txt");
    if (script.open(QIODevice::WriteOnly))
        script.write(commands.join('\n').toUtf8());
    channel.session.stopRecording("T1 verification finished");
    check(await([&] { return !channel.session.recordingSnapshot().busy; }), "drain raw recorder");
    if (channel.session.isActive())
        channel.connectButton->click();
    check(await([&] { return !channel.session.isActive() && !channel.disconnectPending; }),
          "COM released");
    window.close();
    QTest::qWait(200);
    return result;
}
