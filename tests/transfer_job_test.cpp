// TransferJob scenarios against the probe server: segmented and single-stream
// downloads, version change mid-download, stop/resume, error statuses.

#include "multipart/transfer_job.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <map>
#include <set>
#include <future>
#include <thread>

using namespace tdm;
using namespace tdm::multipart;
using namespace std::chrono_literals;
namespace stdfs = std::filesystem;

namespace {

stdfs::path fresh_dir(const std::string& name) {
    auto d = stdfs::temp_directory_path() / "tdm_tests" / name;
    stdfs::remove_all(d);
    stdfs::create_directories(d);
    return d;
}

}  // namespace

class TransferJobTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() { server_ = new test::ProbeServer(false); }
    static void TearDownTestSuite() { delete server_; }

    void SetUp() override {
        ASSERT_TRUE(server_->ok());
        guard_.emplace(asio::make_work_guard(io_));
        for (int i = 0; i < 4; ++i) threads_.emplace_back([this] { io_.run(); });
        pool_ = net::ConnectionManager::create(io_);
        client_ = std::make_unique<net::HttpClient>(pool_);
        // Separate pool for the test's own helper requests (server hashes,
        // version bumps): a job under test may hold every slot of pool_.
        helper_pool_ = net::ConnectionManager::create(io_);
        helper_client_ = std::make_unique<net::HttpClient>(helper_pool_);
        dir_ = fresh_dir(::testing::UnitTest::GetInstance()->current_test_info()->name());
    }
    void TearDown() override {
        client_.reset();
        helper_client_.reset();
        pool_->close_idle();
        helper_pool_->close_idle();
        guard_.reset();
        io_.stop();
        for (auto& t : threads_) t.join();
    }

    TransferOptions opts(const std::string& path) {
        TransferOptions o;
        o.url = server_->url(path);
        o.directory = dir_;
        o.min_split = 512 * 1024;
        return o;
    }

    // Runs a job; `watch` is polled every 20 ms with the job and its progress.
    TransferResult run(TransferOptions o,
                       std::function<void(TransferJob&, const TransferProgress&)> watch = {}) {
        std::promise<TransferResult> p;
        auto f = p.get_future();
        auto job = TransferJob::create(io_, *client_, std::move(o), [&p](const TransferResult& r) { p.set_value(r); });
        job->start();
        while (f.wait_for(20ms) != std::future_status::ready)
            if (watch) watch(*job, job->progress());
        return f.get();
    }

    std::string get(const std::string& path) {  // small helper GET returning the body
        std::promise<void> done;
        std::string body;
        net::Request r;
        r.url = server_->url(path);
        helper_client_->fetch(
            r, {},
            [&](std::span<const std::byte> s) {
                body.append(reinterpret_cast<const char*>(s.data()), s.size());
                return s.size();
            },
            [&](std::error_code, std::uint64_t) { done.set_value(); });
        done.get_future().wait();
        return body;
    }
    std::string server_sha(const std::string& name, std::uint64_t size) {
        return get("/admin/sha256/" + name + "?size=" + std::to_string(size));
    }

    static test::ProbeServer* server_;
    asio::io_context io_;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> guard_;
    std::vector<std::thread> threads_;
    std::shared_ptr<net::ConnectionManager> pool_;
    std::unique_ptr<net::HttpClient> client_;
    std::shared_ptr<net::ConnectionManager> helper_pool_;
    std::unique_ptr<net::HttpClient> helper_client_;
    stdfs::path dir_;
};

test::ProbeServer* TransferJobTest::server_ = nullptr;

TEST_F(TransferJobTest, SegmentedDownloadMatchesServerHash) {
    const std::uint64_t size = 24u << 20;
    auto r = run(opts("/f/seg.bin?rate=4000000&size=" + std::to_string(size)));
    ASSERT_FALSE(r.ec) << r.ec.message();
    EXPECT_TRUE(r.segmented);
    EXPECT_GT(r.connections_used, 4);
    EXPECT_EQ(r.size, size);
    EXPECT_EQ(r.path, dir_ / "seg.bin");  // name from Content-Disposition
    EXPECT_EQ(r.sha256, server_sha("seg.bin", size));
    EXPECT_FALSE(stdfs::exists(dir_ / "seg.bin.tdmpart"));
    EXPECT_EQ(r.bytes_received, size);  // no byte downloaded twice
}

