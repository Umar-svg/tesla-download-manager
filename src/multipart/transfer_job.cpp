#include "multipart/transfer_job.hpp"

#include <algorithm>
#include <thread>

#include "core/http_util.hpp"

namespace tdm::multipart {
namespace {

class Category : public std::error_category {
public:
    const char* name() const noexcept override { return "tdm.transfer"; }
    std::string message(int ev) const override {
        switch (static_cast<TransferError>(ev)) {
            case TransferError::http_status: return "server returned an error status";
            case TransferError::too_many_restarts: return "the file kept changing on the server";
            case TransferError::size_mismatch: return "downloaded size does not match the announced size";
            case TransferError::hash_mismatch: return "SHA-256 does not match the expected value";
            case TransferError::insufficient_space: return "not enough free disk space";
            case TransferError::unexpected_response: return "unexpected server response";
        }
        return "transfer error";
    }
};

std::filesystem::path u8path(const std::string& s) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::string header_or_empty(const net::ResponseHead& h, std::string_view name) {
    const auto* v = h.find(name);
    return v ? std::string(core::trim(*v)) : std::string();
}

}  // namespace

const std::error_category& transfer_category() noexcept {
    static Category c;
    return c;
}

std::shared_ptr<TransferJob> TransferJob::create(asio::io_context& io, net::HttpClient& client, TransferOptions opts,
                                                 DoneFn done) {
    return std::shared_ptr<TransferJob>(new TransferJob(io, client, std::move(opts), std::move(done)));
}

TransferJob::TransferJob(asio::io_context& io, net::HttpClient& client, TransferOptions opts, DoneFn done)
    : io_(io), client_(client), opts_(std::move(opts)), done_(std::move(done)), strand_(asio::make_strand(io)) {
    max_conns_ = std::clamp(opts_.max_connections, 1, 32);
}

void TransferJob::start() {
    std::lock_guard lk(mu_);
    if (phase_ != Phase::Idle) return;
    std::error_code ec;
    std::filesystem::create_directories(opts_.directory, ec);
    if (opts_.resume) resume_from(*opts_.resume);
    else start_probe();
}

void TransferJob::stop() {
    std::lock_guard lk(mu_);
    if (stopping_ || phase_ == Phase::Finishing || phase_ == Phase::Done) return;
    stopping_ = true;
    user_stop_ = true;
    cancel_all();
    maybe_finish_stopped();
}

TransferProgress TransferJob::progress() const {
    std::lock_guard lk(mu_);
    TransferProgress p;
    p.file_name = file_name_;
    p.total = total_;
    p.segmented = segmented_;
    // Only connections that are actually receiving (a request still waiting for
    // a pool slot or for its response head is not a working connection).
    p.active_connections = static_cast<int>(
        std::count_if(conns_.begin(), conns_.end(), [](const auto& kv) { return kv.second.got_head; }));
    if (planner_) {
        p.downloaded = planner_->downloaded();
        p.segments = planner_->snapshot();
    } else {
        p.downloaded = single_pos_;
    }
    return p;
}

std::optional<ResumeState> TransferJob::checkpoint() {
    std::shared_ptr<fs::FileWriter> w;
    ResumeState rs;
    unsigned gen = 0;
    {
        std::lock_guard lk(mu_);
        if (phase_ != Phase::Running || stopping_ || !segmented_ || !planner_ || !writer_ || !total_ ||
            (etag_.empty() && last_modified_.empty()))
            return std::nullopt;
        // on_body enqueues and commits under mu_, so every byte this snapshot
        // claims is already in the writer's queue.
        rs = ResumeState{final_url_, etag_, last_modified_, *total_, part_path_, file_name_, planner_->snapshot()};
        for (auto& s : rs.segments) s.assigned = false;
        w = writer_;
        gen = gen_;
    }
    if (w->flush()) return std::nullopt;
    std::lock_guard lk(mu_);
    if (gen != gen_) return std::nullopt;  // restarted meanwhile: the snapshot belongs to a discarded file
    return rs;
}

std::unique_ptr<fs::FileWriter> TransferJob::make_writer() const {
    return opts_.writer_factory ? opts_.writer_factory(opts_.write_budget) : fs::make_file_writer(opts_.write_budget);
}

std::size_t TransferJob::read_quantum() const {
    std::uint64_t slowest = 0;
    for (const auto& l : opts_.limiters)
        if (const auto r = l->rate(); r > 0 && (slowest == 0 || r < slowest)) slowest = r;
    if (slowest == 0) return 0;
    // Each connection reserves one quantum ahead of its read, so the reserved
    // backlog is n * quantum. Keep it near 1/16 s of the limit regardless of
    // the connection count: smooth pacing without a long tail at the end.
    const std::uint64_t n = std::max<std::size_t>(1, conns_.size());
    return static_cast<std::size_t>(std::clamp<std::uint64_t>(slowest / (16 * n), 16 * 1024, 256 * 1024));
}

void TransferJob::charge(Conn& c, std::uint64_t n) {
    if (n <= c.reserved) {
        c.reserved -= n;  // already paid for
        return;
    }
    const auto extra = n - c.reserved;  // e.g. bytes that arrived with the response head
    c.reserved = 0;
    for (const auto& l : opts_.limiters) l->consume(extra);
}

void TransferJob::refund(Conn& c) {
    if (c.reserved == 0) return;
    for (const auto& l : opts_.limiters) l->refund(c.reserved);
    c.reserved = 0;
}

std::string TransferJob::validator() const {
    if (!etag_.empty() && !etag_.starts_with("W/")) return etag_;  // weak ETags are not allowed in If-Range
    return last_modified_;
}

void TransferJob::start_probe() {
    phase_ = Phase::Probing;
    launch(true, std::nullopt);
}

void TransferJob::resume_from(const ResumeState& rs) {
    final_url_ = rs.url;
    etag_ = rs.etag;
    last_modified_ = rs.last_modified;
    total_ = rs.total;
    file_name_ = rs.file_name;
    part_path_ = rs.part_path;
    auto planner = SegmentPlanner::restore(rs.total, {opts_.min_split, 64 * 1024}, rs.segments);
    std::error_code ec = std::make_error_code(std::errc::invalid_argument);
    if (planner && (!etag_.empty() || !last_modified_.empty())) {
        writer_ = make_writer();
        ec = writer_->open(part_path_, total_, /*keep_existing=*/true);
    }
    if (!planner || ec) {
        // Not resumable (missing/foreign part file, bad table, no validators): start over.
        writer_.reset();
        std::error_code rm;
        if (!part_path_.empty()) std::filesystem::remove(part_path_, rm);
        etag_.clear();
        last_modified_.clear();
        total_.reset();
        file_name_.clear();
        start_probe();
        return;
    }
    planner_ = std::move(*planner);
    segmented_ = true;
    phase_ = Phase::Running;
    if (planner_->complete()) return begin_finalize();
    fill_connections();
}

void TransferJob::launch(bool probe, std::optional<Assignment> a) {
    const int id = ++next_id_;
    Conn c;
    c.probe = probe;
    if (a) {
        c.seg = a->id;
        c.offset = a->offset;
    }
    net::Request r;
    r.url = probe ? opts_.url : final_url_;
    r.headers = opts_.extra_headers;
    r.connect_timeout = opts_.connect_timeout;
    r.idle_timeout = opts_.idle_timeout;
    if (a) {
        r.range_from = a->offset;
        r.if_range = validator();
    }
    auto self = shared_from_this();
    const unsigned gen = gen_;
    r.read_limit = [self]() -> std::size_t {
        std::lock_guard lk(self->mu_);
        return self->read_quantum();
    };
    r.read_gate = [self, gen, id](std::function<void()> resume) {
        std::chrono::nanoseconds wait{0};
        {
            std::lock_guard lk(self->mu_);
            if (gen != self->gen_ || !self->writer_) return true;
            if (!self->writer_->request_room(resume)) return false;  // disk back-pressure first
            auto it = self->conns_.find(id);
            if (it == self->conns_.end()) return true;
            // Bandwidth: hold one quantum of reserved tokens before reading.
            // Unused tokens from the previous read are kept (already paid
            // for), so only the shortfall is reserved. Reservations from all
            // connections queue up in the bucket, which staggers them.
            if (const auto q = self->read_quantum()) {
                auto& c = it->second;
                const std::uint64_t need = q > c.reserved ? q - c.reserved : 0;
                c.reserved += need;
                if (need)
                    for (const auto& l : self->opts_.limiters) wait = std::max(wait, l->reserve(need));
            } else {
                self->refund(it->second);  // limits lifted
            }
        }
        if (wait.count() == 0) return true;
        auto t = std::make_shared<asio::steady_timer>(self->io_, wait);
        t->async_wait([t, resume = std::move(resume)](std::error_code) { resume(); });
        return false;
    };
    conns_.emplace(id, std::move(c));
    ++connections_used_;
    // Callbacks never run inline from fetch(); they block on mu_ until we return.
    conns_[id].handle = client_.fetch(
        std::move(r), [self, gen, id](const net::ResponseHead& h) { return self->on_head(gen, id, h); },
        [self, gen, id](std::span<const std::byte> d) { return self->on_body(gen, id, d); },
        [self, gen, id](std::error_code ec, std::uint64_t) { self->on_done(gen, id, ec); });
}

bool TransferJob::on_head(unsigned gen, int id, const net::ResponseHead& h) {
    std::lock_guard lk(mu_);
    if (gen != gen_ || stopping_ || phase_ == Phase::Restarting) return false;
    auto it = conns_.find(id);
    if (it == conns_.end()) return false;
    return it->second.probe ? probe_head(it->second, h) : segment_head(it->second, h);
}

bool TransferJob::probe_head(Conn& c, const net::ResponseHead& h) {
    if (h.status != 200) {
        c.http_status = h.status;
        if (const auto* ra = h.find("Retry-After"))
            c.retry_after = core::parse_retry_after(*ra, std::chrono::system_clock::now());
        return false;
    }
    final_url_ = h.final_url;
    etag_ = header_or_empty(h, "ETag");
    last_modified_ = header_or_empty(h, "Last-Modified");
    const auto* ar = h.find("Accept-Ranges");
    const bool ranges = ar && core::iequals(core::trim(*ar), "bytes");
    total_ = h.chunked ? std::nullopt : h.content_length;

    std::string name = opts_.file_name;
    if (name.empty())
        if (const auto* cd = h.find("Content-Disposition"))
            if (auto n = core::content_disposition_filename(*cd)) name = *n;
    if (name.empty()) name = core::filename_from_url(final_url_);
    file_name_ = core::sanitize_filename(name);
    part_path_ = opts_.directory / u8path(file_name_ + ".tdmpart");

    if (total_) {
        std::error_code sec;
        const auto sp = std::filesystem::space(opts_.directory, sec);
        if (!sec && sp.available < *total_) {
            fail(TransferError::insufficient_space);
            return false;
        }
    }
    writer_ = make_writer();
    if (auto ec = writer_->open(part_path_, total_, false)) {
        writer_.reset();
        disk_error_ = true;
        fail(ec);
        return false;
    }
    c.got_head = true;
    segmented_ = total_ && ranges && *total_ > 0;
    phase_ = Phase::Running;
    if (segmented_) {
        planner_.emplace(*total_, SegmentPlanner::Options{opts_.min_split, 64 * 1024});
        const auto a = planner_->acquire();  // [0, total): the probe connection keeps streaming it
        c.seg = a->id;
        c.offset = 0;
        fill_connections();  // inserts into conns_; references to elements stay valid
    }
    return true;
}

bool TransferJob::segment_head(Conn& c, const net::ResponseHead& h) {
    if (h.status == 206) {
        const auto* crv = h.find("Content-Range");
        const auto cr = crv ? core::parse_content_range(*crv) : std::nullopt;
        bool ok = cr && cr->first && *cr->first == c.offset && cr->complete_length && total_ &&
                  *cr->complete_length == *total_;
        // A different validator means a different representation (RFC 9110 §13.1.5): never mix them.
        if (ok && !etag_.empty()) {
            const auto e = header_or_empty(h, "ETag");
            if (!e.empty() && e != etag_) ok = false;
        }
        if (ok && !last_modified_.empty()) {
            const auto l = header_or_empty(h, "Last-Modified");
            if (!l.empty() && l != last_modified_) ok = false;
        }
        if (!ok) {
            request_restart();
            return false;
        }
        c.got_head = true;
        return true;
    }
    if (h.status == 200 || h.status == 416) {  // If-Range failed / Range ignored / file shrank
        request_restart();
        return false;
    }
    c.http_status = h.status;
    if (const auto* ra = h.find("Retry-After"))
        c.retry_after = core::parse_retry_after(*ra, std::chrono::system_clock::now());
    return false;
}

std::size_t TransferJob::on_body(unsigned gen, int id, std::span<const std::byte> data) {
    std::lock_guard lk(mu_);
    if (gen != gen_ || stopping_ || phase_ != Phase::Running || !writer_ || writer_->error()) return 0;
    auto it = conns_.find(id);
    if (it == conns_.end()) return 0;
    Conn& c = it->second;
    if (segmented_) {
        if (!c.seg) return 0;
        const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(planner_->limit(*c.seg), data.size()));
        if (n == 0) return 0;  // segment boundary reached (possibly moved by a split)
        // Enqueue and commit under the same lock so a progress snapshot never
        // claims bytes that are not yet queued for the disk.
        writer_->write(planner_->position(*c.seg), data.first(n));
        planner_->commit(*c.seg, n);
        bytes_received_ += n;
        charge(c, n);
        return n;
    }
    writer_->write(single_pos_, data);
    single_pos_ += data.size();
    bytes_received_ += data.size();
    charge(c, data.size());
    return data.size();
}

