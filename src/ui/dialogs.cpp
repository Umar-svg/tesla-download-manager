#include "ui/dialogs.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTimeEdit>
#include <QVBoxLayout>

#include "ui/format.hpp"

namespace tdm::ui {
namespace {

QWidget* folderRow(QLineEdit* edit, QWidget* parent) {
    auto* w = new QWidget(parent);
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    auto* browse = new QPushButton(QObject::tr("Browse..."), w);
    h->addWidget(edit, 1);
    h->addWidget(browse);
    QObject::connect(browse, &QPushButton::clicked, w, [edit, w] {
        const QString d = QFileDialog::getExistingDirectory(w, QObject::tr("Choose folder"), edit->text());
        if (!d.isEmpty()) edit->setText(QDir::toNativeSeparators(d));
    });
    return w;
}

QString rateText(std::uint64_t bps) { return bps ? formatBytes(bps) + QStringLiteral("/s") : QString(); }

}  // namespace

UiSettings UiSettings::load() {
    QSettings s;
    UiSettings u;
    u.default_folder = s.value(QStringLiteral("defaultFolder"),
                               QStandardPaths::writableLocation(QStandardPaths::DownloadLocation))
                           .toString();
    u.connections = std::clamp(s.value(QStringLiteral("connections"), 8).toInt(), 1, 32);
    u.max_active = std::clamp(s.value(QStringLiteral("maxActive"), 8).toInt(), 1, 64);
    u.global_limit = s.value(QStringLiteral("globalLimit"), 0).toULongLong();
    return u;
}

void UiSettings::save() const {
    QSettings s;
    s.setValue(QStringLiteral("defaultFolder"), default_folder);
    s.setValue(QStringLiteral("connections"), connections);
    s.setValue(QStringLiteral("maxActive"), max_active);
    s.setValue(QStringLiteral("globalLimit"), QVariant::fromValue<qulonglong>(global_limit));
}

// ---------------------------------------------------------------------------

AddDownloadDialog::AddDownloadDialog(const UiSettings& d, const std::vector<engine::QueueInfo>& queues,
                                     const QString& url, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Add download"));
    auto* form = new QFormLayout;
    url_ = new QLineEdit(url, this);
    url_->setPlaceholderText(QStringLiteral("https://example.com/file.zip"));
    folder_ = new QLineEdit(QDir::toNativeSeparators(d.default_folder), this);
    name_ = new QLineEdit(this);
    name_->setPlaceholderText(tr("Automatic (from the server)"));
    connections_ = new QSpinBox(this);
    connections_->setRange(1, 32);
    connections_->setValue(d.connections);
    queue_ = new QComboBox(this);
    for (const auto& q : queues) queue_->addItem(fromUtf8(q.name), QVariant::fromValue<qint64>(q.id));
    limit_ = new QLineEdit(this);
    limit_->setPlaceholderText(tr("Unlimited (e.g. 500K, 2M)"));
    sha_ = new QLineEdit(this);
    sha_->setPlaceholderText(tr("Optional: expected SHA-256 (64 hex digits)"));
    start_now_ = new QCheckBox(tr("Start now"), this);
    start_now_->setChecked(true);
    allow_page_ = new QCheckBox(tr("Download even if the link is a web page"), this);
    allow_page_->setToolTip(tr("Links such as a video site's watch page return a web page, not the file. "
                               "Leave this off to be warned instead."));
    error_ = new QLabel(this);
    error_->setStyleSheet(QStringLiteral("color: #c62828"));

    form->addRow(tr("URL:"), url_);
    form->addRow(tr("Save to:"), folderRow(folder_, this));
    form->addRow(tr("File name:"), name_);
    form->addRow(tr("Connections:"), connections_);
    form->addRow(tr("Queue:"), queue_);
    form->addRow(tr("Speed limit:"), limit_);
    form->addRow(tr("SHA-256:"), sha_);
    form->addRow(QString(), start_now_);
    form->addRow(QString(), allow_page_);

    buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    for (auto* e : {url_, folder_, limit_, sha_}) connect(e, &QLineEdit::textChanged, this, &AddDownloadDialog::validate);

    auto* v = new QVBoxLayout(this);
    v->addLayout(form);
    v->addWidget(error_);
    v->addWidget(buttons_);
    resize(560, sizeHint().height());
    validate();
}

void AddDownloadDialog::validate() {
    QString err;
    if (!looksLikeUrl(url_->text())) err = tr("Enter an http:// or https:// URL.");
    else if (folder_->text().trimmed().isEmpty()) err = tr("Choose a folder.");
    else if (!parseRate(limit_->text())) err = tr("Speed limit must look like 500K or 2M.");
    else if (!sha_->text().trimmed().isEmpty() && !isSha256Hex(sha_->text())) err = tr("SHA-256 must be 64 hex digits.");
    error_->setText(err);
    buttons_->button(QDialogButtonBox::Ok)->setEnabled(err.isEmpty());
}

engine::AddRequest AddDownloadDialog::request() const {
    engine::AddRequest r;
    r.url = toUtf8(url_->text().trimmed());
    r.directory = std::filesystem::path(folder_->text().trimmed().toStdWString());
    r.file_name = toUtf8(name_->text().trimmed());
    r.connections = connections_->value();
    r.queue_id = queue_->currentData().toLongLong();
    if (r.queue_id == 0) r.queue_id = 1;
    const auto lim = parseRate(limit_->text()).value_or(0);
    if (lim) r.speed_limit_bps = lim;
    r.sha256_expected = toUtf8(sha_->text().trimmed().toLower());
    r.start_now = start_now_->isChecked();
    r.allow_web_page = allow_page_->isChecked();
    return r;
}

// ---------------------------------------------------------------------------

SettingsDialog::SettingsDialog(const UiSettings& cur, QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Settings"));
    auto* form = new QFormLayout;
    folder_ = new QLineEdit(QDir::toNativeSeparators(cur.default_folder), this);
    connections_ = new QSpinBox(this);
    connections_->setRange(1, 32);
    connections_->setValue(cur.connections);
    max_active_ = new QSpinBox(this);
    max_active_->setRange(1, 64);
    max_active_->setValue(cur.max_active);
    limit_ = new QLineEdit(rateText(cur.global_limit), this);
    limit_->setPlaceholderText(tr("Unlimited (e.g. 5M)"));
    error_ = new QLabel(this);
    error_->setStyleSheet(QStringLiteral("color: #c62828"));
    form->addRow(tr("Default folder:"), folderRow(folder_, this));
    form->addRow(tr("Connections per download:"), connections_);
    form->addRow(tr("Max simultaneous downloads:"), max_active_);
    form->addRow(tr("Global speed limit:"), limit_);
    buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(limit_, &QLineEdit::textChanged, this, [this] {
        const bool ok = parseRate(limit_->text()).has_value();
        error_->setText(ok ? QString() : tr("Speed limit must look like 500K or 2M."));
        buttons_->button(QDialogButtonBox::Ok)->setEnabled(ok);
    });
    auto* v = new QVBoxLayout(this);
    v->addLayout(form);
    v->addWidget(error_);
    v->addWidget(buttons_);
}

UiSettings SettingsDialog::settings() const {
    UiSettings u;
    u.default_folder = folder_->text().trimmed();
    u.connections = connections_->value();
    u.max_active = max_active_->value();
    u.global_limit = parseRate(limit_->text()).value_or(0);
    return u;
}

// ---------------------------------------------------------------------------

QueuesDialog::QueuesDialog(engine::DownloadEngine& engine, QWidget* parent) : QDialog(parent), engine_(engine) {
    setWindowTitle(tr("Queues and scheduler"));
    list_ = new QListWidget(this);
    auto* add = new QPushButton(tr("Add queue..."), this);
    auto* left = new QVBoxLayout;
    left->addWidget(list_);
    left->addWidget(add);

    max_active_ = new QSpinBox(this);
    max_active_->setRange(1, 64);
    schedule_ = new QGroupBox(tr("Run only during this window"), this);
    schedule_->setCheckable(true);
    start_ = new QTimeEdit(this);
    stop_ = new QTimeEdit(this);
    start_->setDisplayFormat(QStringLiteral("HH:mm"));
    stop_->setDisplayFormat(QStringLiteral("HH:mm"));
    auto* days = new QHBoxLayout;
    const char* names[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    for (int i = 0; i < 7; ++i) {
        days_[i] = new QCheckBox(tr(names[i]), this);
        days->addWidget(days_[i]);
    }
    auto* sf = new QFormLayout(schedule_);
    sf->addRow(tr("Start:"), start_);
    sf->addRow(tr("Stop:"), stop_);
    sf->addRow(tr("Days:"), days);
    open_label_ = new QLabel(this);
    auto* apply_btn = new QPushButton(tr("Apply"), this);

    auto* right = new QFormLayout;
    right->addRow(tr("Simultaneous downloads:"), max_active_);
    right->addRow(schedule_);
    right->addRow(open_label_);
    right->addRow(apply_btn);

    auto* h = new QHBoxLayout;
    h->addLayout(left, 1);
    h->addLayout(right, 2);
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(close, &QDialogButtonBox::rejected, this, &QDialog::accept);
    auto* v = new QVBoxLayout(this);
    v->addLayout(h);
    v->addWidget(close);

    connect(list_, &QListWidget::currentRowChanged, this, &QueuesDialog::showQueue);
    connect(apply_btn, &QPushButton::clicked, this, &QueuesDialog::apply);
    connect(add, &QPushButton::clicked, this, &QueuesDialog::addQueue);
    reload();
    resize(640, 360);
}

void QueuesDialog::reload(std::int64_t select_id) {
    queues_ = engine_.queues();
    list_->clear();
    int sel = 0;
    for (int i = 0; i < static_cast<int>(queues_.size()); ++i) {
        list_->addItem(fromUtf8(queues_[static_cast<std::size_t>(i)].name));
        if (queues_[static_cast<std::size_t>(i)].id == select_id) sel = i;
    }
    list_->setCurrentRow(sel);
}

void QueuesDialog::showQueue(int row) {
    if (row < 0 || row >= static_cast<int>(queues_.size())) return;
    const auto& q = queues_[static_cast<std::size_t>(row)];
    max_active_->setValue(q.max_active);
    const auto rule = q.schedule.value_or(scheduler::ScheduleRule{});
    schedule_->setChecked(q.schedule.has_value() && rule.enabled);
    const int a = rule.start_minute.value_or(0), b = rule.stop_minute.value_or(0);
    start_->setTime(QTime(a / 60, a % 60));
    stop_->setTime(QTime(b / 60, b % 60));
    for (int i = 0; i < 7; ++i) days_[i]->setChecked((rule.weekdays_mask >> i) & 1);
    open_label_->setText(q.open ? tr("Status: running allowed now") : tr("Status: outside its window"));
}

void QueuesDialog::apply() {
    const int row = list_->currentRow();
    if (row < 0 || row >= static_cast<int>(queues_.size())) return;
    const auto id = queues_[static_cast<std::size_t>(row)].id;
    engine_.set_queue_limit(id, max_active_->value());
    if (schedule_->isChecked()) {
        scheduler::ScheduleRule r;
        r.start_minute = start_->time().hour() * 60 + start_->time().minute();
        r.stop_minute = stop_->time().hour() * 60 + stop_->time().minute();
        r.weekdays_mask = 0;
        for (int i = 0; i < 7; ++i)
            if (days_[i]->isChecked()) r.weekdays_mask |= static_cast<std::uint8_t>(1 << i);
        engine_.set_queue_schedule(id, r);
    } else {
        engine_.set_queue_schedule(id, std::nullopt);
    }
    reload(id);
}

void QueuesDialog::addQueue() {
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Add queue"), tr("Queue name:"), QLineEdit::Normal, {}, &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    try {
        const auto id = engine_.add_queue(toUtf8(name.trimmed()), 2);
        reload(id);
    } catch (const std::exception& e) {  // e.g. a queue with that name already exists
        QMessageBox::warning(this, tr("Add queue"), tr("Could not add the queue: %1").arg(QString::fromUtf8(e.what())));
    }
}

}  // namespace tdm::ui
