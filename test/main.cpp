#include "ui/main_window.h"
#include "core/self_test.h"
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtGui/QPixmap>
#include <QtWidgets/QApplication>

#include <string>
#include <string_view>
#include <filesystem>

int main(int argc, char *argv[])
{
    std::string snapshotPath;
    std::string initialPage;
    std::string initialSize;
    std::string agentSmokeHost;
    std::uint16_t agentSmokePort = 0;
    std::string adbSerialSmokeSerial;
    std::string adbSerialSmokeEndpoint;
    std::string agentSerialSmokeSerial;
    std::string agentSerialSmokeEndpoint;
    std::uint16_t agentSerialSmokePort = 0;
    std::string agentDeploySmokeSerial;
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--self-test") {
            return serialdemo::runSelfTest();
        }
        if (std::string_view(argv[index]) == "--ui-snapshot" && index + 1 < argc) {
            snapshotPath = argv[++index];
        }
        if (std::string_view(argv[index]) == "--ui-page" && index + 1 < argc) {
            initialPage = argv[++index];
        }
        if (std::string_view(argv[index]) == "--ui-size" && index + 1 < argc) {
            initialSize = argv[++index];
        }
        if (std::string_view(argv[index]) == "--agent-smoke" && index + 2 < argc) {
            agentSmokeHost = argv[++index];
            const int port = std::stoi(argv[++index]);
            if (port <= 0 || port > 65535) {
                return 2;
            }
            agentSmokePort = static_cast<std::uint16_t>(port);
        }
        if (std::string_view(argv[index]) == "--adb-serial-smoke" && index + 2 < argc) {
            adbSerialSmokeSerial = argv[++index];
            adbSerialSmokeEndpoint = argv[++index];
        }
        if (std::string_view(argv[index]) == "--agent-serial-smoke" && index + 3 < argc) {
            agentSerialSmokeSerial = argv[++index];
            const int port = std::stoi(argv[++index]);
            if (port <= 0 || port > 65535) {
                return 2;
            }
            agentSerialSmokePort = static_cast<std::uint16_t>(port);
            agentSerialSmokeEndpoint = argv[++index];
        }
        if (std::string_view(argv[index]) == "--agent-deploy-smoke" && index + 1 < argc) {
            agentDeploySmokeSerial = argv[++index];
        }
    }
    if (!agentSmokeHost.empty()) {
        return serialdemo::runAgentSmokeTest(agentSmokeHost, agentSmokePort);
    }
    if (!adbSerialSmokeSerial.empty()) {
        return serialdemo::runAdbSerialSmokeTest(adbSerialSmokeSerial, adbSerialSmokeEndpoint);
    }
    if (!agentSerialSmokeSerial.empty()) {
        return serialdemo::runAgentSerialSmokeTest(agentSerialSmokeSerial,
                                                   agentSerialSmokePort,
                                                   agentSerialSmokeEndpoint);
    }
    if (!agentDeploySmokeSerial.empty()) {
        const std::string applicationDirectory =
            std::filesystem::absolute(std::filesystem::u8path(argv[0])).parent_path().u8string();
        return serialdemo::runAgentDeploymentSmokeTest(agentDeploySmokeSerial,
                                                       applicationDirectory);
    }
    QApplication app(argc, argv);
    MainWindow window;
    if (!initialPage.empty()) {
        window.navigateTo(QString::fromUtf8(initialPage));
    }
    if (!initialSize.empty()) {
        const QStringList dimensions = QString::fromUtf8(initialSize).split(QLatin1Char('x'));
        bool widthOk = false;
        bool heightOk = false;
        const int width = dimensions.value(0).toInt(&widthOk);
        const int height = dimensions.value(1).toInt(&heightOk);
        if (dimensions.size() == 2 && widthOk && heightOk && width > 0 && height > 0) {
            window.resize(width, height);
        }
    }
    window.show();
    if (!snapshotPath.empty()) {
        QTimer::singleShot(1500, &app, [&app, &window, snapshotPath] {
            const bool saved = window.grab().save(QString::fromUtf8(snapshotPath), "PNG");
            app.exit(saved ? 0 : 2);
        });
    }
    return app.exec();
}
