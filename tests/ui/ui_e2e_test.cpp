// End-to-end UI tests: real MainWindow (offscreen) + real EngineBridge +
// real Engine + real Python probe server. No mocks.

#include <QAction>
#include <QCheckBox>
#include <QApplication>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QTableView>

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "net/http_client.hpp"
#include "test_support.hpp"
#include "ui/dialogs.hpp"
#include "ui/engine_bridge.hpp"
#include "ui/main_window.hpp"

using namespace tdm;
using namespace std::chrono_literals;
namespace stdfs = std::filesystem;

namespace {

// Spins the Qt event loop until pred() or timeout.
bool wait_until(const std::function<bool()>& pred, std::chrono::milliseconds timeout = 60s) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (pred()) return true;
        std::this_thread::sleep_for(5ms);
    }
    QCoreApplication::processEvents();
    return pred();
}

QAction* action(QWidget& w, const QString& text) {
    for (auto* a : w.findChildren<QAction*>())
        if (a->text() == text) return a;
    return nullptr;
}

}  // namespace

class UiE2E : public ::testing::Test {
protected:
    static void SetUpTestSuite() { server_ = new test::ProbeServer(false); }
    static void TearDownTestSuite() { delete server_; }

    void SetUp() override {
        ASSERT_TRUE(server_->ok());
        dir_ = stdfs::temp_directory_path() / "tdm_ui_tests" /
               ::testing::UnitTest::GetInstance()->current_test_info()->name();
        stdfs::remove_all(dir_);
        stdfs::create_directories(dir_);
    }

    engine::EngineOptions options(std::chrono::milliseconds snapshot = 50ms) {
        engine::EngineOptions o;
        o.db_path = dir_ / "tdm.sqlite";
        o.min_split = 512 * 1024;
        o.snapshot_interval = snapshot;
        o.checkpoint_interval = 300ms;
        return o;
    }
    engine::AddRequest req(const std::string& path) {
        engine::AddRequest r;
        r.url = server_->url(path).to_string();
        r.directory = dir_;
        return r;
    }
    std::string server_sha(const std::string& name, std::uint64_t size) {
        asio::io_context io;
        auto pool = net::ConnectionManager::create(io);
        net::HttpClient c(pool);
        std::string body;
        net::Request r;
        r.url = server_->url("/admin/sha256/" + name + "?size=" + std::to_string(size));
        c.fetch(
            r, {},
            [&](std::span<const std::byte> s) {
                body.append(reinterpret_cast<const char*>(s.data()), s.size());
                return s.size();
            },
            [](std::error_code, std::uint64_t) {});
        io.run();
        return body;
    }
    static std::string file_sha(const stdfs::path& p) {
        std::error_code ec;
        return fs::sha256_file(p, ec);
    }
    static void select(ui::MainWindow& w, engine::DownloadId id) {
        auto* proxy = static_cast<QSortFilterProxyModel*>(w.view()->model());
        const auto src = w.model().index(w.model().rowOf(id), 0);
        w.view()->selectRow(proxy->mapFromSource(src).row());
    }

    static test::ProbeServer* server_;
    stdfs::path dir_;
};

test::ProbeServer* UiE2E::server_ = nullptr;

TEST_F(UiE2E, AddDownloadShowsProgressAndCompletes) {
    const std::uint64_t size = 6'000'000;
    ui::EngineBridge bridge(options());
    {
        ui::MainWindow w(bridge);
        w.show();
        bool finished_ok = false;
        int finished_count = 0;
        QObject::connect(&w, &ui::MainWindow::downloadFinished, [&](qint64, bool ok) {
            finished_ok = ok;
            ++finished_count;
        });
        bool saw_progress = false;
        const auto id = bridge.engine().add(req("/f/ui1.bin?rate=3000000&size=" + std::to_string(size)));
        ASSERT_TRUE(wait_until([&] {
            const auto* s = w.model().snapshotById(id);
            if (s && s->state == db::State::Downloading && s->downloaded > 0 && s->downloaded < size)
                saw_progress = true;
            return s && s->state == db::State::Completed;
        }));
        EXPECT_TRUE(saw_progress) << "the model never showed an in-progress state";
        EXPECT_TRUE(finished_ok);
        EXPECT_EQ(finished_count, 1);
        const int row = w.model().rowOf(id);
        EXPECT_EQ(w.model().data(w.model().index(row, ui::DownloadTableModel::Status), Qt::DisplayRole).toString(),
                  "Completed");
        EXPECT_DOUBLE_EQ(
            w.model().data(w.model().index(row, 0), ui::DownloadTableModel::ProgressRole).toDouble(), 1.0);
        const auto* s = w.model().snapshotById(id);
        ASSERT_FALSE(s->file_path.empty());
        EXPECT_EQ(file_sha(stdfs::path(std::u8string(reinterpret_cast<const char8_t*>(s->file_path.data()),
                                                     s->file_path.size()))),
                  server_sha("ui1.bin", size));
        select(w, id);
        EXPECT_FALSE(w.grab().isNull());  // paints table, progress delegate and segment bar offscreen

        bridge.engine().remove(id, false);
        ASSERT_TRUE(wait_until([&] { return w.model().rowOf(id) == -1; }));
        EXPECT_TRUE(stdfs::exists(dir_ / "ui1.bin"));  // "keep files"
    }  // window first, then bridge (engine shutdown)
}