void TransferJob::on_done(unsigned gen, int id, std::error_code ec) {
    std::lock_guard lk(mu_);
    auto it = conns_.find(id);
    if (it == conns_.end()) return;
    Conn c = std::move(it->second);
    conns_.erase(it);
    refund(c);
    if (gen == gen_ && planner_ && c.seg) planner_->release(*c.seg);

    if (stopping_) return maybe_finish_stopped();
    if (phase_ == Phase::Restarting) {
        if (conns_.empty()) do_restart();
        return;
    }
    if (gen != gen_) return;
    if (writer_) {
        if (auto wec = writer_->error()) {
            disk_error_ = true;
            return fail(wec);
        }
    }

    if (c.probe && !c.got_head) {
        if (c.http_status) return fail(TransferError::http_status, c.http_status, c.retry_after);
        return fail(ec ? ec : make_error_code(TransferError::unexpected_response));
    }
    if (!segmented_) {  // single stream on the probe connection: no partial resume possible
        if (ec) return fail(ec);
        if (total_ && single_pos_ != *total_) return fail(TransferError::size_mismatch);
        return begin_finalize();
    }

    const bool failed = ec || c.http_status;
    if (!failed) {
        seg_failures_ = 0;
    } else {
        retry::Failure f;
        if (c.http_status) {
            f.kind = retry::FailureKind::HttpStatus;
            f.http_status = c.http_status;
            f.retry_after = c.retry_after;
            if (c.http_status == 429 || c.http_status == 503) max_conns_ = std::max(1, max_conns_ / 2);
        } else {
            f.kind = ec == net::Error::timeout ? retry::FailureKind::Timeout : retry::FailureKind::Network;
        }
        const auto d = opts_.retry.decide(f, ++seg_failures_, rng_);
        if (d.action == retry::Action::RetrySameState) {
            auto t = std::make_shared<asio::steady_timer>(strand_, d.delay);
            timers_.push_back(t);
            t->async_wait([self = shared_from_this(), t](std::error_code tec) {
                if (tec) return;
                std::lock_guard lk2(self->mu_);
                std::erase(self->timers_, t);
                self->fill_connections();
            });
        } else if (conns_.empty()) {
            if (c.http_status) return fail(TransferError::http_status, c.http_status, c.retry_after);
            return fail(ec);
        }
        // GiveUp while other connections are alive: they will pick up the released segment.
    }

    if (planner_->complete()) {
        if (conns_.empty()) begin_finalize();
        return;
    }
    if (!failed) fill_connections();
}

