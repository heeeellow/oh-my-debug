#include "test_paths.h"
#include <QtTest/QTest>
#include <QtWidgets>
#include <iostream>
#include <windows.h>
struct Find {
    DWORD pid;
    HWND window = nullptr;
};
BOOL CALLBACK findWindow(HWND w, LPARAM data)
{
    auto* f = reinterpret_cast<Find*>(data);
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid == f->pid && IsWindowVisible(w)) {
        f->window = w;
        return FALSE;
    }
    return TRUE;
}
void check(bool b, const char* m)
{
    if (!b)
        throw std::runtime_error(m);
}
int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QProcess process;
    try {
        check(argc == 4, "scrcpy, adb, serial required");
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("ADB", QString::fromLocal8Bit(argv[2]));
        environment.insert(
            "SCRCPY_SERVER_PATH",
            QFileInfo(QString::fromLocal8Bit(argv[1])).dir().filePath("scrcpy-server"));
        process.setProcessEnvironment(environment);
        process.setWorkingDirectory(QFileInfo(QString::fromLocal8Bit(argv[1])).absolutePath());
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.start(QString::fromLocal8Bit(argv[1]),
                      {"--serial=" + QString::fromLocal8Bit(argv[3]),
                       "--window-title=TestTools-0.7-host-probe",
                       "--no-audio",
                       "--no-clipboard-autosync",
                       "--max-size=1280",
                       "--max-fps=30"});
        check(process.waitForStarted(3000), "start owned scrcpy");
        Find found{DWORD(process.processId())};
        for (int i = 0; i < 100 && !found.window; ++i) {
            EnumWindows(findWindow, reinterpret_cast<LPARAM>(&found));
            QTest::qWait(50);
        }
        check(found.window, "owned SDL window found");
        QWidget host;
        host.setWindowTitle("TestTools native host probe");
        host.resize(900, 650);
        auto* layout = new QVBoxLayout(&host);
        auto* foreign = QWindow::fromWinId(WId(found.window));
        check(foreign, "foreign window wrapper");
        auto* container = QWidget::createWindowContainer(foreign, &host);
        layout->addWidget(container);
        host.show();
        QTest::qWait(900);
        check(GetParent(found.window) != nullptr, "SDL window parented");
        RECT before{};
        GetClientRect(found.window, &before);
        host.resize(750, 550);
        QTest::qWait(400);
        RECT after{};
        GetClientRect(found.window, &after);
        check(before.right != after.right && after.right > 0, "native client follows Qt resize");
        app.primaryScreen()
            ->grabWindow(WId(host.winId()))
            .save(testOutputPath("mirror-host-probe.png"));
        foreign->setParent(nullptr);
        ShowWindow(found.window, SW_SHOWNORMAL);
        QTest::qWait(300);
        check(GetParent(found.window) == nullptr && IsWindow(found.window), "detach native window");
        foreign->setParent(container->windowHandle());
        container->show();
        QTest::qWait(300);
        check(GetParent(found.window) != nullptr && IsWindow(found.window),
              "reattach native window");
        std::cout << "native parent/resize/detach/reattach passed; DPR=" << host.devicePixelRatioF()
                  << " native DPI=" << GetDpiForWindow(found.window)
                  << ". Input focus, Chinese IME and APK drop require separate acceptance; probe "
                     "does not establish them.\n";
        foreign->setParent(nullptr);
        process.terminate();
        if (!process.waitForFinished(2000)) {
            process.kill();
            process.waitForFinished(1500);
        }
        std::cout << process.readAll().toStdString();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        process.terminate();
        if (!process.waitForFinished(1500)) {
            process.kill();
            process.waitForFinished(1000);
        }
        std::cerr << process.readAll().toStdString();
        return 1;
    }
}
