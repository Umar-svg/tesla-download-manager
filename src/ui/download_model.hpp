#pragma once
// Table model of all downloads, fed by engine snapshots (UI thread only).

#include <QAbstractTableModel>
#include <QHash>

#include <map>
#include <optional>
#include <vector>

#include "engine/download_engine.hpp"

Q_DECLARE_METATYPE(tdm::engine::Snapshot)

namespace tdm::ui {

class DownloadTableModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { Name, Size, Progress, Speed, Eta, Connections, Status, Queue, ColumnCount };
    enum Role {
        IdRole = Qt::UserRole + 1,
        ProgressRole,  // double 0..1, or -1 when the size is unknown
        SortRole,      // numeric value for sorting
        StateRole,     // int(db::State)
    };

    explicit DownloadTableModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation o, int role) const override;

    void apply(const engine::Snapshot& s);  // insert, update or remove a row
    void reset(const std::vector<engine::Snapshot>& all);
    void setQueueNames(std::map<std::int64_t, QString> names);

    const engine::Snapshot* snapshotAt(int row) const;
    const engine::Snapshot* snapshotById(engine::DownloadId id) const;
    int rowOf(engine::DownloadId id) const;

    double totalSpeed() const;
    int activeCount() const;

private:
    std::vector<engine::Snapshot> rows_;
    QHash<qint64, int> index_;  // id → row
    std::map<std::int64_t, QString> queue_names_;
};

}  // namespace tdm::ui
