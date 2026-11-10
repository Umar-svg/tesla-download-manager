#include "ui/download_model.hpp"

#include "ui/format.hpp"

namespace tdm::ui {

DownloadTableModel::DownloadTableModel(QObject* parent) : QAbstractTableModel(parent) {}

int DownloadTableModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int DownloadTableModel::columnCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : ColumnCount; }

QVariant DownloadTableModel::headerData(int section, Qt::Orientation o, int role) const {
    if (o != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
        case Name: return tr("File name");
        case Size: return tr("Size");
        case Progress: return tr("Progress");
        case Speed: return tr("Speed");
        case Eta: return tr("Time left");
        case Connections: return tr("Connections");
        case Status: return tr("Status");
        case Queue: return tr("Queue");
    }
    return {};
}

QVariant DownloadTableModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= static_cast<int>(rows_.size())) return {};
    const auto& s = rows_[static_cast<std::size_t>(index.row())];
    const double progress =
        s.total && *s.total > 0 ? static_cast<double>(s.downloaded) / static_cast<double>(*s.total)
                                : (s.state == db::State::Completed ? 1.0 : -1.0);
    switch (role) {
        case IdRole: return QVariant::fromValue<qint64>(s.id);
        case ProgressRole: return progress;
        case StateRole: return static_cast<int>(s.state);
        case Qt::ToolTipRole:
            if (!s.error.empty()) return fromUtf8(s.error);
            return fromUtf8(s.url);
        case Qt::TextAlignmentRole:
            if (index.column() == Size || index.column() == Speed || index.column() == Eta ||
                index.column() == Connections)
                return QVariant(Qt::AlignRight | Qt::AlignVCenter);
            return {};
        case SortRole:
            switch (index.column()) {
                case Size: return QVariant::fromValue<qulonglong>(s.total.value_or(0));
                case Progress: return progress;
                case Speed: return s.bytes_per_sec;
                case Eta: {
                    if (!s.total || s.bytes_per_sec < 1) return 1e18;
                    return static_cast<double>(*s.total - std::min(*s.total, s.downloaded)) / s.bytes_per_sec;
                }
                case Connections: return s.active_connections;
                case Status: return static_cast<int>(s.state);
                default: return data(index, Qt::DisplayRole);
            }
        case Qt::DisplayRole:
            switch (index.column()) {
                case Name: {
                    if (!s.file_name.empty()) return fromUtf8(s.file_name);
                    return fromUtf8(s.url);
                }
                case Size: return s.total ? formatBytes(*s.total) : QString();
                case Progress:
                    return progress >= 0 ? QStringLiteral("%1%").arg(progress * 100.0, 0, 'f', 1)
                                         : formatBytes(s.downloaded);
                case Speed: return s.state == db::State::Downloading ? formatSpeed(s.bytes_per_sec) : QString();
                case Eta:
                    return s.state == db::State::Downloading ? formatEta(s.total, s.downloaded, s.bytes_per_sec)
                                                             : QString();
                case Connections: return s.active_connections > 0 ? QString::number(s.active_connections) : QString();
                case Status: {
                    QString st = stateName(s.state);
                    if (s.state == db::State::Failed && !s.error.empty()) st += QStringLiteral(": ") + fromUtf8(s.error);
                    return st;
                }
                case Queue: {
                    auto q = queue_names_.find(s.queue_id);
                    return q != queue_names_.end() ? q->second : QString::number(s.queue_id);
                }
            }
            return {};
        default:
            return {};
    }
}

void DownloadTableModel::apply(const engine::Snapshot& s) {
    const auto it = index_.find(s.id);
    if (s.removed) {
        if (it == index_.end()) return;
        const int row = *it;
        beginRemoveRows({}, row, row);
        rows_.erase(rows_.begin() + row);
        endRemoveRows();
        index_.clear();
        for (int i = 0; i < static_cast<int>(rows_.size()); ++i) index_[rows_[static_cast<std::size_t>(i)].id] = i;
        return;
    }
    if (it == index_.end()) {
        const int row = static_cast<int>(rows_.size());
        beginInsertRows({}, row, row);
        rows_.push_back(s);
        index_[s.id] = row;
        endInsertRows();
        return;
    }
    rows_[static_cast<std::size_t>(*it)] = s;
    emit dataChanged(index(*it, 0), index(*it, ColumnCount - 1));
}

void DownloadTableModel::reset(const std::vector<engine::Snapshot>& all) {
    beginResetModel();
    rows_.clear();
    index_.clear();
    for (const auto& s : all) {
        if (s.removed) continue;
        index_[s.id] = static_cast<int>(rows_.size());
        rows_.push_back(s);
    }
    endResetModel();
}

void DownloadTableModel::setQueueNames(std::map<std::int64_t, QString> names) {
    queue_names_ = std::move(names);
    if (!rows_.empty()) emit dataChanged(index(0, Queue), index(rowCount() - 1, Queue));
}

const engine::Snapshot* DownloadTableModel::snapshotAt(int row) const {
    return row >= 0 && row < static_cast<int>(rows_.size()) ? &rows_[static_cast<std::size_t>(row)] : nullptr;
}

const engine::Snapshot* DownloadTableModel::snapshotById(engine::DownloadId id) const {
    const auto it = index_.find(id);
    return it == index_.end() ? nullptr : &rows_[static_cast<std::size_t>(*it)];
}

int DownloadTableModel::rowOf(engine::DownloadId id) const {
    const auto it = index_.find(id);
    return it == index_.end() ? -1 : *it;
}

double DownloadTableModel::totalSpeed() const {
    double t = 0;
    for (const auto& s : rows_)
        if (s.state == db::State::Downloading) t += s.bytes_per_sec;
    return t;
}

int DownloadTableModel::activeCount() const {
    int n = 0;
    for (const auto& s : rows_)
        if (s.state == db::State::Downloading) ++n;
    return n;
}

}  // namespace tdm::ui
