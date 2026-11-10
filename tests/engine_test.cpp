// Engine scenarios against the probe server: completion, persistence across
// engine restarts, pause/resume, retry after 503, cancel, concurrency limit.

#include "engine/engine.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <ctime>
#include <set>
#include <thread>

using namespace tdm;
using namespace tdm::engine;
using namespace std::chrono_literals;
namespace stdfs = std::filesystem;

class EngineTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() { server_ = new test::ProbeServer(false); }
    static void TearDownTestSuite() { delete server_; }

    void SetUp() override {
        ASSERT_TRUE(server_->ok());
        dir_ = stdfs::temp_directory_path() / "tdm_tests" /
               ::testing::UnitTest::GetInstance()->current_test_info()->name();
        stdfs::remove_all(dir_);
        stdfs::create_directories(dir_);
        db_ = dir_ / "tdm.sqlite";
    }

    EngineOptions options() {
        EngineOptions o;
        o.db_path = db_;
        o.min_split = 512 * 1024;
        o.checkpoint_interval = 300ms;
        o.snapshot_interval = 50ms;
        o.retry.base = 200ms;
        return o;
    }
    AddRequest add_req(const std::string& path) {
        AddRequest a;
        a.url = server_->url(path).to_string();
        a.directory = dir_;
        return a;
    }
    static Snapshot wait_for(Engine& e, DownloadId id, std::function<bool(const Snapshot&)> pred,
                             std::chrono::seconds timeout = 60s) {
        const auto end = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < end) {
            auto s = e.snapshot(id);
            if (s && pred(*s)) return *s;
            std::this_thread::sleep_for(10ms);
        }
        auto s = e.snapshot(id);
        ADD_FAILURE() << "timed out; state=" << (s ? static_cast<int>(s->state) : -1)
                      << " error=" << (s ? s->error : "");
        return s.value_or(Snapshot{});
    }
    static bool done(const Snapshot& s) {
        return s.state == db::State::Completed || s.state == db::State::Failed || s.state == db::State::Cancelled;
    }
    std::string server_sha(const std::string& name, std::uint64_t size) {
        // Probe server SHA endpoint through a throwaway engine-independent client.
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
    db::DownloadRow row_of(DownloadId id) {
        db::Store s(db_.string());
        return *s.get_download(id);
    }

    static test::ProbeServer* server_;
    stdfs::path dir_, db_;
};

test::ProbeServer* EngineTest::server_ = nullptr;

TEST_F(EngineTest, AddDownloadsAndPersistsCompletion) {
    const std::uint64_t size = 8u << 20;
    DownloadId id = 0;
    std::atomic<int> notified{0};
    {
        Engine e(options());
        e.subscribe([&](const Snapshot&) { ++notified; });
        id = e.add(add_req("/f/e1.bin?rate=8000000&size=" + std::to_string(size)));
        auto s = wait_for(e, id, done);
        ASSERT_EQ(s.state, db::State::Completed) << s.error;
        EXPECT_EQ(s.downloaded, size);
    }
    EXPECT_GT(notified.load(), 2);
    const auto row = row_of(id);  // persisted after the engine is gone
    EXPECT_EQ(row.state, db::State::Completed);
    EXPECT_EQ(row.sha256_actual, server_sha("e1.bin", size));
    EXPECT_EQ(row.file_name, "e1.bin");
    EXPECT_TRUE(row.part_path.empty());
    EXPECT_EQ(stdfs::file_size(dir_ / "e1.bin"), size);
    db::Store st(db_.string());
    EXPECT_TRUE(st.load_segments(id).empty());
}

