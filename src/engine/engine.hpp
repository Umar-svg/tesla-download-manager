#pragma once
// Download engine (architecture module 5): owns the network stack, the
// database and one TransferJob per running download.
//
// States: Queued → Downloading → Completed
//                 ↘ Waiting (retry at T) → Queued
//                 ↘ Paused / Failed / Cancelled
//
// Persistence: every row change is written immediately; the segment table of
// each running download is checkpointed every `checkpoint_interval` (durable:
// captured, then flushed, see TransferJob::checkpoint) and again on
// pause/stop. On construction, unfinished downloads are reloaded and resumed.
//
// Thread-safe. Observers are called on the engine's maintenance thread at
// `snapshot_interval`, plus once after every state change, never with
// internal locks held.

#include <asio.hpp>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <thread>
#include <vector>

#include "db/store.hpp"
#include "engine/download_engine.hpp"
#include "multipart/transfer_job.hpp"
#include "net/http_client.hpp"
#include "retry/retry_policy.hpp"
#include "scheduler/scheduler.hpp"
#include "scheduler/token_bucket.hpp"

namespace tdm::engine {

struct EngineOptions {
    std::filesystem::path db_path;
    int network_threads = 4;
    int max_active = 8;                       // global cap; each queue also has its own max_active
    std::uint64_t min_split = 1u << 20;
    // Hard ceiling per host. The per-download connection count (1..32, user
    // choice) is the real control, so the pool must not silently cap it lower
    // (it used to default to 8: a 16-connection download only ever got 8).
    net::PoolOptions pool = [] {
        net::PoolOptions p;
        p.max_per_host = 32;
        return p;
    }();
    retry::RetryPolicy retry;                 // whole-download retries
    std::chrono::milliseconds checkpoint_interval{1000};
    std::chrono::milliseconds snapshot_interval{250};
    bool resume_on_start = true;              // false: unfinished downloads come back Paused
    std::function<std::chrono::system_clock::time_point()> now;  // schedule clock; default system_clock
};

class Engine final : public DownloadEngine {
public:
    explicit Engine(EngineOptions opts);
    ~Engine() override;

    DownloadId add(const AddRequest& req) override;
    void start(DownloadId id) override;
    void pause(DownloadId id) override;
    void cancel(DownloadId id, bool delete_file) override;
    void remove(DownloadId id, bool delete_file) override;
    void set_max_active(int max_active) override;
    void set_global_speed_limit(std::optional<std::uint64_t> bps) override;
    void set_speed_limit(DownloadId id, std::optional<std::uint64_t> bps) override;
    void set_priority(DownloadId id, int priority) override;
    void move_to_queue(DownloadId id, std::int64_t queue_id) override;
    std::int64_t add_queue(const std::string& name, int max_active) override;
    void set_queue_limit(std::int64_t queue_id, int max_active) override;
    void set_queue_schedule(std::int64_t queue_id, std::optional<scheduler::ScheduleRule> rule) override;
    std::vector<QueueInfo> queues() const override;
    void subscribe(Observer obs) override;
    void shutdown() override;

    std::optional<Snapshot> snapshot(DownloadId id) const;
    std::vector<Snapshot> snapshots() const override;

private:
    struct Item {
        db::DownloadRow row;
        std::shared_ptr<multipart::TransferJob> job;
        std::optional<multipart::ResumeState> resume;
        int attempts = 0;
        std::shared_ptr<asio::steady_timer> retry_timer;
        std::shared_ptr<scheduler::TokenBucket> bucket = std::make_shared<scheduler::TokenBucket>();
        enum class Stop { None, Pause, Cancel, Shutdown, Schedule } stop = Stop::None;
        bool delete_file = false;
        bool remove_after = false;  // remove() while running: drop the row once the job has stopped
        // live figures, refreshed by the maintenance thread
        std::uint64_t downloaded = 0;
        int connections = 0;
        std::vector<multipart::Segment> segments;
        std::uint64_t last_bytes = 0;
        std::chrono::steady_clock::time_point last_time{};
        double bps = 0;
    };

    struct QueueEntry {
        db::QueueRow row;
        std::optional<scheduler::ScheduleRule> rule;
    };

    // require mu_
    void load_from_db();
    bool queue_open(std::int64_t queue_id, scheduler::LocalTime t) const;
    scheduler::LocalTime local_now() const;
    void pump();
    void start_job(DownloadId id, Item& it);
    void persist(Item& it);
    void save_resume(DownloadId id, Item& it, const multipart::ResumeState& rs);
    void clear_resume(DownloadId id, Item& it);
    void erase_item(DownloadId id, Item& it, bool delete_file, std::vector<Snapshot>& out);
    Snapshot make_snapshot(DownloadId id, const Item& it) const;

    void on_job_done(DownloadId id, std::shared_ptr<multipart::TransferJob> job, const multipart::TransferResult& r);
    void maintenance_loop();
    void notify(const std::vector<Snapshot>& snaps);  // no locks held

    EngineOptions opts_;
    asio::io_context io_;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> guard_;
    std::vector<std::thread> threads_;
    std::shared_ptr<net::ConnectionManager> pool_;
    std::unique_ptr<net::HttpClient> client_;

    std::mutex db_mu_;                 // order: mu_ → db_mu_
    std::unique_ptr<db::Store> store_;

    mutable std::mutex mu_;
    std::condition_variable cv_;       // job completions, maintenance wake-ups
    std::map<DownloadId, Item> items_;
    int running_ = 0;                  // jobs whose done callback has not run yet
    bool shut_ = false;
    std::shared_ptr<scheduler::TokenBucket> global_bucket_ = std::make_shared<scheduler::TokenBucket>();
    std::map<std::int64_t, QueueEntry> queues_;
    std::mt19937_64 rng_{std::random_device{}()};

    std::mutex obs_mu_;
    std::vector<Observer> observers_;

    std::thread maintenance_;
    bool stop_maintenance_ = false;    // under mu_
};

}  // namespace tdm::engine
