#pragma once
// Multipart transfer driver: downloads one URL into one file.
//
//   probe GET (no Range, D1) → validators, size, name → open .tdmpart
//   → segmented mode (size known + Accept-Ranges: bytes): the probe connection
//     becomes segment 0, up to N-1 more connections split the largest remaining
//     piece (D2), every Range request carries If-Range (D3) and every 206 is
//     checked (Content-Range start/total, ETag, Last-Modified)
//   → otherwise single-stream mode on the probe connection
//   → any sign that the file changed: cancel everything, discard, restart
//   → finalize: flush, rename, size check, SHA-256 (optional expected hash)
//
// Retry inside a job covers segment connections only (backoff, Retry-After,
// halving the connection count on 429/503). A failed probe, or a job that
// runs out of connections, ends with an error; the engine (Phase 5) decides
// whether to retry the whole job, resuming from `TransferResult::resume`.
//
// Thread-safe. Callbacks from HttpClient run on many connection strands, so all
// job state is guarded by one mutex. The done callback runs exactly once, on an
// internal thread, without the lock held.

#include <asio.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "fs/file_writer.hpp"
#include "multipart/segment_planner.hpp"
#include "net/http_client.hpp"
#include "retry/retry_policy.hpp"
#include "scheduler/token_bucket.hpp"

namespace tdm::multipart {

enum class TransferError {
    http_status = 1,      // server answered the probe with a non-200 status; see http_status
    too_many_restarts,    // the file kept changing during the download
    size_mismatch,        // bytes on disk differ from the announced size
    hash_mismatch,        // SHA-256 differs from the expected value
    insufficient_space,
    unexpected_response,  // e.g. 206 to a request without Range
};
const std::error_category& transfer_category() noexcept;
inline std::error_code make_error_code(TransferError e) noexcept { return {static_cast<int>(e), transfer_category()}; }

struct ResumeState {
    core::Url url;                 // final URL after redirects
    std::string etag;
    std::string last_modified;
    std::uint64_t total = 0;
    std::filesystem::path part_path;
    std::string file_name;
    std::vector<Segment> segments;  // durable progress (flushed before capture)
};

struct TransferOptions {
    core::Url url;
    std::filesystem::path directory;
    std::string file_name;               // empty → Content-Disposition, then URL
    int max_connections = 8;
    std::uint64_t min_split = 1u << 20;
    std::chrono::seconds connect_timeout{15};
    std::chrono::seconds idle_timeout{30};
    std::string expected_sha256;         // lower-case hex, optional
    std::optional<ResumeState> resume;
    retry::RetryPolicy retry;
    int max_restarts = 3;
    std::size_t write_budget = 64u << 20;
    std::vector<net::Header> extra_headers;
    // Creates the file writer (default: fs::make_file_writer). Tests inject a
    // recording decorator to prove that writes never overlap.
    std::function<std::unique_ptr<fs::FileWriter>(std::size_t budget)> writer_factory;
    // Bandwidth limits applied to every connection of this transfer (e.g. the
    // global bucket and a per-download bucket). Rates may change at any time.
    std::vector<std::shared_ptr<scheduler::TokenBucket>> limiters;
};

struct TransferProgress {
    std::string file_name;  // known after the probe
    std::optional<std::uint64_t> total;
    std::uint64_t downloaded = 0;
    int active_connections = 0;
    bool segmented = false;
    std::vector<Segment> segments;
};

struct TransferResult {
    std::error_code ec;
    int http_status = 0;
    std::optional<std::chrono::seconds> retry_after;
    std::filesystem::path path;           // final file (success)
    std::string file_name;
    std::uint64_t size = 0;
    std::string sha256;
    bool segmented = false;
    int restarts = 0;
    int connections_used = 0;
    std::uint64_t bytes_received = 0;     // body bytes written in this run
    bool disk_error = false;              // ec came from the file system (not worth retrying)
    core::Url final_url;
    std::optional<ResumeState> resume;    // set when stopped/failed with resumable progress
};

class TransferJob : public std::enable_shared_from_this<TransferJob> {
public:
    using DoneFn = std::function<void(const TransferResult&)>;

