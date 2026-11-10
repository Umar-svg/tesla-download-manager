// Integration tests: HttpClient + ConnectionManager against the Python probe
// server (tools/testserver/probe_server.py) started on a free local port.

#include "net/http_client.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>
#include <openssl/evp.h>

#include <atomic>
#include <future>
#include <thread>
#include <windows.h>

using namespace tdm;
using namespace tdm::net;
using tdm::test::ProbeServer;
using tdm::test::Sha256;
using namespace std::chrono_literals;

namespace {

struct Result {
    std::error_code ec;
    ResponseHead head;
    bool got_head = false;
    std::uint64_t consumed = 0;
    std::string body;  // kept only when keep_body
    std::string sha;
};

// Runs one fetch to completion. stop_after limits how many body bytes the sink accepts.
Result run(HttpClient& client, Request req, bool keep_body = false,
           std::uint64_t stop_after = UINT64_MAX, std::function<void(FetchHandle&)> on_first_bytes = {}) {
    auto res = std::make_shared<Result>();
    auto sha = std::make_shared<Sha256>();
    auto taken = std::make_shared<std::uint64_t>(0);
    std::promise<void> done;
    std::shared_ptr<FetchHandle> handle;
    std::atomic<bool> fired{false};
    std::mutex hm;
    std::unique_lock hlk(hm);  // the sink may run before fetch() returns and `handle` is set
    handle = client.fetch(
        std::move(req),
        [res](const ResponseHead& h) {
            res->head = h;
            res->got_head = true;
            return true;
        },
        [&, res, sha, taken](std::span<const std::byte> s) -> std::size_t {
            const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(s.size(), stop_after - *taken));
            sha->update(s.first(n));
            if (keep_body) res->body.append(reinterpret_cast<const char*>(s.data()), n);
            *taken += n;
            if (on_first_bytes && !fired.exchange(true)) {
                std::lock_guard lk(hm);
                on_first_bytes(*handle);
            }
            return n;
        },
        [&, res](std::error_code ec, std::uint64_t consumed) {
            res->ec = ec;
            res->consumed = consumed;
            done.set_value();
        });
    hlk.unlock();
    done.get_future().wait();
    res->sha = sha->final_hex();
    return *res;
}

const std::string* header(const ResponseHead& h, std::string_view name) { return h.find(name); }

}  // namespace

class HttpClientTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        server_ = new ProbeServer(false);
        tls_server_ = new ProbeServer(true);
    }
    static void TearDownTestSuite() {
        delete server_;
        delete tls_server_;
    }

    void SetUp() override {
        ASSERT_TRUE(server_->ok()) << "probe server did not start (python: " TDM_PYTHON ")";
        make_pool({});
    }
    void TearDown() override { stop(); }

    void make_pool(PoolOptions opts) {
        stop();
        io_ = std::make_unique<asio::io_context>();
        guard_.emplace(asio::make_work_guard(*io_));
        for (int i = 0; i < 4; ++i) threads_.emplace_back([this] { io_->run(); });
        pool_ = ConnectionManager::create(*io_, std::move(opts));
        client_ = std::make_unique<HttpClient>(pool_);
    }
    void stop() {
        if (!io_) return;
        client_.reset();
        if (pool_) pool_->close_idle();
        guard_.reset();
        io_->stop();
        for (auto& t : threads_) t.join();
        threads_.clear();
        pool_.reset();
        io_.reset();
    }

    Request req(const std::string& path) {
        Request r;
        r.url = server_->url(path);
        return r;
    }
    std::string server_sha(const std::string& name, std::uint64_t size) {
        return run(*client_, req("/admin/sha256/" + name + "?size=" + std::to_string(size)), true).body;
    }

    static ProbeServer* server_;
    static ProbeServer* tls_server_;
    std::unique_ptr<asio::io_context> io_;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> guard_;
    std::vector<std::thread> threads_;
    std::shared_ptr<ConnectionManager> pool_;
    std::unique_ptr<HttpClient> client_;
};

