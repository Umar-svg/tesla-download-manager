#include "ui/download_model.hpp"

#include <gtest/gtest.h>

using namespace tdm;
using tdm::ui::DownloadTableModel;

namespace {
engine::Snapshot snap(engine::DownloadId id, db::State st, std::uint64_t done, std::optional<std::uint64_t> total) {
    engine::Snapshot s;
    s.id = id;
    s.state = st;
    s.downloaded = done;
    s.total = total;
    s.file_name = "file" + std::to_string(id) + ".bin";
    s.url = "http://h/" + s.file_name;
    return s;
}
}  // namespace

TEST(UiModel, InsertUpdateRemoveKeepsIndexConsistent) {
    DownloadTableModel m;
    m.apply(snap(1, db::State::Downloading, 10, 100));
    m.apply(snap(2, db::State::Queued, 0, std::nullopt));
    m.apply(snap(3, db::State::Paused, 50, 100));
    ASSERT_EQ(m.rowCount(), 3);
    m.apply(snap(1, db::State::Downloading, 60, 100));  // update in place
    EXPECT_EQ(m.rowCount(), 3);
    EXPECT_EQ(m.snapshotById(1)->downloaded, 60u);

    auto removed = snap(2, db::State::Queued, 0, std::nullopt);
    removed.removed = true;
    m.apply(removed);
    ASSERT_EQ(m.rowCount(), 2);
    EXPECT_EQ(m.rowOf(1), 0);
    EXPECT_EQ(m.rowOf(3), 1);  // index rebuilt after removal
    EXPECT_EQ(m.rowOf(2), -1);
    m.apply(removed);  // removing an unknown id is a no-op
    EXPECT_EQ(m.rowCount(), 2);
}

TEST(UiModel, DisplayAndRoles) {
    DownloadTableModel m;
    auto s = snap(7, db::State::Downloading, 25'000'000, 100'000'000);
    s.bytes_per_sec = 5'000'000;
    s.active_connections = 8;
    s.queue_id = 2;
    m.setQueueNames({{1, "Main"}, {2, "Night"}});
    m.apply(s);
    auto at = [&](int col, int role = Qt::DisplayRole) { return m.data(m.index(0, col), role); };
    EXPECT_EQ(at(DownloadTableModel::Name).toString(), "file7.bin");
    EXPECT_EQ(at(DownloadTableModel::Size).toString(), "100 MB");
    EXPECT_EQ(at(DownloadTableModel::Progress).toString(), "25.0%");
    EXPECT_DOUBLE_EQ(at(DownloadTableModel::Progress, DownloadTableModel::ProgressRole).toDouble(), 0.25);
    EXPECT_EQ(at(DownloadTableModel::Speed).toString(), "5.00 MB/s");
    EXPECT_EQ(at(DownloadTableModel::Eta).toString(), "0:15");
    EXPECT_EQ(at(DownloadTableModel::Connections).toString(), "8");
    EXPECT_EQ(at(DownloadTableModel::Status).toString(), "Downloading");
    EXPECT_EQ(at(DownloadTableModel::Queue).toString(), "Night");
    EXPECT_EQ(at(0, DownloadTableModel::IdRole).toLongLong(), 7);
    EXPECT_DOUBLE_EQ(m.totalSpeed(), 5'000'000);
    EXPECT_EQ(m.activeCount(), 1);
}

TEST(UiModel, FailedShowsErrorAndUnknownSizeHasNoBar) {
    DownloadTableModel m;
    auto f = snap(1, db::State::Failed, 0, std::nullopt);
    f.error = "server returned an error status (HTTP 404)";
    m.apply(f);
    m.apply(snap(2, db::State::Downloading, 123456, std::nullopt));
    EXPECT_EQ(m.data(m.index(0, DownloadTableModel::Status), Qt::DisplayRole).toString(),
              "Failed: server returned an error status (HTTP 404)");
    EXPECT_EQ(m.data(m.index(0, 0), Qt::ToolTipRole).toString(), "server returned an error status (HTTP 404)");
    EXPECT_LT(m.data(m.index(1, DownloadTableModel::Progress), DownloadTableModel::ProgressRole).toDouble(), 0);
    EXPECT_EQ(m.data(m.index(1, DownloadTableModel::Progress), Qt::DisplayRole).toString(), "123 KB");
}

TEST(UiModel, ResetSkipsRemoved) {
    DownloadTableModel m;
    auto gone = snap(2, db::State::Completed, 1, 1);
    gone.removed = true;
    m.reset({snap(1, db::State::Completed, 5, 5), gone});
    EXPECT_EQ(m.rowCount(), 1);
    EXPECT_EQ(m.rowOf(1), 0);
}