    static std::shared_ptr<TransferJob> create(asio::io_context& io, net::HttpClient& client, TransferOptions opts,
                                               DoneFn done);
    void start();
    void stop();  // keeps resumable progress; done gets std::errc::operation_canceled
    TransferProgress progress() const;

    // Durable live snapshot for crash recovery: captures the segment table, then
    // flushes every write enqueued before the capture (without stalling the
    // connections). nullopt when the transfer is not resumable right now
    // (probing, single stream, no validators, restarting, finished).
    std::optional<ResumeState> checkpoint();

private:
    TransferJob(asio::io_context& io, net::HttpClient& client, TransferOptions opts, DoneFn done);

    enum class Phase { Idle, Probing, Running, Restarting, Finishing, Done };
    struct Conn {
        std::shared_ptr<net::FetchHandle> handle;
        std::optional<SegmentId> seg;
        std::uint64_t offset = 0;  // expected Content-Range start
        bool probe = false;
        bool got_head = false;
        int http_status = 0;
        std::optional<std::chrono::seconds> retry_after;
        bool restart_requested = false;
        std::uint64_t reserved = 0;  // bandwidth tokens reserved for this connection's next read
    };

    // All private members below require mu_ unless noted.
    void start_probe();
    void resume_from(const ResumeState& rs);
    void launch(bool probe, std::optional<Assignment> a);
    bool on_head(unsigned gen, int id, const net::ResponseHead& h);
    bool probe_head(Conn& c, const net::ResponseHead& h);
    bool segment_head(Conn& c, const net::ResponseHead& h);
    std::size_t on_body(unsigned gen, int id, std::span<const std::byte> data);
    void on_done(unsigned gen, int id, std::error_code ec);
    void fill_connections();
    void request_restart();
    void do_restart();
    void fail(std::error_code ec, int http_status = 0, std::optional<std::chrono::seconds> retry_after = {});
    void cancel_all();
    void maybe_finish_stopped();
    void begin_finalize();
    void finalize_blocking();  // runs on its own thread, phase_ == Finishing
    std::string validator() const;
    std::size_t read_quantum() const;  // requires mu_; 0 = unlimited (no limiter has a rate)
    void charge(Conn& c, std::uint64_t n);
    void refund(Conn& c);
    std::unique_ptr<fs::FileWriter> make_writer() const;
    void deliver(TransferResult r);  // no lock

    asio::io_context& io_;
    net::HttpClient& client_;
    TransferOptions opts_;
    DoneFn done_;
    asio::strand<asio::io_context::executor_type> strand_;

    mutable std::mutex mu_;
    Phase phase_ = Phase::Idle;
    unsigned gen_ = 0;
    std::shared_ptr<fs::FileWriter> writer_;  // shared so checkpoint() can flush without mu_
    std::optional<SegmentPlanner> planner_;
    bool segmented_ = false;
    std::optional<std::uint64_t> total_;
    std::uint64_t single_pos_ = 0;
    std::string etag_, last_modified_;
    core::Url final_url_;
    std::string file_name_;
    std::filesystem::path part_path_;
    std::unordered_map<int, Conn> conns_;
    int next_id_ = 0;
    int max_conns_ = 8;
    int restarts_ = 0;
    int connections_used_ = 0;
    int seg_failures_ = 0;
    std::uint64_t bytes_received_ = 0;
    bool stopping_ = false;
    bool user_stop_ = false;
    std::error_code fail_ec_;
    bool disk_error_ = false;
    int fail_status_ = 0;
    std::optional<std::chrono::seconds> fail_retry_after_;
    std::vector<std::shared_ptr<asio::steady_timer>> timers_;
    std::mt19937_64 rng_{std::random_device{}()};
};

}  // namespace tdm::multipart

template <>
struct std::is_error_code_enum<tdm::multipart::TransferError> : std::true_type {};
