#include <QApplication>
#include <QDir>
#include <QMessageBox>
#include <QStandardPaths>

#include <memory>

#include "core/version.hpp"
#include "ui/dialogs.hpp"
#include "ui/engine_bridge.hpp"
#include "ui/format.hpp"
#include "ui/instance_guard.hpp"
#include "ui/main_window.hpp"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("TeslaDM"));
    QApplication::setApplicationDisplayName(QString::fromUtf8(tdm::kProductName));
    QApplication::setApplicationVersion(QString::fromUtf8(tdm::kVersion));
    QApplication::setOrganizationName(QStringLiteral("TeslaDM"));

    // Single instance: a second launch passes its URL (or nothing) to the
    // running instance and exits before touching the database.
    const QStringList args = QApplication::arguments();
    const QString url_arg = args.size() > 1 && tdm::ui::looksLikeUrl(args.at(1)) ? args.at(1) : QString();
    tdm::ui::InstanceGuard guard(tdm::ui::InstanceGuard::defaultKey());
    if (!guard.tryBecomePrimary(url_arg)) return 0;

    const QString data_dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(data_dir);

    const auto settings = tdm::ui::UiSettings::load();
    tdm::engine::EngineOptions opts;
    opts.db_path = std::filesystem::path((data_dir + QStringLiteral("/tdm.sqlite")).toStdWString());
    opts.max_active = settings.max_active;

    // Ownership order matters: the bridge (engine) is created first and
    // destroyed last, after the window that uses it.
    std::unique_ptr<tdm::ui::EngineBridge> bridge;
    try {
        bridge = std::make_unique<tdm::ui::EngineBridge>(opts);
    } catch (const std::exception& e) {
        QMessageBox::critical(nullptr, QString::fromUtf8(tdm::kProductName),
                              QObject::tr("Could not start the download engine:\n%1\n\nDatabase: %2")
                                  .arg(QString::fromUtf8(e.what()), QDir::toNativeSeparators(data_dir)));
        return 1;
    }

    int rc = 0;
    {
        tdm::ui::MainWindow w(*bridge);
        w.show();
        // "TeslaDM.exe <url>" opens the Add dialog (hook for browser integration),
        // also when the URL arrives from a second launch.
        auto handle = [&w](const QString& url) {
            w.showNormal();
            w.raise();
            w.activateWindow();
            if (!url.isEmpty()) w.openAddDialog(url);
        };
        QObject::connect(&guard, &tdm::ui::InstanceGuard::messageReceived, &w, handle, Qt::QueuedConnection);
        if (!url_arg.isEmpty())
            QMetaObject::invokeMethod(&w, [handle, url_arg] { handle(url_arg); }, Qt::QueuedConnection);
        rc = QApplication::exec();
    }  // window destroyed here
    bridge.reset();  // engine shutdown: saves resume state, joins engine threads
    return rc;
}