TEST_F(EngineTest, PauseSurvivesEngineRestartAndResumes) {
    const std::uint64_t size = 32u << 20;
    DownloadId id = 0;
    std::uint64_t at_pause = 0;
    {
        Engine e(options());
        id = e.add(add_req("/f/e2.bin?rate=2000000&size=" + std::to_string(size)));
        wait_for(e, id, [](const Snapshot& s) { return s.downloaded > (6u << 20); });
        e.pause(id);
        auto s = wait_for(e, id, [](const Snapshot& s) { return s.state == db::State::Paused; });
        at_pause = s.downloaded;
        EXPECT_GT(at_pause, 6u << 20);
    }
    {
        const auto row = row_of(id);
        EXPECT_EQ(row.state, db::State::Paused);
        EXPECT_FALSE(row.part_path.empty());
        EXPECT_TRUE(stdfs::exists(stdfs::path(row.part_path)));
    }
    Engine e(options());
    auto s0 = e.snapshot(id);
    ASSERT_TRUE(s0);
    EXPECT_EQ(s0->state, db::State::Paused);  // paused downloads stay paused
    EXPECT_EQ(s0->downloaded, at_pause);      // progress reloaded from the segment table
    e.start(id);
    auto s = wait_for(e, id, done);
    ASSERT_EQ(s.state, db::State::Completed) << s.error;
    e.shutdown();
    EXPECT_EQ(row_of(id).sha256_actual, server_sha("e2.bin", size));
}

TEST_F(EngineTest, ShutdownMidDownloadResumesOnNextStart) {
    const std::uint64_t size = 24u << 20;
    DownloadId id = 0;
    {
        Engine e(options());
        id = e.add(add_req("/f/e3.bin?rate=2000000&size=" + std::to_string(size)));
        wait_for(e, id, [](const Snapshot& s) { return s.downloaded > (4u << 20); });
    }  // destructor = graceful shutdown while downloading
    EXPECT_EQ(row_of(id).state, db::State::Queued);
    Engine e(options());  // resume_on_start
    auto s = wait_for(e, id, done);
    ASSERT_EQ(s.state, db::State::Completed) << s.error;
    e.shutdown();
    EXPECT_EQ(row_of(id).sha256_actual, server_sha("e3.bin", size));
}

TEST_F(EngineTest, CheckpointsAreWrittenWhileRunning) {
    const std::uint64_t size = 24u << 20;
    Engine e(options());
    auto id = e.add(add_req("/f/e4.bin?rate=2000000&size=" + std::to_string(size)));
    wait_for(e, id, [](const Snapshot& s) { return s.downloaded > (6u << 20); });
    std::this_thread::sleep_for(700ms);  // > 2 checkpoint intervals
    {
        db::Store st(db_.string());  // a second connection reads the WAL database
        const auto segs = st.load_segments(id);
        ASSERT_FALSE(segs.empty()) << "no checkpoint persisted while running";
        std::uint64_t claimed = 0;
        for (const auto& sg : segs) claimed += sg.pos - sg.begin;
        EXPECT_GT(claimed, 0u);
        EXPECT_FALSE(st.get_download(id)->part_path.empty());
    }
    auto s = wait_for(e, id, done);
    EXPECT_EQ(s.state, db::State::Completed) << s.error;
}

TEST_F(EngineTest, RetriesAfter503UsingRetryAfter) {
    Engine e(options());
    const auto t0 = std::chrono::steady_clock::now();
    std::set<db::State> seen;
    std::mutex m;
    e.subscribe([&](const Snapshot& s) {
        std::lock_guard lk(m);
        seen.insert(s.state);
    });
    auto id = e.add(add_req("/f/e5.bin?size=100000&busy=1&retry_after=1"));
    auto s = wait_for(e, id, done);
    ASSERT_EQ(s.state, db::State::Completed) << s.error;
    EXPECT_GE(std::chrono::steady_clock::now() - t0, 1s);
    std::lock_guard lk(m);
    EXPECT_TRUE(seen.count(db::State::Waiting));
}

TEST_F(EngineTest, NotFoundFailsWithoutRetrying) {
    Engine e(options());
    auto id = e.add(add_req("/missing"));
    auto s = wait_for(e, id, done);
    EXPECT_EQ(s.state, db::State::Failed);
    EXPECT_NE(s.error.find("404"), std::string::npos) << s.error;
}

