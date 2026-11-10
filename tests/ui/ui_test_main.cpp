// Entry point for the offscreen UI tests: a real QApplication on the
// "offscreen" platform, so widgets are created and painted without a display.
#include <QApplication>

#include <gtest/gtest.h>

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QCoreApplication::addLibraryPath(QStringLiteral(TDM_QT_PLUGIN_DIR));  // qoffscreen.dll
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("TeslaDM-tests"));  // isolate QSettings
    QApplication::setApplicationName(QStringLiteral("TeslaDM-tests"));
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