ProbeServer* HttpClientTest::server_ = nullptr;
ProbeServer* HttpClientTest::tls_server_ = nullptr;

TEST_F(HttpClientTest, FullBodyMatchesServerHash) {
    const std::uint64_t size = 3 * 1024 * 1024 + 1;
    auto r = run(*client_, req("/f/full.bin?size=" + std::to_string(size)));
    ASSERT_FALSE(r.ec) << r.ec.message();
    EXPECT_EQ(r.head.status, 200);
    EXPECT_EQ(*r.head.content_length, size);
    EXPECT_EQ(r.consumed, size);
    ASSERT_NE(header(r.head, "ETag"), nullptr);
    EXPECT_EQ(*header(r.head, "Accept-Ranges"), "bytes");
    EXPECT_EQ(r.sha, server_sha("full.bin", size));
}

TEST_F(HttpClientTest, StopThenResumeWithRangeReassemblesFile) {
    const std::uint64_t size = 5'000'000, half = 2'345'678;
    const auto path = "/f/split.bin?size=" + std::to_string(size);
    auto a = run(*client_, req(path), true, half);
    ASSERT_FALSE(a.ec) << a.ec.message();
    EXPECT_EQ(a.consumed, half);

    auto rq = req(path);
    rq.range_from = half;
    rq.if_range = *header(a.head, "ETag");
    auto b = run(*client_, rq, true);
    ASSERT_FALSE(b.ec) << b.ec.message();
    EXPECT_EQ(b.head.status, 206);
    EXPECT_EQ(*header(b.head, "Content-Range"), "bytes 2345678-4999999/5000000");
    EXPECT_EQ(b.consumed, size - half);

    Sha256 all;
    all.update(std::as_bytes(std::span<const char>(a.body)));
    all.update(std::as_bytes(std::span<const char>(b.body)));
    EXPECT_EQ(all.final_hex(), server_sha("split.bin", size));
}

TEST_F(HttpClientTest, IfRangeMismatchFallsBackTo200) {
    auto rq = req("/f/ifr.bin?size=100000");
    rq.range_from = 500;
    rq.if_range = "\"not-the-etag\"";
    auto r = run(*client_, rq);
    ASSERT_FALSE(r.ec);
    EXPECT_EQ(r.head.status, 200);
    EXPECT_EQ(r.consumed, 100000u);
}

TEST_F(HttpClientTest, RangeBeyondEndIs416) {
    auto rq = req("/f/small.bin?size=1000");
    rq.range_from = 1000;
    auto r = run(*client_, rq);
    ASSERT_FALSE(r.ec);
    EXPECT_EQ(r.head.status, 416);
    EXPECT_EQ(*header(r.head, "Content-Range"), "bytes */1000");
}

TEST_F(HttpClientTest, FollowsRedirectChain) {
    auto r = run(*client_, req("/redirect/3/f/r.bin?size=4096"));
    ASSERT_FALSE(r.ec) << r.ec.message();
    EXPECT_EQ(r.head.status, 200);
    EXPECT_EQ(r.head.final_url.target, "/f/r.bin?size=4096");
    EXPECT_EQ(r.consumed, 4096u);
}

TEST_F(HttpClientTest, TooManyRedirects) {
    auto rq = req("/redirect/5/f/r.bin?size=10");
    rq.max_redirects = 4;
    auto r = run(*client_, rq);
    EXPECT_EQ(r.ec, make_error_code(Error::too_many_redirects));
}

TEST_F(HttpClientTest, ExposesRetryAfterOn503) {
    auto r = run(*client_, req("/f/busy.bin?size=10&busy=1&retry_after=7"));
    ASSERT_FALSE(r.ec);
    EXPECT_EQ(r.head.status, 503);
    ASSERT_NE(header(r.head, "Retry-After"), nullptr);
    EXPECT_EQ(*header(r.head, "Retry-After"), "7");
    auto again = run(*client_, req("/f/busy.bin?size=10&busy=1&retry_after=7"));
    EXPECT_EQ(again.head.status, 200);
}

