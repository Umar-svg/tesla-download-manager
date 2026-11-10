#include "ui/main_window.hpp"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTableView>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>

#include "core/version.hpp"
#include "ui/engine_bridge.hpp"
#include "ui/format.hpp"
#include "ui/widgets.hpp"

namespace tdm::ui {

MainWindow::MainWindow(EngineBridge& bridge, QWidget* parent)
    : QMainWindow(parent), bridge_(bridge), settings_(UiSettings::load()) {
    setWindowTitle(QString::fromUtf8(kProductName));
    setWindowIcon(style()->standardIcon(QStyle::SP_ArrowDown));
    resize(1100, 600);

    model_ = new DownloadTableModel(this);
    proxy_ = new QSortFilterProxyModel(this);
    proxy_->setSourceModel(model_);
    proxy_->setSortRole(DownloadTableModel::SortRole);

    view_ = new QTableView(this);
    view_->setModel(proxy_);
    view_->setSortingEnabled(true);
    view_->sortByColumn(-1, Qt::AscendingOrder);  // insertion order until the user sorts
    view_->setSelectionBehavior(QAbstractItemView::SelectRows);
    view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view_->setContextMenuPolicy(Qt::ActionsContextMenu);
    view_->verticalHeader()->hide();
    view_->setItemDelegateForColumn(DownloadTableModel::Progress, new ProgressDelegate(view_));
    view_->horizontalHeader()->setSectionResizeMode(DownloadTableModel::Name, QHeaderView::Stretch);
    view_->setColumnWidth(DownloadTableModel::Progress, 140);
    view_->setColumnWidth(DownloadTableModel::Status, 160);

    segbar_ = new SegmentBar(this);
    auto* central = new QWidget(this);
    auto* v = new QVBoxLayout(central);
    v->setContentsMargins(4, 4, 4, 4);
    v->addWidget(view_, 1);
    v->addWidget(segbar_);
    setCentralWidget(central);

    status_active_ = new QLabel(this);
    status_speed_ = new QLabel(this);
    statusBar()->addWidget(status_active_);
    statusBar()->addPermanentWidget(status_speed_);

    buildActions();

    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        tray_ = new QSystemTrayIcon(windowIcon(), this);
        tray_->setToolTip(QString::fromUtf8(kProductName));
        auto* menu = new QMenu(this);
        menu->addAction(tr("Show"), this, [this] {
            showNormal();
            raise();
            activateWindow();
        });
        menu->addAction(tr("Quit"), qApp, &QApplication::quit);
        tray_->setContextMenu(menu);
        tray_->show();
    }

    connect(&bridge_, &EngineBridge::snapshotsReady, this, &MainWindow::onSnapshots);
    connect(view_->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] {
        updateActions();
        segbar_->setSnapshot(current());
    });
    connect(view_, &QTableView::doubleClicked, this, [this] { openSelectedFile(false); });

    refreshQueueNames();
    model_->reset(bridge_.engine().snapshots());
    updateActions();
    updateStatus();
}

void MainWindow::buildActions() {
    auto* tb = addToolBar(tr("Main"));
    tb->setMovable(false);
    tb->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    auto mk = [&](const QString& text, QStyle::StandardPixmap icon, const QKeySequence& key) {
        auto* a = new QAction(style()->standardIcon(icon), text, this);
        if (!key.isEmpty()) a->setShortcut(key);
        return a;
    };
    add_ = mk(tr("Add URL"), QStyle::SP_FileDialogNewFolder, QKeySequence::New);
    resume_ = mk(tr("Resume"), QStyle::SP_MediaPlay, {});
    pause_ = mk(tr("Pause"), QStyle::SP_MediaPause, {});
    cancel_ = mk(tr("Cancel"), QStyle::SP_MediaStop, {});
    remove_ = mk(tr("Remove"), QStyle::SP_TrashIcon, QKeySequence::Delete);
    open_file_ = mk(tr("Open"), QStyle::SP_FileIcon, {});
    open_folder_ = mk(tr("Open folder"), QStyle::SP_DirOpenIcon, {});
    auto* queues = mk(tr("Queues"), QStyle::SP_FileDialogListView, {});
    auto* settings = mk(tr("Settings"), QStyle::SP_FileDialogDetailedView, {});

    connect(add_, &QAction::triggered, this, [this] {
        const QString clip = QApplication::clipboard()->text().trimmed();
        openAddDialog(looksLikeUrl(clip) ? clip : QString());
    });
    connect(resume_, &QAction::triggered, this, [this] { forSelected([this](auto id) { bridge_.engine().start(id); }); });
    connect(pause_, &QAction::triggered, this, [this] { forSelected([this](auto id) { bridge_.engine().pause(id); }); });
    connect(cancel_, &QAction::triggered, this,
            [this] { forSelected([this](auto id) { bridge_.engine().cancel(id, false); }); });
    connect(remove_, &QAction::triggered, this, &MainWindow::removeSelected);
    connect(open_file_, &QAction::triggered, this, [this] { openSelectedFile(false); });
    connect(open_folder_, &QAction::triggered, this, [this] { openSelectedFile(true); });
    connect(queues, &QAction::triggered, this, &MainWindow::showQueues);
    connect(settings, &QAction::triggered, this, &MainWindow::showSettings);

    for (auto* a : {add_, resume_, pause_, cancel_, remove_}) tb->addAction(a);
    tb->addSeparator();
    for (auto* a : {queues, settings}) tb->addAction(a);
    for (auto* a : {resume_, pause_, cancel_, remove_, open_file_, open_folder_}) view_->addAction(a);

    auto* file = menuBar()->addMenu(tr("&File"));
    file->addAction(add_);
    file->addSeparator();
    file->addAction(tr("Quit"), QKeySequence::Quit, qApp, &QApplication::quit);
    auto* dl = menuBar()->addMenu(tr("&Downloads"));
    for (auto* a : {resume_, pause_, cancel_, remove_, open_file_, open_folder_}) dl->addAction(a);
    auto* tools = menuBar()->addMenu(tr("&Tools"));
    tools->addAction(queues);
    tools->addAction(settings);
}

