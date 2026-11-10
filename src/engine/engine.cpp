#include "engine/engine.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

#include "core/http_util.hpp"
#include "net/errors.hpp"
#include "queue/queue_selector.hpp"

namespace tdm::engine {
namespace {

using multipart::ResumeState;
using multipart::TransferError;
using multipart::TransferResult;

std::filesystem::path u8path(const std::string& s) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::string utf8(const std::filesystem::path& p) {
    const auto u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

std::uint64_t downloaded_of(const std::vector<multipart::Segment>& segs) {
    std::uint64_t n = 0;
    for (const auto& s : segs) n += s.pos - s.begin;
    return n;
}

// Which failures are worth retrying automatically. nullopt = needs the user.
std::optional<retry::Failure> classify(const TransferResult& r) {
    if (r.disk_error) return std::nullopt;  // disk full, access denied...
    if (r.ec.category() == multipart::transfer_category()) {
        if (r.ec == TransferError::http_status)
            return retry::Failure{retry::FailureKind::HttpStatus, r.http_status, r.retry_after};
        return std::nullopt;  // file keeps changing, hash/size mismatch, no space
    }
    if (r.ec.category() == net::error_category()) {
        switch (static_cast<net::Error>(r.ec.value())) {
            case net::Error::timeout: return retry::Failure{retry::FailureKind::Timeout, 0, {}};
            case net::Error::bad_response:
            case net::Error::connection_closed: return retry::Failure{retry::FailureKind::Protocol, 0, {}};
            default: return std::nullopt;  // redirect loop, oversized headers, cancelled
        }
    }
    if (r.ec.category() == asio::error::get_ssl_category()) return std::nullopt;  // certificate problems
    return retry::Failure{retry::FailureKind::Network, 0, {}};
}

std::string describe(const TransferResult& r) {
    std::string m = r.ec.message();
    if (r.http_status) m += " (HTTP " + std::to_string(r.http_status) + ")";
    return m;
}

bool is_active_state(db::State s) {
    return s == db::State::Queued || s == db::State::Probing || s == db::State::Downloading ||
           s == db::State::Waiting || s == db::State::Verifying;
}

}  // namespace

Engine::Engine(EngineOptions opts) : opts_(std::move(opts)) {
    store_ = std::make_unique<db::Store>(utf8(opts_.db_path));
    guard_.emplace(asio::make_work_guard(io_));
    for (int i = 0; i < std::max(1, opts_.network_threads); ++i) threads_.emplace_back([this] { io_.run(); });
    pool_ = net::ConnectionManager::create(io_, opts_.pool);
    client_ = std::make_unique<net::HttpClient>(pool_);
    {
        std::lock_guard lk(mu_);
        load_from_db();
        pump();
    }
    maintenance_ = std::thread([this] { maintenance_loop(); });
}

Engine::~Engine() { shutdown(); }

void Engine::load_from_db() {
    std::vector<db::DownloadRow> rows;
    {
        std::lock_guard dl(db_mu_);
        rows = store_->list_downloads();
        for (auto& q : store_->list_queues()) {
            QueueEntry e{q, std::nullopt};
            if (auto j = store_->get_setting("queue." + std::to_string(q.id) + ".schedule"))
                e.rule = scheduler::schedule_from_json(*j);
            queues_[q.id] = std::move(e);
        }
        if (auto g = store_->get_setting("global_speed_limit")) {
            try {
                global_bucket_->set_rate(std::stoull(*g));
            } catch (const std::exception&) {
                // malformed setting: stay unlimited
            }
        }
    }
    for (auto& row : rows) {
        Item it;
        it.row = row;
        it.bucket->set_rate(row.speed_limit);
        bool changed = false;
        if (is_active_state(row.state)) {  // interrupted by an exit or a crash
            it.row.state = opts_.resume_on_start ? db::State::Queued : db::State::Paused;
            changed = it.row.state != row.state;
        }
        if (row.state != db::State::Completed && row.state != db::State::Cancelled && !row.part_path.empty() &&
            row.total_size) {
            std::vector<db::SegmentRow> segs;
            {
                std::lock_guard dl(db_mu_);
                segs = store_->load_segments(row.id);
            }
            const auto url = core::parse_url(row.final_url.empty() ? row.url : row.final_url);
            if (!segs.empty() && url) {
                ResumeState rs;
                rs.url = *url;
                rs.etag = row.etag;
                rs.last_modified = row.last_modified;
                rs.total = *row.total_size;
                rs.part_path = u8path(row.part_path);
                rs.file_name = row.file_name;
                for (std::size_t i = 0; i < segs.size(); ++i)
                    rs.segments.push_back({static_cast<multipart::SegmentId>(i), segs[i].begin, segs[i].pos,
                                           segs[i].end, false});
                it.downloaded = downloaded_of(rs.segments);
                it.segments = rs.segments;
                it.resume = std::move(rs);
            }
        }
        if (row.state == db::State::Completed && row.total_size) it.downloaded = *row.total_size;
        if (changed) persist(it);
        items_.emplace(row.id, std::move(it));
    }
}

void Engine::persist(Item& it) {
    std::lock_guard dl(db_mu_);
    store_->update_download(it.row);
}

void Engine::save_resume(DownloadId id, Item& it, const ResumeState& rs) {
    it.resume = rs;
    it.row.final_url = rs.url.to_string();
    it.row.etag = rs.etag;
    it.row.last_modified = rs.last_modified;
    it.row.total_size = rs.total;
    it.row.part_path = utf8(rs.part_path);
    if (!rs.file_name.empty()) it.row.file_name = rs.file_name;
    it.downloaded = downloaded_of(rs.segments);
    it.segments = rs.segments;
    std::vector<db::SegmentRow> rows;
    rows.reserve(rs.segments.size());
    for (const auto& s : rs.segments) rows.push_back({s.begin, s.pos, s.end});
    std::lock_guard dl(db_mu_);
    store_->update_download(it.row);
    store_->save_segments(id, rows);
}

void Engine::clear_resume(DownloadId id, Item& it) {
    it.resume.reset();
    it.row.part_path.clear();
    it.segments.clear();
    std::lock_guard dl(db_mu_);
    store_->save_segments(id, {});
}

DownloadId Engine::add(const AddRequest& req) {
    const auto url = core::parse_url(req.url);
    if (!url) throw std::invalid_argument("unsupported URL: " + req.url);
    db::DownloadRow row;
    row.url = url->to_string();
    row.directory = utf8(req.directory);
    row.file_name = req.file_name;
    row.sha256_expected = req.sha256_expected;
    std::transform(row.sha256_expected.begin(), row.sha256_expected.end(), row.sha256_expected.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    row.connections = std::clamp(req.connections, 1, 32);
    row.queue_id = req.queue_id;
    row.speed_limit = req.speed_limit_bps.value_or(0);
    row.state = req.start_now ? db::State::Queued : db::State::Paused;

    std::vector<Snapshot> snaps;
    DownloadId id = 0;
    {
        std::lock_guard lk(mu_);
        if (shut_) throw std::logic_error("engine is shut down");
        if (!queues_.count(row.queue_id)) row.queue_id = 1;
        {
            std::lock_guard dl(db_mu_);
            id = store_->add_download(row);
            row = *store_->get_download(id);
        }
        items_[id].row = row;
        items_[id].bucket->set_rate(row.speed_limit);
        pump();
        snaps.push_back(make_snapshot(id, items_[id]));
    }
    notify(snaps);
    return id;
}

scheduler::LocalTime Engine::local_now() const {
    return scheduler::to_local(opts_.now ? opts_.now() : std::chrono::system_clock::now());
}

bool Engine::queue_open(std::int64_t queue_id, scheduler::LocalTime t) const {
    auto q = queues_.find(queue_id);
    return q == queues_.end() || !q->second.rule || scheduler::schedule_allows(*q->second.rule, t);
}

void Engine::pump() {
    if (shut_) return;
    const auto t = local_now();
    std::vector<queue::Candidate> queued;
    std::map<std::int64_t, int> running;
    int running_total = 0;
    for (auto& [id, it] : items_) {
        if (it.job) {
            ++running[it.row.queue_id];
            ++running_total;
        } else if (it.row.state == db::State::Queued) {
            queued.push_back({id, it.row.queue_id, it.row.priority});
        }
    }
    // Jobs that are still finishing a stop count against the global cap too.
    running_total = std::max(running_total, running_);
    std::map<std::int64_t, queue::QueueState> qs;
    for (const auto& [qid, q] : queues_) qs[qid] = {std::max(1, q.row.max_active), queue_open(qid, t)};
    for (auto id : queue::select_to_start(std::move(queued), qs, std::move(running), running_total, opts_.max_active))
        start_job(id, items_.at(id));
}

void Engine::start_job(DownloadId id, Item& it) {
    multipart::TransferOptions o;
    o.url = *core::parse_url(it.row.url);  // validated by add()
    o.directory = u8path(it.row.directory);
    o.file_name = it.row.file_name;
    o.max_connections = it.row.connections;
    o.min_split = opts_.min_split;
    o.expected_sha256 = it.row.sha256_expected;
    o.resume = it.resume;
    o.limiters = {global_bucket_, it.bucket};
    auto holder = std::make_shared<std::weak_ptr<multipart::TransferJob>>();
    auto job = multipart::TransferJob::create(io_, *client_, std::move(o),
                                              [this, id, holder](const TransferResult& r) {
                                                  on_job_done(id, holder->lock(), r);
                                              });
    *holder = job;
    it.job = job;
    it.stop = Item::Stop::None;
    it.row.state = db::State::Downloading;
    it.row.error.clear();
    it.last_bytes = it.downloaded;
    it.last_time = std::chrono::steady_clock::now();
    it.bps = 0;
    ++running_;
    persist(it);
    job->start();  // callbacks are always deferred, so holding mu_ here is safe
}

void Engine::on_job_done(DownloadId id, std::shared_ptr<multipart::TransferJob> job, const TransferResult& r) {
    std::vector<Snapshot> snaps;
    {
        std::lock_guard lk(mu_);
        --running_;
        auto f = items_.find(id);
        if (f == items_.end()) {
            cv_.notify_all();
            return;
        }
        Item& it = f->second;
        if (it.job == job) it.job.reset();
        const auto stop = it.stop;
        it.stop = Item::Stop::None;
        it.bps = 0;
        it.connections = 0;
        if (!r.file_name.empty()) it.row.file_name = r.file_name;

        auto remove_part = [&] {
            std::error_code ec;
            if (r.resume) std::filesystem::remove(r.resume->part_path, ec);
            if (!it.row.part_path.empty()) std::filesystem::remove(u8path(it.row.part_path), ec);
            clear_resume(id, it);
            it.downloaded = 0;
        };

        if (!r.ec) {
            it.row.state = db::State::Completed;
            it.row.file_path = utf8(r.path);
            it.row.total_size = r.size;
            it.row.sha256_actual = r.sha256;
            it.row.error.clear();
            it.downloaded = r.size;
            it.attempts = 0;
            clear_resume(id, it);
        } else if (stop == Item::Stop::Cancel) {
            remove_part();
            it.row.state = db::State::Cancelled;
        } else if (r.ec == std::errc::operation_canceled) {
            if (r.resume) {
                save_resume(id, it, *r.resume);
            } else {
                clear_resume(id, it);  // not resumable (single stream / no validators)
                it.downloaded = 0;
            }
            it.row.state = (stop == Item::Stop::Shutdown || stop == Item::Stop::Schedule) ? db::State::Queued
                                                                                         : db::State::Paused;
        } else {
            if (r.resume) {
                save_resume(id, it, *r.resume);
            } else {
                clear_resume(id, it);
                it.downloaded = 0;
            }
            if (!r.path.empty()) it.row.file_path = utf8(r.path);
            it.row.error = describe(r);
            const auto failure = classify(r);
            retry::Decision d;
            if (failure) d = opts_.retry.decide(*failure, ++it.attempts, rng_);
            if (shut_ || stop == Item::Stop::Shutdown || stop == Item::Stop::Schedule) {
                it.row.state = db::State::Queued;  // try again on the next start
            } else if (stop == Item::Stop::Pause) {
                it.row.state = db::State::Paused;
            } else if (failure && d.action == retry::Action::RetrySameState) {
                it.row.state = db::State::Waiting;
                auto timer = std::make_shared<asio::steady_timer>(io_, d.delay);
                it.retry_timer = timer;
                timer->async_wait([this, id, timer](std::error_code ec) {
                    if (ec) return;
                    std::vector<Snapshot> s;
                    {
                        std::lock_guard l2(mu_);
                        auto g = items_.find(id);
                        if (g == items_.end() || g->second.retry_timer != timer) return;
                        g->second.retry_timer.reset();
                        if (shut_ || g->second.row.state != db::State::Waiting) return;
                        g->second.row.state = db::State::Queued;
                        persist(g->second);
                        pump();
                        s.push_back(make_snapshot(id, g->second));
                    }
                    notify(s);
                });
            } else {
                it.row.state = db::State::Failed;
            }
        }
        if (it.remove_after) {
            erase_item(id, it, it.delete_file, snaps);  // `it` is gone after this
        } else {
            persist(it);
            snaps.push_back(make_snapshot(id, it));
        }
        pump();
        cv_.notify_all();
    }
    notify(snaps);
}

void Engine::erase_item(DownloadId id, Item& it, bool delete_file, std::vector<Snapshot>& out) {
    std::error_code ec;
    if (!it.row.part_path.empty()) std::filesystem::remove(u8path(it.row.part_path), ec);
    if (delete_file && it.row.state == db::State::Completed && !it.row.file_path.empty())
        std::filesystem::remove(u8path(it.row.file_path), ec);
    if (it.retry_timer) it.retry_timer->cancel();
    Snapshot s = make_snapshot(id, it);
    s.removed = true;
    out.push_back(std::move(s));
    {
        std::lock_guard dl(db_mu_);
        store_->remove_download(id);  // segments cascade
    }
    items_.erase(id);
}

void Engine::remove(DownloadId id, bool delete_file) {
    std::vector<Snapshot> snaps;
    {
        std::lock_guard lk(mu_);
        auto f = items_.find(id);
        if (f == items_.end()) return;
        Item& it = f->second;
        if (it.job) {
            it.stop = Item::Stop::Cancel;
            it.delete_file = delete_file;
            it.remove_after = true;
            it.job->stop();  // on_job_done erases the item
            return;
        }
        erase_item(id, it, delete_file, snaps);
    }
    notify(snaps);
}

void Engine::set_max_active(int max_active) {
    std::lock_guard lk(mu_);
    opts_.max_active = std::max(1, max_active);
    pump();
}

void Engine::start(DownloadId id) {
    std::vector<Snapshot> snaps;
    {
        std::lock_guard lk(mu_);
        auto f = items_.find(id);
        if (f == items_.end() || shut_) return;
        Item& it = f->second;
        if (it.job || it.row.state == db::State::Completed) return;
        if (it.retry_timer) {
            it.retry_timer->cancel();
            it.retry_timer.reset();
        }
        it.row.state = db::State::Queued;
        it.row.error.clear();
        it.attempts = 0;
        persist(it);
        pump();
        snaps.push_back(make_snapshot(id, it));
    }
    notify(snaps);
}

void Engine::pause(DownloadId id) {
    std::vector<Snapshot> snaps;
    {
        std::lock_guard lk(mu_);
        auto f = items_.find(id);
        if (f == items_.end()) return;
        Item& it = f->second;
        if (it.job) {
            it.stop = Item::Stop::Pause;
            it.job->stop();  // on_job_done saves the resume state and sets Paused
            return;
        }
        if (it.row.state == db::State::Queued || it.row.state == db::State::Waiting) {
            if (it.retry_timer) {
                it.retry_timer->cancel();
                it.retry_timer.reset();
            }
            it.row.state = db::State::Paused;
            persist(it);
            snaps.push_back(make_snapshot(id, it));
        }
    }
    notify(snaps);
}

void Engine::cancel(DownloadId id, bool delete_file) {
    std::vector<Snapshot> snaps;
    {
        std::lock_guard lk(mu_);
        auto f = items_.find(id);
        if (f == items_.end()) return;
        Item& it = f->second;
        if (it.job) {
            it.stop = Item::Stop::Cancel;
            it.delete_file = delete_file;
            it.job->stop();  // on_job_done removes the part file and sets Cancelled
            return;
        }
        if (it.retry_timer) {
            it.retry_timer->cancel();
            it.retry_timer.reset();
        }
        std::error_code ec;
        if (!it.row.part_path.empty()) std::filesystem::remove(u8path(it.row.part_path), ec);
        if (delete_file && it.row.state == db::State::Completed && !it.row.file_path.empty())
            std::filesystem::remove(u8path(it.row.file_path), ec);
        clear_resume(id, it);
        it.downloaded = 0;
        it.row.state = db::State::Cancelled;
        persist(it);
        snaps.push_back(make_snapshot(id, it));
    }
    notify(snaps);
}

void Engine::set_global_speed_limit(std::optional<std::uint64_t> bps) {
    const std::uint64_t v = bps.value_or(0);
    global_bucket_->set_rate(v);  // running transfers pick it up on their next read
    std::lock_guard dl(db_mu_);
    if (store_) store_->set_setting("global_speed_limit", std::to_string(v));
}

void Engine::set_speed_limit(DownloadId id, std::optional<std::uint64_t> bps) {
    std::lock_guard lk(mu_);
    auto f = items_.find(id);
    if (f == items_.end()) return;
    f->second.row.speed_limit = bps.value_or(0);
    f->second.bucket->set_rate(f->second.row.speed_limit);
    persist(f->second);
}

void Engine::set_priority(DownloadId id, int priority) {
    std::lock_guard lk(mu_);
    auto f = items_.find(id);
    if (f == items_.end()) return;
    f->second.row.priority = priority;
    persist(f->second);
    pump();
}

void Engine::move_to_queue(DownloadId id, std::int64_t queue_id) {
    std::lock_guard lk(mu_);
    auto f = items_.find(id);
    if (f == items_.end() || !queues_.count(queue_id)) return;
    f->second.row.queue_id = queue_id;
    persist(f->second);
    pump();
}

std::int64_t Engine::add_queue(const std::string& name, int max_active) {
    std::lock_guard lk(mu_);
    db::QueueRow q{0, name, std::max(1, max_active)};
    {
        std::lock_guard dl(db_mu_);
        q.id = store_->add_queue(q.name, q.max_active);
    }
    queues_[q.id] = QueueEntry{q, std::nullopt};
    return q.id;
}

void Engine::set_queue_limit(std::int64_t queue_id, int max_active) {
    std::lock_guard lk(mu_);
    auto q = queues_.find(queue_id);
    if (q == queues_.end()) return;
    q->second.row.max_active = std::max(1, max_active);
    {
        std::lock_guard dl(db_mu_);
        store_->update_queue(q->second.row);
    }
    pump();  // a raised limit starts more; a lowered one lets running downloads finish
}

void Engine::set_queue_schedule(std::int64_t queue_id, std::optional<scheduler::ScheduleRule> rule) {
    std::lock_guard lk(mu_);
    auto q = queues_.find(queue_id);
    if (q == queues_.end()) return;
    q->second.rule = rule;
    {
        std::lock_guard dl(db_mu_);
        store_->set_setting("queue." + std::to_string(queue_id) + ".schedule",
                            rule ? scheduler::to_json(*rule) : std::string("null"));
    }
    cv_.notify_all();  // the maintenance thread applies the new window on its next tick
}

std::vector<QueueInfo> Engine::queues() const {
    std::lock_guard lk(mu_);
    const auto t = local_now();
    std::vector<QueueInfo> out;
    for (const auto& [id, q] : queues_) out.push_back({id, q.row.name, q.row.max_active, q.rule, queue_open(id, t)});
    return out;
}

void Engine::subscribe(Observer obs) {
    std::lock_guard lk(obs_mu_);
    observers_.push_back(std::move(obs));
}

void Engine::shutdown() {
    {
        std::unique_lock lk(mu_);
        if (shut_ && !maintenance_.joinable() && threads_.empty()) return;  // already done
        shut_ = true;
        for (auto& [id, it] : items_) {
            if (it.job) {
                it.stop = Item::Stop::Shutdown;
                it.job->stop();
            }
            if (it.retry_timer) it.retry_timer->cancel();
        }
        // Running jobs save their resume state in on_job_done; a job that is
        // already finalizing (rename + hash) completes first.
        cv_.wait(lk, [this] { return running_ == 0; });
        stop_maintenance_ = true;
        cv_.notify_all();
    }
    if (maintenance_.joinable()) maintenance_.join();
    if (pool_) pool_->close_idle();
    guard_.reset();
    io_.stop();
    for (auto& t : threads_) t.join();
    threads_.clear();
    client_.reset();
    std::lock_guard dl(db_mu_);
    store_.reset();
}

Snapshot Engine::make_snapshot(DownloadId id, const Item& it) const {
    Snapshot s;
    s.id = id;
    s.state = it.row.state;
    s.file_name = it.row.file_name;
    s.total = it.row.total_size;
    s.downloaded = it.downloaded;
    s.bytes_per_sec = it.job ? it.bps : 0;
    s.active_connections = it.job ? it.connections : 0;
    s.error = it.row.error;
    s.url = it.row.url;
    s.file_path = it.row.file_path;
    s.queue_id = it.row.queue_id;
    s.priority = it.row.priority;
    s.speed_limit = it.row.speed_limit;
    for (const auto& seg : it.segments) s.segments.push_back({seg.begin, seg.pos, seg.end, seg.assigned});
    return s;
}

std::optional<Snapshot> Engine::snapshot(DownloadId id) const {
    std::lock_guard lk(mu_);
    auto f = items_.find(id);
    if (f == items_.end()) return std::nullopt;
    return make_snapshot(id, f->second);
}

std::vector<Snapshot> Engine::snapshots() const {
    std::lock_guard lk(mu_);
    std::vector<Snapshot> v;
    for (const auto& [id, it] : items_) v.push_back(make_snapshot(id, it));
    return v;
}

void Engine::notify(const std::vector<Snapshot>& snaps) {
    if (snaps.empty()) return;
    std::vector<Observer> obs;
    {
        std::lock_guard lk(obs_mu_);
        obs = observers_;
    }
    for (const auto& o : obs)
        for (const auto& s : snaps) o(s);
}

void Engine::maintenance_loop() {
    using clock = std::chrono::steady_clock;
    auto next_checkpoint = clock::now() + opts_.checkpoint_interval;
    std::unique_lock lk(mu_);
    while (!stop_maintenance_) {
        cv_.wait_for(lk, opts_.snapshot_interval, [this] { return stop_maintenance_; });
        if (stop_maintenance_) break;
        const auto now = clock::now();
        std::vector<Snapshot> snaps;
        std::vector<std::pair<DownloadId, std::shared_ptr<multipart::TransferJob>>> jobs;
        // Schedules: stop downloads whose queue window closed (they return to
        // Queued with their progress saved), then start whatever may run now.
        const auto lt = local_now();
        for (auto& [id, it] : items_)
            if (it.job && it.stop == Item::Stop::None && !queue_open(it.row.queue_id, lt)) {
                it.stop = Item::Stop::Schedule;
                it.job->stop();
            }
        pump();
        for (auto& [id, it] : items_) {
            if (!it.job) continue;
            const auto p = it.job->progress();
            it.downloaded = p.downloaded;
            it.connections = p.active_connections;
            it.segments = p.segments;
            if (p.total) it.row.total_size = p.total;
            if (!p.file_name.empty()) it.row.file_name = p.file_name;
            const double dt = std::chrono::duration<double>(now - it.last_time).count();
            if (dt > 0) {
                const double inst =
                    it.downloaded >= it.last_bytes ? static_cast<double>(it.downloaded - it.last_bytes) / dt : 0.0;
                it.bps = it.bps == 0 ? inst : 0.6 * it.bps + 0.4 * inst;  // smoothed
            }
            it.last_bytes = it.downloaded;
            it.last_time = now;
            snaps.push_back(make_snapshot(id, it));
            jobs.emplace_back(id, it.job);
        }
        const bool checkpoint = now >= next_checkpoint;
        if (checkpoint) next_checkpoint = now + opts_.checkpoint_interval;
        lk.unlock();
        notify(snaps);
        if (checkpoint) {
            for (auto& [id, job] : jobs) {
                auto rs = job->checkpoint();  // flushes; must not hold mu_
                if (!rs) continue;
                std::lock_guard l2(mu_);
                auto f = items_.find(id);
                if (f != items_.end() && f->second.job == job) save_resume(id, f->second, *rs);
            }
        }
        lk.lock();
    }
}

}  // namespace tdm::engine