TEST_F(UiE2E, PauseAndResumeThroughToolbarActions) {
    const std::uint64_t size = 16'000'000;
    ui::EngineBridge bridge(options());
    ui::MainWindow w(bridge);
    w.show();
    const auto id = bridge.engine().add(req("/f/ui2.bin?rate=2000000&size=" + std::to_string(size)));
    ASSERT_TRUE(wait_until([&] {
        const auto* s = w.model().snapshotById(id);
        return s && s->downloaded > (2u << 20);
    }));
    select(w, id);
    auto* pause = action(w, "Pause");
    auto* resume = action(w, "Resume");
    ASSERT_TRUE(pause && resume);
    EXPECT_TRUE(pause->isEnabled());
    pause->trigger();
    ASSERT_TRUE(wait_until([&] { return w.model().snapshotById(id)->state == db::State::Paused; }));
    const auto at_pause = w.model().snapshotById(id)->downloaded;
    EXPECT_GT(at_pause, 2u << 20);
    EXPECT_TRUE(resume->isEnabled());
    EXPECT_FALSE(pause->isEnabled());
    resume->trigger();
    ASSERT_TRUE(wait_until([&] { return w.model().snapshotById(id)->state == db::State::Completed; }));
    EXPECT_EQ(file_sha(dir_ / "ui2.bin"), server_sha("ui2.bin", size));
}

// Progress events must be coalesced: while the UI thread is busy, the engine
// keeps reporting, but the UI receives a single batched event afterwards.
TEST_F(UiE2E, CoalescesProgressEventsWhileUiIsBusy) {
    ui::EngineBridge bridge(options(5ms));  // very chatty engine
    ui::MainWindow w(bridge);
    const auto id = bridge.engine().add(req("/f/ui3.bin?rate=1000000&size=40000000"));
    ASSERT_TRUE(wait_until([&] {
        const auto* s = w.model().snapshotById(id);
        return s && s->state == db::State::Downloading && s->downloaded > 0;
    }));
    QCoreApplication::processEvents();
    const auto r0 = bridge.snapshotsReceived();
    const auto f0 = bridge.flushesDelivered();
    std::this_thread::sleep_for(600ms);  // UI thread blocked: no event processing
    QCoreApplication::processEvents();
    const auto received = bridge.snapshotsReceived() - r0;
    const auto flushed = bridge.flushesDelivered() - f0;
    std::printf("[   info   ] while busy: %llu engine snapshots -> %llu UI flush(es)\n",
                static_cast<unsigned long long>(received), static_cast<unsigned long long>(flushed));
    EXPECT_GE(received, 30u);
    EXPECT_LE(flushed, 2u);
    bridge.engine().remove(id, true);
    ASSERT_TRUE(wait_until([&] { return w.model().rowOf(id) == -1; }));
}

// Closing the app mid-download (window, then bridge) must save progress; a new
// session restores the row and finishes the same file.
TEST_F(UiE2E, ShutdownMidDownloadThenResumeInNewSession) {
    const std::uint64_t size = 20'000'000;
    engine::DownloadId id = 0;
    std::uint64_t before = 0;
    {
        auto bridge = std::make_unique<ui::EngineBridge>(options());
        auto w = std::make_unique<ui::MainWindow>(*bridge);
        w->show();
        id = bridge->engine().add(req("/f/ui4.bin?rate=2000000&size=" + std::to_string(size)));
        ASSERT_TRUE(wait_until([&] {
            const auto* s = w->model().snapshotById(id);
            return s && s->downloaded > (3u << 20);
        }));
        before = w->model().snapshotById(id)->downloaded;
        w.reset();       // UI goes first...
        bridge.reset();  // ...then the engine shuts down and saves resume state
    }
    ui::EngineBridge bridge(options());
    ui::MainWindow w(bridge);
    const auto* restored = w.model().snapshotById(id);
    ASSERT_NE(restored, nullptr) << "row not restored from the database";
    EXPECT_GE(restored->downloaded, before / 2);  // checkpointed progress (>= last checkpoint)
    ASSERT_TRUE(wait_until([&] {
        const auto* s = w.model().snapshotById(id);
        return s && s->state == db::State::Completed;
    }));
    EXPECT_EQ(file_sha(dir_ / "ui4.bin"), server_sha("ui4.bin", size));
}

TEST_F(UiE2E, AddDialogValidatesAndBuildsRequest) {
    ui::UiSettings d;
    d.default_folder = QString::fromStdWString(dir_.wstring());
    d.connections = 5;
    std::vector<engine::QueueInfo> queues = {{1, "Main", 3, std::nullopt, true}, {4, "Night", 1, std::nullopt, true}};
    ui::AddDownloadDialog ok_dlg(d, queues, QStringLiteral("https://example.com/a.iso"));
    auto* ok_btn = ok_dlg.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);
    EXPECT_TRUE(ok_btn->isEnabled());
    const auto r = ok_dlg.request();
    EXPECT_EQ(r.url, "https://example.com/a.iso");
    EXPECT_EQ(r.connections, 5);
    EXPECT_EQ(r.queue_id, 1);
    EXPECT_EQ(r.directory, dir_);
    EXPECT_TRUE(r.start_now);
    EXPECT_FALSE(r.speed_limit_bps);
    EXPECT_FALSE(r.allow_web_page);  // off by default: web pages are reported
    QCheckBox* allow = nullptr;
    for (auto* c : ok_dlg.findChildren<QCheckBox*>())
        if (c->text().contains(QStringLiteral("web page"))) allow = c;
    ASSERT_NE(allow, nullptr);
    allow->setChecked(true);
    EXPECT_TRUE(ok_dlg.request().allow_web_page);

    ui::AddDownloadDialog bad(d, queues, QStringLiteral("not a url"));
    EXPECT_FALSE(bad.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->isEnabled());
}