TEST_F(TransferJobTest, ChunkedFallsBackToSingleStream) {
    const std::uint64_t size = 3'000'001;
    auto r = run(opts("/f/chk.bin?chunked=1&size=" + std::to_string(size)));
    ASSERT_FALSE(r.ec) << r.ec.message();
    EXPECT_FALSE(r.segmented);
    EXPECT_EQ(r.connections_used, 1);
    EXPECT_EQ(r.sha256, server_sha("chk.bin", size));
}

TEST_F(TransferJobTest, NoRangeServerUsesSingleStream) {
    const std::uint64_t size = 5'000'000;
    auto r = run(opts("/f/nor.bin?norange=1&size=" + std::to_string(size)));
    ASSERT_FALSE(r.ec) << r.ec.message();
    EXPECT_FALSE(r.segmented);
    EXPECT_EQ(r.sha256, server_sha("nor.bin", size));
}

TEST_F(TransferJobTest, SmallAndEmptyFiles) {
    auto a = run(opts("/f/small.bin?size=1000"));
    ASSERT_FALSE(a.ec) << a.ec.message();
    EXPECT_EQ(a.connections_used, 1);
    EXPECT_EQ(a.sha256, server_sha("small.bin", 1000));

    auto b = run(opts("/f/empty.bin?size=0"));
    ASSERT_FALSE(b.ec) << b.ec.message();
    EXPECT_EQ(b.size, 0u);
    EXPECT_EQ(b.sha256, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_F(TransferJobTest, NameCollisionGetsSuffix) {
    { std::ofstream(dir_ / "dup.bin") << "existing"; }
    auto r = run(opts("/f/dup.bin?size=2000"));
    ASSERT_FALSE(r.ec);
    EXPECT_EQ(r.path, dir_ / "dup (1).bin");
}

TEST_F(TransferJobTest, VersionChangeNeverMixesBytes) {
    const std::uint64_t size = 16u << 20;
    auto o = opts("/f/ver.bin?rate=3000000&size=" + std::to_string(size));
    o.max_connections = 3;
    const auto old_sha = server_sha("ver.bin", size);
    bool bumped = false;
    auto r = run(o, [&](TransferJob&, const TransferProgress& p) {
        if (!bumped && p.downloaded > (3u << 20)) {
            bumped = true;
            get("/admin/bump/ver.bin");
        }
    });
    ASSERT_TRUE(bumped);
    ASSERT_FALSE(r.ec) << r.ec.message();
    const auto new_sha = server_sha("ver.bin", size);
    ASSERT_NE(old_sha, new_sha);
    // The file is entirely one version or the other, never a mix.
    EXPECT_TRUE(r.sha256 == new_sha || r.sha256 == old_sha) << r.sha256;
    if (r.sha256 == new_sha) EXPECT_GE(r.restarts, 1);
    std::printf("[   info   ] outcome=%s restarts=%d\n", r.sha256 == new_sha ? "new-version" : "old-version",
                r.restarts);
}

TEST_F(TransferJobTest, StopThenResumeContinuesWithoutRedownloading) {
    const std::uint64_t size = 32u << 20;
    const auto path = "/f/res.bin?rate=2000000&size=" + std::to_string(size);
    auto first = run(opts(path), [](TransferJob& j, const TransferProgress& p) {
        if (p.downloaded > (6u << 20)) j.stop();
    });
    ASSERT_EQ(first.ec, std::make_error_code(std::errc::operation_canceled));
    ASSERT_TRUE(first.resume);
    EXPECT_TRUE(stdfs::exists(first.resume->part_path));
    EXPECT_FALSE(first.resume->etag.empty());

    auto o = opts(path);
    o.resume = first.resume;
    auto second = run(o);
    ASSERT_FALSE(second.ec) << second.ec.message();
    EXPECT_EQ(second.restarts, 0);
    EXPECT_EQ(first.bytes_received + second.bytes_received, size);  // nothing fetched twice
    EXPECT_EQ(second.sha256, server_sha("res.bin", size));
}

TEST_F(TransferJobTest, ResumeAfterServerChangeRestartsCleanly) {
    const std::uint64_t size = 16u << 20;
    const auto path = "/f/chg.bin?rate=2000000&size=" + std::to_string(size);
    auto first = run(opts(path), [](TransferJob& j, const TransferProgress& p) {
        if (p.downloaded > (4u << 20)) j.stop();
    });
    ASSERT_TRUE(first.resume);
    get("/admin/bump/chg.bin");
    auto o = opts(path);
    o.resume = first.resume;
    auto second = run(o);
    ASSERT_FALSE(second.ec) << second.ec.message();
    EXPECT_EQ(second.restarts, 1);
    EXPECT_EQ(second.sha256, server_sha("chg.bin", size));  // the new version, complete
}

TEST_F(TransferJobTest, ResumeWithMissingPartFileStartsOver) {
    const std::uint64_t size = 8u << 20;
    const auto path = "/f/gone.bin?rate=2000000&size=" + std::to_string(size);
    auto first = run(opts(path), [](TransferJob& j, const TransferProgress& p) {
        if (p.downloaded > (2u << 20)) j.stop();
    });
    ASSERT_TRUE(first.resume);
    stdfs::remove(first.resume->part_path);
    auto o = opts(path);
    o.resume = first.resume;
    auto second = run(o);
    ASSERT_FALSE(second.ec) << second.ec.message();
    EXPECT_EQ(second.bytes_received, size);
    EXPECT_EQ(second.sha256, server_sha("gone.bin", size));
}

TEST_F(TransferJobTest, HttpErrorsAreReported) {
    auto nf = run(opts("/nope"));
    EXPECT_EQ(nf.ec, make_error_code(TransferError::http_status));
    EXPECT_EQ(nf.http_status, 404);

    auto busy = run(opts("/f/b503.bin?size=10&busy=1&retry_after=3"));
    EXPECT_EQ(busy.ec, make_error_code(TransferError::http_status));
    EXPECT_EQ(busy.http_status, 503);
    ASSERT_TRUE(busy.retry_after);
    EXPECT_EQ(*busy.retry_after, 3s);
    EXPECT_TRUE(stdfs::is_empty(dir_));  // nothing left behind
}

TEST_F(TransferJobTest, ExpectedHashIsEnforced) {
    auto o = opts("/f/h.bin?size=5000");
    o.expected_sha256 = std::string(64, '0');
    auto r = run(o);
    EXPECT_EQ(r.ec, make_error_code(TransferError::hash_mismatch));

    auto ok = opts("/f/h2.bin?size=5000");
    ok.expected_sha256 = server_sha("h2.bin", 5000);
    EXPECT_FALSE(run(ok).ec);
}

TEST_F(TransferJobTest, RedirectedDownloadUsesFinalUrlForSegments) {
    const std::uint64_t size = 6u << 20;
    auto r = run(opts("/redirect/2/f/redir.bin?rate=4000000&size=" + std::to_string(size)));
    ASSERT_FALSE(r.ec) << r.ec.message();
    EXPECT_TRUE(r.segmented);
    EXPECT_EQ(r.final_url.target, "/f/redir.bin?rate=4000000&size=" + std::to_string(size));
    EXPECT_EQ(r.sha256, server_sha("redir.bin", size));
}

// ---------------------------------------------------------------------------
// Concurrent file writing: 1 GiB, 8 uneven connections.
//
// LedgerWriter wraps the real writer and records every write range at the
// moment the job hands it over. Any overlap with an earlier write (in this
// run or a previous run of the same download) is a violation.
// ---------------------------------------------------------------------------
namespace {

class Ledger {
public:
    void add(std::uint64_t a, std::uint64_t b) {
        std::lock_guard lk(m_);
        ++writes_;
        auto it = iv_.upper_bound(a);
        if (it != iv_.begin() && std::prev(it)->second > a) ++violations_;
        if (it != iv_.end() && it->first < b) ++violations_;
        std::uint64_t s = a, e = b;
        it = iv_.lower_bound(a);
        if (it != iv_.begin()) {
            auto p = std::prev(it);
            if (p->second == a) {
                s = p->first;
                it = iv_.erase(p);
            }
        }
        if (it != iv_.end() && it->first == b) {
            e = it->second;
            iv_.erase(it);
        }
        iv_[s] = e;
    }
    int violations() const {
        std::lock_guard lk(m_);
        return violations_;
    }
    std::uint64_t writes() const {
        std::lock_guard lk(m_);
        return writes_;
    }
    bool covers_exactly(std::uint64_t total) const {
        std::lock_guard lk(m_);
        return iv_.size() == 1 && iv_.begin()->first == 0 && iv_.begin()->second == total;
    }

private:
    mutable std::mutex m_;
    std::map<std::uint64_t, std::uint64_t> iv_;  // merged [start, end)
    int violations_ = 0;
    std::uint64_t writes_ = 0;
};

class LedgerWriter final : public fs::FileWriter {
public:
    LedgerWriter(std::unique_ptr<fs::FileWriter> inner, std::shared_ptr<Ledger> l)
        : inner_(std::move(inner)), ledger_(std::move(l)) {}
    std::error_code open(const stdfs::path& p, std::optional<std::uint64_t> s, bool k) override {
        return inner_->open(p, s, k);
    }
    void write(std::uint64_t off, std::span<const std::byte> d) override {
        ledger_->add(off, off + d.size());
        inner_->write(off, d);
    }
    bool request_room(std::function<void()> f) override { return inner_->request_room(std::move(f)); }
    std::uint64_t queued_bytes() const override { return inner_->queued_bytes(); }
    std::error_code error() const override { return inner_->error(); }
    std::error_code flush() override { return inner_->flush(); }
    std::error_code close() override { return inner_->close(); }
    std::error_code finalize(const stdfs::path& f, stdfs::path& a) override { return inner_->finalize(f, a); }
    void discard() override { inner_->discard(); }
    const stdfs::path& part_path() const override { return inner_->part_path(); }

private:
    std::unique_ptr<fs::FileWriter> inner_;
    std::shared_ptr<Ledger> ledger_;
};

// Checks every progress snapshot: contiguous disjoint coverage, positions in
// bounds and never moving backwards, and when each segment completes.
struct SegmentWatch {
    std::map<std::uint64_t, std::uint64_t> last_pos;                          // begin -> pos
    std::map<std::uint64_t, std::chrono::steady_clock::time_point> done_at;  // begin -> first time done
    std::size_t max_segments = 0;
    int bad_snapshots = 0;
    void check(const TransferProgress& p, std::uint64_t total) {
        if (p.segments.empty()) return;
        max_segments = std::max(max_segments, p.segments.size());
        std::uint64_t expect = 0;
        for (const auto& s : p.segments) {
            if (s.begin != expect || s.pos < s.begin || s.pos > s.end) ++bad_snapshots;
            expect = s.end;
            auto& lp = last_pos[s.begin];
            if (s.pos < lp) ++bad_snapshots;  // progress must never go backwards
            lp = s.pos;
            if (s.done() && !done_at.count(s.begin)) done_at[s.begin] = std::chrono::steady_clock::now();
        }
        if (expect != total) ++bad_snapshots;
    }
    std::size_t distinct_finish_moments() const {
        std::set<long long> slots;  // 20 ms polling slots
        for (auto& [b, t] : done_at)
            slots.insert(std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count() / 20);
        return slots.size();
    }
};

std::uint64_t downloaded_of(const std::vector<Segment>& segs) {
    std::uint64_t n = 0;
    for (const auto& s : segs) n += s.pos - s.begin;
    return n;
}

std::string sha_of_file_range(const stdfs::path& p, std::uint64_t a, std::uint64_t b) {
    std::ifstream f(p, std::ios::binary);
    f.seekg(static_cast<std::streamoff>(a));
    test::Sha256 h;
    std::vector<char> buf(1 << 20);
    while (a < b) {
        const auto n = static_cast<std::streamsize>(std::min<std::uint64_t>(buf.size(), b - a));
        f.read(buf.data(), n);
        if (f.gcount() != n) return "short-read";
        h.update(std::as_bytes(std::span<const char>(buf.data(), static_cast<std::size_t>(n))));
        a += static_cast<std::uint64_t>(n);
    }
    return h.final_hex();
}

}  // namespace

TEST_F(TransferJobTest, Large1GiBEightUnevenSegmentsWriteExactlyOnce) {
    const std::uint64_t size = 1ull << 30;
    auto ledger = std::make_shared<Ledger>();
    auto o = opts("/f/giga.bin?rate=60000000&rate_jitter=1&size=" + std::to_string(size));
    o.max_connections = 8;
    o.min_split = 64u << 20;
    o.writer_factory = [ledger](std::size_t budget) {
        return std::make_unique<LedgerWriter>(fs::make_file_writer(budget), ledger);
    };
    SegmentWatch watch;
    const auto t0 = std::chrono::steady_clock::now();
    auto r = run(o, [&](TransferJob&, const TransferProgress& p) { watch.check(p, size); });
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    ASSERT_FALSE(r.ec) << r.ec.message();

    EXPECT_EQ(ledger->violations(), 0) << "overlapping writes";
    EXPECT_TRUE(ledger->covers_exactly(size)) << "holes in the file";
    EXPECT_EQ(r.bytes_received, size);
    EXPECT_EQ(watch.bad_snapshots, 0);
    EXPECT_GE(watch.max_segments, 8u);
    EXPECT_GE(watch.distinct_finish_moments(), 4u) << "segments should finish at different times";
    EXPECT_EQ(r.size, size);
    EXPECT_EQ(r.sha256, server_sha("giga.bin", size));
    std::printf("[   info   ] 1 GiB: %.1f s incl. hashing, %zu segments max, %zu finish moments, %llu writes\n", secs,
                watch.max_segments, watch.distinct_finish_moments(),
                static_cast<unsigned long long>(ledger->writes()));
    std::error_code ec;
    stdfs::remove_all(dir_, ec);
}

TEST_F(TransferJobTest, Large1GiBStopResumePreservesValidProgress) {
    const std::uint64_t size = 1ull << 30;
    const auto path = "/f/giga2.bin?rate=50000000&rate_jitter=1&size=" + std::to_string(size);
    auto ledger = std::make_shared<Ledger>();  // shared by both runs
    auto factory = [ledger](std::size_t budget) -> std::unique_ptr<fs::FileWriter> {
        return std::make_unique<LedgerWriter>(fs::make_file_writer(budget), ledger);
    };
    auto o = opts(path);
    o.min_split = 64u << 20;
    o.writer_factory = factory;
    auto first = run(o, [&](TransferJob& j, const TransferProgress& p) {
        if (p.downloaded > size * 2 / 5) j.stop();
    });
    ASSERT_EQ(first.ec, std::make_error_code(std::errc::operation_canceled));
    ASSERT_TRUE(first.resume);

    // Every byte the resume state claims must already be on disk and correct.
    std::uint64_t claimed = 0;
    int checked = 0, partial = 0;
    for (const auto& s : first.resume->segments) {
        if (s.pos == s.begin) continue;
        claimed += s.pos - s.begin;
        if (!s.done()) ++partial;
        const auto expect = get("/admin/sha256/giga2.bin?size=" + std::to_string(size) + "&start=" +
                                std::to_string(s.begin) + "&end=" + std::to_string(s.pos));
        EXPECT_EQ(sha_of_file_range(first.resume->part_path, s.begin, s.pos), expect)
            << "segment [" << s.begin << ", " << s.pos << ")";
        ++checked;
    }
    EXPECT_EQ(claimed, first.bytes_received);
    EXPECT_GE(checked, 4);
    EXPECT_GE(partial, 1) << "expected some half-finished segments at stop time";

    auto o2 = opts(path);
    o2.min_split = 64u << 20;
    o2.writer_factory = factory;
    o2.resume = first.resume;
    SegmentWatch watch;
    auto second = run(o2, [&](TransferJob&, const TransferProgress& p) { watch.check(p, size); });
    ASSERT_FALSE(second.ec) << second.ec.message();
    EXPECT_EQ(second.restarts, 0);
    EXPECT_EQ(ledger->violations(), 0) << "resume rewrote bytes that were already valid";
    EXPECT_TRUE(ledger->covers_exactly(size));
    EXPECT_EQ(first.bytes_received + second.bytes_received, size);
    EXPECT_EQ(watch.bad_snapshots, 0);
    EXPECT_EQ(second.sha256, server_sha("giga2.bin", size));
    std::printf("[   info   ] stop at %.0f%%: %d segments verified on disk (%d partial), resume fetched %llu MiB\n",
                100.0 * static_cast<double>(first.bytes_received) / static_cast<double>(size), checked, partial,
                static_cast<unsigned long long>(second.bytes_received >> 20));
    std::error_code ec;
    stdfs::remove_all(dir_, ec);
}

// A live checkpoint is a valid crash-recovery point: every byte it claims is
// already durable and correct while the transfer keeps running, and resuming
// from it (ignoring everything written afterwards, as after a crash) produces
// the exact file.
TEST_F(TransferJobTest, LiveCheckpointIsAValidCrashRecoveryPoint) {
    const std::uint64_t size = 64u << 20;
    const auto path = "/f/ckpt.bin?rate=4000000&rate_jitter=1&size=" + std::to_string(size);
    std::optional<ResumeState> cp;
    int taken = 0;
    // Verification requests use helper_client_ (own pool); sharing the job's
    // pool once starved them until the download had finished (see PROJECT_STATUS).
    auto first = run(opts(path), [&](TransferJob& j, const TransferProgress& p) {
        if (!cp && p.downloaded > (12u << 20)) {
            if (auto rs = j.checkpoint()) {
                ++taken;
                cp = rs;
                // Verify on disk now, while the connections are still writing.
                for (const auto& s : rs->segments)
                    if (s.pos > s.begin)
                        EXPECT_EQ(sha_of_file_range(rs->part_path, s.begin, s.pos),
                                  get("/admin/sha256/ckpt.bin?size=" + std::to_string(size) + "&start=" +
                                      std::to_string(s.begin) + "&end=" + std::to_string(s.pos)));
            }
        }
        if (cp && p.downloaded > (24u << 20)) j.stop();  // "crash" later than the checkpoint
    });
    ASSERT_EQ(taken, 1);
    ASSERT_TRUE(first.resume) << "the download finished before stop(); ec=" << first.ec.message();
    ASSERT_LT(downloaded_of(cp->segments), downloaded_of(first.resume->segments));

    auto o = opts(path);
    o.resume = cp;  // the older state, as if the newer one had never been saved
    auto second = run(o);
    ASSERT_FALSE(second.ec) << second.ec.message();
    EXPECT_EQ(second.restarts, 0);
    EXPECT_EQ(downloaded_of(cp->segments) + second.bytes_received, size);
    EXPECT_EQ(second.sha256, server_sha("ckpt.bin", size));
}

// Bandwidth limits measured against the wall clock (unthrottled server).
TEST_F(TransferJobTest, PerDownloadLimitHoldsWithinTenPercent) {
    const std::uint64_t size = 12'000'000, limit = 3'000'000;
    auto o = opts("/f/lim.bin?size=" + std::to_string(size));
    o.limiters = {std::make_shared<scheduler::TokenBucket>(limit)};
    const auto t0 = std::chrono::steady_clock::now();
    auto r = run(o);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    ASSERT_FALSE(r.ec) << r.ec.message();
    EXPECT_EQ(r.sha256, server_sha("lim.bin", size));
    const double rate = static_cast<double>(size) / secs;
    std::printf("[   info   ] limit %llu B/s -> measured %.0f B/s over %.2f s (%d connections)\n",
                static_cast<unsigned long long>(limit), rate, secs, r.connections_used);
    EXPECT_GT(rate, 0.90 * static_cast<double>(limit));
    EXPECT_LT(rate, 1.10 * static_cast<double>(limit));
}

TEST_F(TransferJobTest, GlobalLimitIsSharedByConcurrentDownloads) {
    const std::uint64_t size = 6'000'000, limit = 3'000'000;
    auto global = std::make_shared<scheduler::TokenBucket>(limit);
    std::promise<TransferResult> pa, pb;
    auto oa = opts("/f/ga.bin?size=" + std::to_string(size));
    auto ob = opts("/f/gb.bin?size=" + std::to_string(size));
    oa.limiters = {global};
    ob.limiters = {global};
    const auto t0 = std::chrono::steady_clock::now();
    auto ja = TransferJob::create(io_, *client_, oa, [&](const TransferResult& r) { pa.set_value(r); });
    auto jb = TransferJob::create(io_, *client_, ob, [&](const TransferResult& r) { pb.set_value(r); });
    ja->start();
    jb->start();
    auto ra = pa.get_future().get();
    auto rb = pb.get_future().get();
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    ASSERT_FALSE(ra.ec);
    ASSERT_FALSE(rb.ec);
    const double rate = static_cast<double>(2 * size) / secs;
    std::printf("[   info   ] shared limit %llu B/s -> aggregate %.0f B/s over %.2f s\n",
                static_cast<unsigned long long>(limit), rate, secs);
    EXPECT_GT(rate, 0.90 * static_cast<double>(limit));
    EXPECT_LT(rate, 1.10 * static_cast<double>(limit));
    EXPECT_EQ(ra.sha256, server_sha("ga.bin", size));
    EXPECT_EQ(rb.sha256, server_sha("gb.bin", size));
}