TEST_F(HttpClientTest, ChunkedBodyMatchesHash) {
    const std::uint64_t size = 1'234'567;
    auto r = run(*client_, req("/f/chunk.bin?chunked=1&size=" + std::to_string(size)));
    ASSERT_FALSE(r.ec) << r.ec.message();
    EXPECT_TRUE(r.head.chunked);
    EXPECT_FALSE(r.head.content_length);
    EXPECT_EQ(r.consumed, size);
    EXPECT_EQ(r.sha, server_sha("chunk.bin", size));
}

TEST_F(HttpClientTest, NoRangeServerReturnsFullBody) {
    auto rq = req("/f/nr.bin?norange=1&size=50000");
    rq.range_from = 100;
    auto r = run(*client_, rq);
    ASSERT_FALSE(r.ec);
    EXPECT_EQ(r.head.status, 200);
    EXPECT_EQ(header(r.head, "Accept-Ranges"), nullptr);
    EXPECT_EQ(r.consumed, 50000u);
}

TEST_F(HttpClientTest, KeepAliveConnectionIsReused) {
    for (int i = 0; i < 3; ++i) ASSERT_FALSE(run(*client_, req("/f/ka.bin?size=20000")).ec);
    const auto st = pool_->stats();
    EXPECT_EQ(st.connects, 1u);
    EXPECT_EQ(st.reuses, 2u);
    EXPECT_EQ(pool_->idle_count(server_->url("/")), 1u);
}

