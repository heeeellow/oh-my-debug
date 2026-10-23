#include "core/support/command_line.h"
#include "diagnostics/self_test.h"
#include "ui/main_window.h"
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtGui/QPixmap>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QTreeView>

#include <filesystem>
#include <string>
#include <string_view>

int main(int argc, char* argv[])
{
    // Establish the offline boundary before any early diagnostic command can run.
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--offline")
            qputenv("TESTTOOLS_OFFLINE", "1");
    }
    std::string snapshotPath;
    std::string initialPage;
    std::string initialSize;
    std::string initialSerialMode;
    int snapshotDelay = 1500;
    std::string agentSmokeHost;
    std::uint16_t agentSmokePort = 0;
    std::string adbSerialSmokeSerial;
    std::string adbSerialSmokeEndpoint;
    std::string agentSerialSmokeSerial;
    std::string agentSerialSmokeEndpoint;
    std::uint16_t agentSerialSmokePort = 0;
    std::string agentDeploySmokeSerial;
    std::string agentReconnectSmokeSerial;
    std::string agentAtSmokeSerial;
    std::string agentAtSmokeEndpoint;
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--offline")
            qputenv("TESTTOOLS_OFFLINE", "1");
        if (std::string_view(argv[index]) == "--snapshot-delay" && index + 1 < argc) {
            bool ok = false;
            snapshotDelay = QString::fromUtf8(argv[++index]).toInt(&ok);
            if (!ok || snapshotDelay < 100 || snapshotDelay > 30000)
                return 2;
        }
        if (std::string_view(argv[index]) == "--self-test") {
            if (qEnvironmentVariableIsSet("TESTTOOLS_OFFLINE"))
                return 2;
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
        if (std::string_view(argv[index]) == "--ui-serial-mode" && index + 1 < argc) {
            initialSerialMode = argv[++index];
        }
        if (std::string_view(argv[index]) == "--agent-smoke" && index + 2 < argc) {
            agentSmokeHost = argv[++index];
            if (!testtools::parseTcpPort(argv[++index], agentSmokePort)) {
                return 2;
            }
        }
        if (std::string_view(argv[index]) == "--adb-serial-smoke" && index + 2 < argc) {
            adbSerialSmokeSerial = argv[++index];
            adbSerialSmokeEndpoint = argv[++index];
        }
        if (std::string_view(argv[index]) == "--agent-serial-smoke" && index + 3 < argc) {
            agentSerialSmokeSerial = argv[++index];
            if (!testtools::parseTcpPort(argv[++index], agentSerialSmokePort)) {
                return 2;
            }
            agentSerialSmokeEndpoint = argv[++index];
        }
        if (std::string_view(argv[index]) == "--agent-deploy-smoke" && index + 1 < argc) {
            agentDeploySmokeSerial = argv[++index];
        }
        if (std::string_view(argv[index]) == "--agent-reconnect-smoke" && index + 1 < argc) {
            agentReconnectSmokeSerial = argv[++index];
        }
        if (std::string_view(argv[index]) == "--agent-at-smoke" && index + 2 < argc) {
            agentAtSmokeSerial = argv[++index];
            agentAtSmokeEndpoint = argv[++index];
        }
    }
    if (qEnvironmentVariableIsSet("TESTTOOLS_OFFLINE") &&
        (!agentSmokeHost.empty() || !adbSerialSmokeSerial.empty() ||
         !agentSerialSmokeSerial.empty() || !agentDeploySmokeSerial.empty() ||
         !agentReconnectSmokeSerial.empty() || !agentAtSmokeSerial.empty()))
        return 2;
    if (!agentSmokeHost.empty()) {
        return serialdemo::runAgentSmokeTest(agentSmokeHost, agentSmokePort);
    }
    if (!adbSerialSmokeSerial.empty()) {
        return serialdemo::runAdbSerialSmokeTest(adbSerialSmokeSerial, adbSerialSmokeEndpoint);
    }
    if (!agentSerialSmokeSerial.empty()) {
        return serialdemo::runAgentSerialSmokeTest(
            agentSerialSmokeSerial, agentSerialSmokePort, agentSerialSmokeEndpoint);
    }
    if (!agentDeploySmokeSerial.empty()) {
        const std::string applicationDirectory =
            std::filesystem::absolute(std::filesystem::u8path(argv[0])).parent_path().u8string();
        return serialdemo::runAgentDeploymentSmokeTest(agentDeploySmokeSerial,
                                                       applicationDirectory);
    }
    if (!agentReconnectSmokeSerial.empty()) {
        return serialdemo::runAgentReconnectSmokeTest(agentReconnectSmokeSerial);
    }
    if (!agentAtSmokeSerial.empty()) {
        return serialdemo::runAgentAtSmokeTest(agentAtSmokeSerial, agentAtSmokeEndpoint);
    }
    QApplication app(argc, argv);
    QApplication::setStyle(QStringLiteral("Fusion"));
    QApplication::setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 10));
    MainWindow window;
    if (!initialPage.empty()) {
        window.navigateTo(QString::fromUtf8(initialPage));
    }
    if (!initialSerialMode.empty() &&
        !window.setSerialModeForDiagnostics(QString::fromUtf8(initialSerialMode))) {
        return 2;
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
    const QSize requestedSize = window.size();
    window.show();
    if (!initialSize.empty() && !snapshotPath.empty()) {
        // The window manager may constrain the initial show to the current screen.
        // Restore the requested logical size for reproducible off-screen captures.
        QTimer::singleShot(0, &window, [&window, requestedSize] { window.resize(requestedSize); });
    }
    if (!snapshotPath.empty()) {
        QTimer::singleShot(snapshotDelay, &app, [&app, &window, snapshotPath] {
            const bool saved = window.grab().save(QString::fromUtf8(snapshotPath), "PNG");
            QJsonArray controls;
            controls.append(QJsonObject{{"name", "window"},
                                        {"logicalWidth", window.width()},
                                        {"logicalHeight", window.height()},
                                        {"dpr", window.devicePixelRatioF()}});
            for (auto* widget : window.findChildren<QWidget*>()) {
                QString name = widget->property("diagnosticObjectName").toString();
                if (name.isEmpty()) name = widget->objectName();
                const auto* label = qobject_cast<QLabel*>(widget);
                if (name.isEmpty() && !label)
                    continue;
                QJsonObject entry{{"name", name},
                                  {"visible", widget->isVisible()},
                                  {"enabled", widget->isEnabled()}};
                if (label)
                    entry.insert("text", label->text());
                if (auto* button = qobject_cast<QPushButton*>(widget)) {
                    entry.insert("text", button->text());
                    entry.insert("checked", button->isChecked());
                }
                if (auto* view = qobject_cast<QTreeView*>(widget); view && view->model()) {
                    entry.insert("rowCount", view->model()->rowCount());
                }
                if (auto* table = qobject_cast<QTableWidget*>(widget)) {
                    QJsonArray rows;
                    for (int row = 0; row < table->rowCount(); ++row) {
                        QJsonArray fields;
                        for (int column = 0; column < table->columnCount(); ++column)
                            fields.append(table->item(row, column)
                                              ? table->item(row, column)->text()
                                              : QString{});
                        rows.append(fields);
                    }
                    entry.insert("rows", rows);
                    entry.insert("selectedRow", table->currentRow());
                }
                if (auto* tree = qobject_cast<QTreeWidget*>(widget)) {
                    entry.insert("rowCount", tree->topLevelItemCount());
                    QJsonArray rows;
                    for (int row = 0; row < tree->topLevelItemCount(); ++row) {
                        QJsonArray fields;
                        for (int column = 0; column < tree->columnCount(); ++column)
                            fields.append(tree->topLevelItem(row)->text(column));
                        rows.append(fields);
                    }
                    entry.insert("rows", rows);
                }
                controls.append(entry);
            }
            QFile stateFile(QString::fromUtf8(snapshotPath) + QStringLiteral(".json"));
            const auto state = QJsonDocument(controls).toJson();
            const bool stateSaved =
                stateFile.open(QIODevice::WriteOnly) && stateFile.write(state) == state.size();
            app.exit(saved && stateSaved ? 0 : 2);
        });
    }
    return app.exec();
}
