#pragma once

#include <QtCore/QDir>
#include <QtCore/QString>
#include <filesystem>

inline QString testOutputPath(const QString& relative = {})
{
    auto directory = qEnvironmentVariable("TESTTOOLS_TEST_OUTPUT");
    if (directory.isEmpty())
        directory = QDir::currentPath() + "/build/ui-checks";
    directory = QDir(directory).absolutePath();
    QDir().mkpath(directory);
    return relative.isEmpty() ? directory : QDir(directory).filePath(relative);
}

inline std::filesystem::path testOutputDirectory()
{
    return std::filesystem::path(testOutputPath().toStdWString());
}