TEST_F(EngineTest, CancelRemovesPartialFile) {
    Engine e(options());
    auto id = e.add(add_req("/f/e6.bin?rate=1000000&size=50000000"));
    wait_for(e, id, [](const Snapshot& s) { return s.downloaded > (2u << 20); });
    e.cancel(id, true);
    auto s = wait_for(e, id, [](const Snapshot& s) { return s.state == db::State::Cancelled; });
    EXPECT_EQ(s.downloaded, 0u);
    e.shutdown();
    EXPECT_FALSE(stdfs::exists(dir_ / "e6.bin.tdmpart"));
    EXPECT_FALSE(stdfs::exists(dir_ / "e6.bin"));
    EXPECT_TRUE(row_of(id).part_path.empty());
}

TEST_F(EngineTest, MaxActiveLimitsConcurrentDownloads) {
    auto o = options();
    o.max_active = 2;
    Engine e(o);
    std::atomic<int> max_seen{0};
    std::vector<DownloadId> ids;
    for (int i = 0; i < 5; ++i)
        ids.push_back(e.add(add_req("/f/m" + std::to_string(i) + ".bin?rate=4000000&size=3000000")));
    bool all_done = false;
    const auto end = std::chrono::steady_clock::now() + 60s;
    while (!all_done && std::chrono::steady_clock::now() < end) {
        int active = 0;
        all_done = true;
        for (auto& s : e.snapshots()) {
            if (s.state == db::State::Downloading) ++active;
            if (!done(s)) all_done = false;
        }
        max_seen = std::max(max_seen.load(), active);
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_TRUE(all_done);
    EXPECT_LE(max_seen.load(), 2);
    EXPECT_EQ(max_seen.load(), 2);
    for (auto id : ids) EXPECT_EQ(e.snapshot(id)->state, db::State::Completed);
}

TEST_F(EngineTest, RejectsUnsupportedUrl) {
    Engine e(options());
    AddRequest a;
    a.url = "ftp://example.com/x";
    a.directory = dir_;
    EXPECT_THROW(e.add(a), std::invalid_argument);
}

TEST_F(EngineTest, GlobalLimitAppliesLiveAndPersists) {
    const std::uint64_t size = 40'000'000, limit = 2'000'000;
    DownloadId id = 0;
    {
        Engine e(options());
        e.set_global_speed_limit(limit);
        id = e.add(add_req("/f/gl.bin?size=" + std::to_string(size)));
        wait_for(e, id, [](const Snapshot& s) { return s.downloaded > 0; });
        const auto a = e.snapshot(id)->downloaded;
        const auto t0 = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(2s);
        const auto b = e.snapshot(id)->downloaded;
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const double rate = static_cast<double>(b - a) / secs;
        std::printf("[   info   ] engine global limit %llu B/s -> measured %.0f B/s\n",
                    static_cast<unsigned long long>(limit), rate);
        EXPECT_GT(rate, 0.80 * static_cast<double>(limit));  // 2 s window + 250 ms snapshot granularity
        EXPECT_LT(rate, 1.20 * static_cast<double>(limit));
        e.set_global_speed_limit(std::nullopt);  // lift it: the rest finishes quickly
        auto s = wait_for(e, id, done, 30s);
        EXPECT_EQ(s.state, db::State::Completed) << s.error;
        e.set_global_speed_limit(limit);
    }
    db::Store st(db_.string());
    EXPECT_EQ(*st.get_setting("global_speed_limit"), std::to_string(limit));
}

TEST_F(EngineTest, PerQueueConcurrencyLimits) {
    auto o = options();
    o.max_active = 8;
    Engine e(o);
    e.set_queue_limit(1, 1);
    const auto q2 = e.add_queue("Second", 1);
    std::vector<DownloadId> ids;
    for (int i = 0; i < 2; ++i) {
        ids.push_back(e.add(add_req("/f/qa" + std::to_string(i) + ".bin?rate=3000000&size=2500000")));
        auto r = add_req("/f/qb" + std::to_string(i) + ".bin?rate=3000000&size=2500000");
        r.queue_id = q2;
        ids.push_back(e.add(r));
    }
    int max_q1 = 0, max_q2 = 0, max_total = 0;
    const auto end = std::chrono::steady_clock::now() + 60s;
    bool all = false;
    while (!all && std::chrono::steady_clock::now() < end) {
        int a1 = 0, a2 = 0;
        all = true;
        for (std::size_t i = 0; i < ids.size(); ++i) {
            auto s = *e.snapshot(ids[i]);
            if (s.state == db::State::Downloading) (i % 2 == 0 ? a1 : a2)++;
            if (!done(s)) all = false;
        }
        max_q1 = std::max(max_q1, a1);
        max_q2 = std::max(max_q2, a2);
        max_total = std::max(max_total, a1 + a2);
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_TRUE(all);
    EXPECT_EQ(max_q1, 1);
    EXPECT_EQ(max_q2, 1);
    EXPECT_EQ(max_total, 2);  // both queues ran in parallel
    for (auto id : ids) EXPECT_EQ(e.snapshot(id)->state, db::State::Completed);
    const auto qs = e.queues();
    ASSERT_EQ(qs.size(), 2u);
    EXPECT_EQ(qs[1].name, "Second");
}

TEST_F(EngineTest, ScheduleWindowStartsAndStopsQueue) {
    // Injected clock: local 09:00 today, moved by the test.
    std::time_t now_t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now_t);
    auto at = [&](int h, int m) {
        std::tm t = tm;
        t.tm_hour = h;
        t.tm_min = m;
        t.tm_sec = 0;
        t.tm_isdst = -1;
        return std::chrono::system_clock::from_time_t(std::mktime(&t));
    };
    std::mutex cm;
    auto fake = at(9, 0);
    auto o = options();
    o.now = [&] {
        std::lock_guard lk(cm);
        return fake;
    };
    auto set_time = [&](int h, int m) {
        std::lock_guard lk(cm);
        fake = at(h, m);
    };
    Engine e(o);
    scheduler::ScheduleRule rule;
    rule.start_minute = 10 * 60;
    rule.stop_minute = 11 * 60;
    e.set_queue_schedule(1, rule);
    EXPECT_FALSE(e.queues()[0].open);

    const std::uint64_t size = 16'000'000;
    auto id = e.add(add_req("/f/sch.bin?rate=2000000&size=" + std::to_string(size)));
    std::this_thread::sleep_for(400ms);
    EXPECT_EQ(e.snapshot(id)->state, db::State::Queued);  // window closed

    set_time(10, 30);
    wait_for(e, id, [](const Snapshot& s) { return s.downloaded > (3u << 20); });

    set_time(11, 30);  // window closes: back to Queued, progress kept
    auto q = wait_for(e, id, [](const Snapshot& s) { return s.state == db::State::Queued; });
    EXPECT_GT(q.downloaded, 3u << 20);
    std::this_thread::sleep_for(400ms);
    EXPECT_EQ(e.snapshot(id)->state, db::State::Queued);

    set_time(10, 45);
    auto s = wait_for(e, id, done);
    ASSERT_EQ(s.state, db::State::Completed) << s.error;
    e.shutdown();
    EXPECT_EQ(row_of(id).sha256_actual, server_sha("sch.bin", size));
}

TEST_F(EngineTest, RemoveRunningAndCompletedDownloads) {
    Engine e(options());
    std::vector<Snapshot> removed;
    std::mutex m;
    e.subscribe([&](const Snapshot& s) {
        if (!s.removed) return;
        std::lock_guard lk(m);
        removed.push_back(s);
    });
    // Running: stopped first, partial file deleted, row gone.
    auto running = e.add(add_req("/f/rm1.bin?rate=1000000&size=30000000"));
    wait_for(e, running, [](const Snapshot& s) { return s.downloaded > (1u << 20); });
    e.remove(running, false);
    const auto end = std::chrono::steady_clock::now() + 20s;
    while (e.snapshot(running) && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(10ms);
    EXPECT_FALSE(e.snapshot(running));
    EXPECT_FALSE(stdfs::exists(dir_ / "rm1.bin.tdmpart"));

    // Completed, removed with delete_file: the file goes too.
    auto done_id = e.add(add_req("/f/rm2.bin?size=100000"));
    ASSERT_EQ(wait_for(e, done_id, done).state, db::State::Completed);
    ASSERT_TRUE(stdfs::exists(dir_ / "rm2.bin"));
    e.remove(done_id, true);
    EXPECT_FALSE(e.snapshot(done_id));
    EXPECT_FALSE(stdfs::exists(dir_ / "rm2.bin"));
    e.shutdown();

    db::Store st(db_.string());
    EXPECT_FALSE(st.get_download(running));
    EXPECT_FALSE(st.get_download(done_id));
    std::lock_guard lk(m);
    ASSERT_EQ(removed.size(), 2u);  // observers were told, so a UI can drop the rows
    EXPECT_EQ(removed[0].id, running);
    EXPECT_EQ(removed[1].id, done_id);
}

TEST_F(EngineTest, SnapshotsCarryDescriptiveFields) {
    Engine e(options());
    auto r = add_req("/f/desc.bin?size=1000");
    r.speed_limit_bps = 123456;
    auto id = e.add(r);
    auto s = wait_for(e, id, done);
    EXPECT_EQ(s.url, r.url);
    EXPECT_EQ(s.queue_id, 1);
    EXPECT_EQ(s.speed_limit, 123456u);
    EXPECT_EQ(stdfs::path(s.file_path), dir_ / "desc.bin");
}

// Regression (Phase 8 benchmark): the engine's connection pool used to cap
// every host at 8, so a 16-connection download silently got 8. With the
// server capping each connection at 2 MB/s, 24 MB takes ~0.75 s with 16 real
// connections and ~1.5 s with 8.
TEST_F(EngineTest, SixteenConnectionsAreReallyUsed) {
    Engine e(options());
    auto r = add_req("/f/c16.bin?rate=2000000&size=24000000");
    r.connections = 16;
    const auto t0 = std::chrono::steady_clock::now();
    auto id = e.add(r);
    int max_active = 0;
    Snapshot s;
    const auto end = t0 + 30s;
    while (std::chrono::steady_clock::now() < end) {
        s = *e.snapshot(id);
        max_active = std::max(max_active, s.active_connections);
        if (done(s)) break;
        std::this_thread::sleep_for(5ms);
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    ASSERT_EQ(s.state, db::State::Completed) << s.error;
    std::printf("[   info   ] 16 connections: %.2f s, max active %d\n", secs, max_active);
    EXPECT_GT(max_active, 8);
    EXPECT_LE(max_active, 16);
    EXPECT_LT(secs, 1.25);  // 8 connections cannot do it in under ~1.5 s
}

TEST_F(EngineTest, WebPageLinkFailsClearlyWithoutRetry) {
    Engine e(options());
    std::set<db::State> seen;
    std::mutex m;
    e.subscribe([&](const Snapshot& s) {
        std::lock_guard lk(m);
        seen.insert(s.state);
    });
    auto id = e.add(add_req("/f/watch?size=80000&nodisp=1&ctype=text/html"));
    auto s = wait_for(e, id, done);
    EXPECT_EQ(s.state, db::State::Failed);
    EXPECT_NE(s.error.find("web page"), std::string::npos) << s.error;
    {
        std::lock_guard lk(m);
        EXPECT_FALSE(seen.count(db::State::Waiting)) << "a web page must not be retried";
    }
    EXPECT_FALSE(stdfs::exists(dir_ / "watch"));

    auto r = add_req("/f/watch?size=80000&nodisp=1&ctype=text/html");
    r.allow_web_page = true;
    auto id2 = e.add(r);
    auto s2 = wait_for(e, id2, done);
    EXPECT_EQ(s2.state, db::State::Completed) << s2.error;
    EXPECT_EQ(stdfs::path(s2.file_path), dir_ / "watch.html");
    e.shutdown();
    EXPECT_TRUE(row_of(id2).allow_web_page);  // persisted for restarts
}