void TransferJob::fill_connections() {
    if (stopping_ || phase_ != Phase::Running || !planner_) return;
    while (static_cast<int>(conns_.size()) < max_conns_) {
        auto a = planner_->acquire();
        if (!a) break;
        launch(false, a);
    }
}

void TransferJob::request_restart() {
    if (phase_ == Phase::Restarting) return;
    if (++restarts_ > opts_.max_restarts) return fail(TransferError::too_many_restarts);
    phase_ = Phase::Restarting;
    cancel_all();
}

void TransferJob::do_restart() {
    ++gen_;
    if (writer_) {
        writer_->discard();
        writer_.reset();
    }
    planner_.reset();
    segmented_ = false;
    total_.reset();
    single_pos_ = 0;
    etag_.clear();
    last_modified_.clear();
    seg_failures_ = 0;
    max_conns_ = std::clamp(opts_.max_connections, 1, 32);
    start_probe();
}

void TransferJob::fail(std::error_code ec, int http_status, std::optional<std::chrono::seconds> retry_after) {
    if (stopping_) return;
    stopping_ = true;
    fail_ec_ = ec ? ec : make_error_code(TransferError::unexpected_response);
    fail_status_ = http_status;
    fail_retry_after_ = retry_after;
    cancel_all();
    maybe_finish_stopped();
}

