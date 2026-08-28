#include "ui/main_window.h"
#include "core/self_test.h"
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtGui/QPixmap>
#include <QtWidgets/QApplication>

#include <string>
#include <string_view>

int main(int argc, char *argv[])
{
    std::string snapshotPath;
    std::string initialPage;
    std::string initialSize;
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