void MainWindow::onSnapshots(const QList<tdm::engine::Snapshot>& batch) {
    for (const auto& s : batch) {
        const auto* before = model_->snapshotById(s.id);
        const bool was_running = before && before->state != db::State::Completed && before->state != db::State::Failed;
        model_->apply(s);
        if (was_running && (s.state == db::State::Completed || s.state == db::State::Failed)) {
            emit downloadFinished(s.id, s.state == db::State::Completed);
            if (tray_) {
                tray_->showMessage(s.state == db::State::Completed ? tr("Download complete") : tr("Download failed"),
                                   fromUtf8(s.file_name.empty() ? s.url : s.file_name),
                                   s.state == db::State::Completed ? QSystemTrayIcon::Information
                                                                   : QSystemTrayIcon::Warning,
                                   4000);
            }
        }
    }
    segbar_->setSnapshot(current());
    updateActions();
    updateStatus();
}

void MainWindow::refreshQueueNames() {
    std::map<std::int64_t, QString> names;
    for (const auto& q : bridge_.engine().queues()) names[q.id] = fromUtf8(q.name);
    model_->setQueueNames(std::move(names));
}

std::vector<engine::DownloadId> MainWindow::selectedIds() const {
    std::vector<engine::DownloadId> ids;
    for (const auto& idx : view_->selectionModel()->selectedRows())
        ids.push_back(proxy_->data(idx, DownloadTableModel::IdRole).toLongLong());
    return ids;
}

const engine::Snapshot* MainWindow::current() const {
    const auto ids = selectedIds();
    return ids.size() == 1 ? model_->snapshotById(ids.front()) : nullptr;
}

void MainWindow::forSelected(const std::function<void(engine::DownloadId)>& f) {
    for (auto id : selectedIds()) f(id);
}

void MainWindow::updateActions() {
    bool any_run = false, any_resumable = false, any = false, any_done = false;
    for (auto id : selectedIds()) {
        const auto* s = model_->snapshotById(id);
        if (!s) continue;
        any = true;
        const auto st = s->state;
        if (st == db::State::Downloading || st == db::State::Queued || st == db::State::Waiting ||
            st == db::State::Probing)
            any_run = true;
        if (st == db::State::Paused || st == db::State::Failed || st == db::State::Cancelled) any_resumable = true;
        if (st == db::State::Completed) any_done = true;
    }
    pause_->setEnabled(any_run);
    resume_->setEnabled(any_resumable);
    cancel_->setEnabled(any_run || any_resumable);
    remove_->setEnabled(any);
    open_file_->setEnabled(any_done);
    open_folder_->setEnabled(any);
}

void MainWindow::updateStatus() {
    status_active_->setText(tr("%n downloading", nullptr, model_->activeCount()) +
                            tr(" | %n in list", nullptr, model_->rowCount()));
    const QString sp = formatSpeed(model_->totalSpeed());
    status_speed_->setText(sp.isEmpty() ? QString() : tr("Total: %1").arg(sp));
}

void MainWindow::openAddDialog(const QString& url) {
    AddDownloadDialog dlg(settings_, bridge_.engine().queues(), url, this);
    if (dlg.exec() != QDialog::Accepted) return;
    try {
        bridge_.engine().add(dlg.request());
    } catch (const std::exception& e) {
        showError(tr("Add download"), e);
    }
}

void MainWindow::removeSelected() {
    const auto ids = selectedIds();
    if (ids.empty()) return;
    QMessageBox box(QMessageBox::Question, tr("Remove"), tr("Remove %n download(s) from the list?", nullptr,
                                                            static_cast<int>(ids.size())),
                    QMessageBox::NoButton, this);
    auto* keep = box.addButton(tr("Remove, keep files"), QMessageBox::AcceptRole);
    auto* del = box.addButton(tr("Remove and delete files"), QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != keep && box.clickedButton() != del) return;
    const bool delete_files = box.clickedButton() == del;
    for (auto id : ids) bridge_.engine().remove(id, delete_files);
}

void MainWindow::openSelectedFile(bool folder) {
    const auto* s = current();
    if (!s) return;
    if (!folder && s->state == db::State::Completed && !s->file_path.empty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(fromUtf8(s->file_path)));
        return;
    }
    if (folder && !s->file_path.empty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(fromUtf8(s->file_path)).absolutePath()));
    else if (folder)
        QDesktopServices::openUrl(QUrl::fromLocalFile(settings_.default_folder));
}

void MainWindow::showSettings() {
    SettingsDialog dlg(settings_, this);
    if (dlg.exec() != QDialog::Accepted) return;
    settings_ = dlg.settings();
    settings_.save();
    bridge_.engine().set_max_active(settings_.max_active);
    bridge_.engine().set_global_speed_limit(settings_.global_limit ? std::optional(settings_.global_limit)
                                                                   : std::nullopt);
}

void MainWindow::showQueues() {
    QueuesDialog dlg(bridge_.engine(), this);
    dlg.exec();
    refreshQueueNames();
}

void MainWindow::showError(const QString& what, const std::exception& e) {
    QMessageBox::warning(this, what, QString::fromUtf8(e.what()));
}

}  // namespace tdm::ui
