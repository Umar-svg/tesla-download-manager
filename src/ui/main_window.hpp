#pragma once

#include <QMainWindow>

#include <functional>
#include <map>
#include <vector>

#include "ui/dialogs.hpp"
#include "ui/download_model.hpp"

class QTableView;
class QSortFilterProxyModel;
class QLabel;
class QAction;
class QSystemTrayIcon;

namespace tdm::ui {

class EngineBridge;
class SegmentBar;

// Main window. Talks to the engine only through EngineBridge (public
// DownloadEngine API + batched snapshot signal). Must be destroyed before the
// bridge.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(EngineBridge& bridge, QWidget* parent = nullptr);

    DownloadTableModel& model() noexcept { return *model_; }
    QTableView* view() const noexcept { return view_; }
    void openAddDialog(const QString& url = {});

signals:
    void downloadFinished(qint64 id, bool ok);  // for notifications (and tests)

private:
    void buildActions();
    void onSnapshots(const QList<tdm::engine::Snapshot>& batch);
    void refreshQueueNames();
    void updateActions();
    void updateStatus();
    std::vector<engine::DownloadId> selectedIds() const;
    const engine::Snapshot* current() const;
    void forSelected(const std::function<void(engine::DownloadId)>& f);
    void removeSelected();
    void openSelectedFile(bool folder);
    void showSettings();
    void showQueues();
    void showError(const QString& what, const std::exception& e);

    EngineBridge& bridge_;
    UiSettings settings_;
    DownloadTableModel* model_;
    QSortFilterProxyModel* proxy_;
    QTableView* view_;
    SegmentBar* segbar_;
    QLabel* status_active_;
    QLabel* status_speed_;
    QSystemTrayIcon* tray_ = nullptr;
    QAction *add_, *resume_, *pause_, *cancel_, *remove_, *open_file_, *open_folder_;
};

}  // namespace tdm::ui