void TransferJob::cancel_all() {
    for (auto& [id, c] : conns_)
        if (c.handle) c.handle->cancel();
    for (auto& t : timers_) t->cancel();
    timers_.clear();
}

void TransferJob::maybe_finish_stopped() {
    if (!stopping_ || !conns_.empty() || phase_ == Phase::Done || phase_ == Phase::Finishing) return;
    phase_ = Phase::Done;
    TransferResult r;
    r.ec = user_stop_ ? std::make_error_code(std::errc::operation_canceled) : fail_ec_;
    r.http_status = fail_status_;
    r.retry_after = fail_retry_after_;
    r.file_name = file_name_;
    r.segmented = segmented_;
    r.restarts = restarts_;
    r.connections_used = connections_used_;
    r.bytes_received = bytes_received_;
    r.final_url = final_url_;
    r.disk_error = disk_error_;
    if (writer_) {
        const bool resumable = segmented_ && planner_ && total_ && (!etag_.empty() || !last_modified_.empty());
        if (resumable && !writer_->close()) {  // close() flushes: the snapshot below is durable
            r.resume = ResumeState{final_url_, etag_, last_modified_, *total_, part_path_, file_name_,
                                   planner_->snapshot()};
            for (auto& s : r.resume->segments) s.assigned = false;
        } else {
            writer_->discard();
        }
        writer_.reset();
    }
    asio::post(io_, [self = shared_from_this(), r = std::move(r)]() mutable { self->deliver(std::move(r)); });
}

