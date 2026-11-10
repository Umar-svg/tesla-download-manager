#pragma once
// Add-download, settings and queue/scheduler dialogs.

#include <QDialog>

#include <optional>

#include "engine/download_engine.hpp"

class QLineEdit;
class QSpinBox;
class QComboBox;
class QCheckBox;
class QListWidget;
class QTimeEdit;
class QDialogButtonBox;
class QLabel;
class QGroupBox;

namespace tdm::ui {

struct UiSettings {
    QString default_folder;
    int connections = 8;
    int max_active = 8;
    std::uint64_t global_limit = 0;  // bytes/s, 0 = unlimited
    static UiSettings load();        // QSettings
    void save() const;
};

class AddDownloadDialog : public QDialog {
    Q_OBJECT
public:
    AddDownloadDialog(const UiSettings& defaults, const std::vector<engine::QueueInfo>& queues,
                      const QString& url, QWidget* parent = nullptr);
    engine::AddRequest request() const;  // valid after accept()

private:
    void validate();
    QLineEdit* url_;
    QLineEdit* folder_;
    QLineEdit* name_;
    QSpinBox* connections_;
    QComboBox* queue_;
    QLineEdit* limit_;
    QLineEdit* sha_;
    QCheckBox* start_now_;
    QLabel* error_;
    QDialogButtonBox* buttons_;
};

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    SettingsDialog(const UiSettings& current, QWidget* parent = nullptr);
    UiSettings settings() const;

private:
    QLineEdit* folder_;
    QSpinBox* connections_;
    QSpinBox* max_active_;
    QLineEdit* limit_;
    QLabel* error_;
    QDialogButtonBox* buttons_;
};

// Lists queues; edits concurrency and schedule; adds queues. Applies changes
// through the engine API immediately.
class QueuesDialog : public QDialog {
    Q_OBJECT
public:
    QueuesDialog(engine::DownloadEngine& engine, QWidget* parent = nullptr);

private:
    void reload(std::int64_t select_id = 0);
    void showQueue(int row);
    void apply();
    void addQueue();

    engine::DownloadEngine& engine_;
    std::vector<engine::QueueInfo> queues_;
    QListWidget* list_;
    QSpinBox* max_active_;
    QGroupBox* schedule_;
    QTimeEdit* start_;
    QTimeEdit* stop_;
    QCheckBox* days_[7];
    QLabel* open_label_;
};

}  // namespace tdm::ui