TEST_F(HttpClientTest, StoppedTransferIsNotReused) {
    auto r = run(*client_, req("/f/big.bin?size=20000000"), false, 100'000);
    ASSERT_FALSE(r.ec);
    EXPECT_EQ(r.consumed, 100'000u);
    EXPECT_EQ(pool_->idle_count(server_->url("/")), 0u);
    EXPECT_EQ(pool_->open_count(server_->url("/")), 0u);
}

TEST_F(HttpClientTest, PerHostCapIsEnforced) {
    PoolOptions o;
    o.max_per_host = 2;
    make_pool(o);
    std::vector<std::future<Result>> fs;
    for (int i = 0; i < 6; ++i)
        fs.push_back(std::async(std::launch::async, [this] {
            return run(*client_, req("/f/cap.bin?size=300000&rate=3000000"));
        }));
    for (auto& f : fs) {
        auto r = f.get();
        EXPECT_FALSE(r.ec) << r.ec.message();
        EXPECT_EQ(r.consumed, 300000u);
    }
    EXPECT_LE(pool_->stats().connects, 2u);
    EXPECT_EQ(pool_->stats().connects + pool_->stats().reuses, 6u);
}

TEST_F(HttpClientTest, IdleTimeout) {
    auto rq = req("/f/slow.bin?size=1000000&rate=1000");  // 16 KiB, then ~16 s of silence
    rq.idle_timeout = 1s;
    const auto t0 = std::chrono::steady_clock::now();
    auto r = run(*client_, rq);
    EXPECT_EQ(r.ec, make_error_code(Error::timeout));
    EXPECT_LT(std::chrono::steady_clock::now() - t0, 8s);
}

TEST_F(HttpClientTest, CancelStopsTransfer) {
    auto r = run(*client_, req("/f/cancel.bin?size=50000000&rate=2000000"), false, UINT64_MAX,
                 [](FetchHandle& h) { h.cancel(); });
    EXPECT_EQ(r.ec, make_error_code(Error::cancelled));
    EXPECT_LT(r.consumed, 50'000'000u);
}

TEST_F(HttpClientTest, ConnectionRefused) {
    Request rq;
    rq.url = *core::parse_url("http://127.0.0.1:1/x");
    rq.connect_timeout = 5s;
    auto r = run(*client_, rq);
    EXPECT_TRUE(r.ec);
    EXPECT_FALSE(r.got_head);
    EXPECT_EQ(pool_->stats().failures, 1u);
    EXPECT_EQ(pool_->open_count(rq.url), 0u);
}

// 8 parallel open-ended Range requests, each stopped at its segment end by the
// sink (as the multipart driver will do), reassembled by offset.
TEST_F(HttpClientTest, ParallelSegmentsReassembleExactly) {
    const std::uint64_t size = 32ull << 20;
    const std::string path = "/f/par.bin?size=" + std::to_string(size);
    std::string file(size, '\0');
    std::vector<std::future<Result>> fs;
    const std::uint64_t seg = size / 8;
    for (int i = 0; i < 8; ++i) {
        fs.push_back(std::async(std::launch::async, [&, i] {
            auto rq = req(path);
            const std::uint64_t begin = seg * i;
            rq.range_from = begin;
            auto r = run(*client_, rq, true, seg);
            if (!r.ec) std::copy(r.body.begin(), r.body.end(), file.begin() + static_cast<std::ptrdiff_t>(begin));
            return r;
        }));
    }
    for (auto& f : fs) {
        auto r = f.get();
        ASSERT_FALSE(r.ec) << r.ec.message();
        EXPECT_EQ(r.head.status, 206);
        EXPECT_EQ(r.consumed, seg);
    }
    Sha256 h;
    h.update(std::as_bytes(std::span<const char>(file)));
    EXPECT_EQ(h.final_hex(), server_sha("par.bin", size));
}

TEST_F(HttpClientTest, TlsWithTrustedTestCa) {
    ASSERT_TRUE(tls_server_->ok());
    PoolOptions o;
    o.tls.use_system_store = false;
    o.tls.extra_ca_file = TDM_TEST_CERT;
    make_pool(o);
    Request rq;
    rq.url = tls_server_->url("/f/tls.bin?size=2000000");
    auto r = run(*client_, rq);
    ASSERT_FALSE(r.ec) << r.ec.message();
    EXPECT_EQ(r.head.status, 200);
    EXPECT_EQ(r.consumed, 2000000u);
    // Reuse over TLS too.
    ASSERT_FALSE(run(*client_, rq).ec);
    EXPECT_EQ(pool_->stats().connects, 1u);
}

TEST_F(HttpClientTest, TlsRejectsUntrustedCertificate) {
    ASSERT_TRUE(tls_server_->ok());
    Request rq;
    rq.url = tls_server_->url("/f/tls.bin?size=10");  // default pool: system roots only
    auto r = run(*client_, rq);
    EXPECT_TRUE(r.ec);
    EXPECT_FALSE(r.got_head);
}

// Regression (0.1.2): a product-only User-Agent made Brave's download link
// redirect to its Linux web page. The default UA must be browser-compatible
// and still carry the TeslaDM token, as seen by the server.
TEST_F(HttpClientTest, DefaultUserAgentIsBrowserCompatibleOnTheWire) {
    ASSERT_FALSE(run(*client_, req("/f/ua-check.bin?size=10")).ec);
    const auto log = run(*client_, req("/admin/log"), true).body;
    const auto at = log.rfind("ua-check.bin");
    ASSERT_NE(at, std::string::npos);
    const auto ua_at = log.find("\"User-Agent\": \"", at);
    ASSERT_NE(ua_at, std::string::npos);
    const auto ua = log.substr(ua_at + 15, log.find('"', ua_at + 15) - (ua_at + 15));
    EXPECT_TRUE(ua.starts_with("Mozilla/5.0 (Windows NT 10.0; Win64; x64)")) << ua;
    EXPECT_NE(ua.find("TeslaDM/"), std::string::npos) << ua;
}