void TransferJob::begin_finalize() {
    phase_ = Phase::Finishing;
    // Flushing, renaming and hashing block for a while: keep them off the network threads.
    std::thread([self = shared_from_this()] { self->finalize_blocking(); }).detach();
}

void TransferJob::finalize_blocking() {
    TransferResult r;
    std::filesystem::path final_path;
    std::uint64_t expected = 0;
    {
        std::lock_guard lk(mu_);
        r.file_name = file_name_;
        r.segmented = segmented_;
        r.restarts = restarts_;
        r.connections_used = connections_used_;
        r.bytes_received = bytes_received_;
        r.final_url = final_url_;
        final_path = opts_.directory / u8path(file_name_);
        expected = total_ ? *total_ : single_pos_;
    }
    std::filesystem::path actual;
    std::error_code ec = writer_->finalize(final_path, actual);
    if (ec) r.disk_error = true;
    if (!ec) {
        r.path = actual;
        r.size = std::filesystem::file_size(actual, ec);
        if (!ec && r.size != expected) ec = TransferError::size_mismatch;
    }
    if (!ec) r.sha256 = fs::sha256_file(actual, ec);
    if (!ec && !opts_.expected_sha256.empty() && !core::iequals(r.sha256, opts_.expected_sha256))
        ec = TransferError::hash_mismatch;
    r.ec = ec;
    {
        std::lock_guard lk(mu_);
        writer_.reset();
        phase_ = Phase::Done;
    }
    deliver(std::move(r));
}

void TransferJob::deliver(TransferResult r) {
    DoneFn d;
    {
        std::lock_guard lk(mu_);
        d = std::move(done_);
        done_ = nullptr;
    }
    if (d) d(r);
}

}  // namespace tdm::multipart
