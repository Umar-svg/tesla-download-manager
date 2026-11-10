#include "net/http_client.hpp"

#include <array>
#include <atomic>
#include <mutex>
#include <optional>

#include "net/response_parser.hpp"

namespace tdm::net {
namespace {

bool is_redirect(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

bool header_safe(std::string_view s) { return s.find_first_of("\r\n") == std::string_view::npos; }

class Exchange final : public FetchHandle, public std::enable_shared_from_this<Exchange> {
public:
    Exchange(std::shared_ptr<ConnectionManager> pool, const ClientOptions& opts, Request req, HeadHandler on_head,
             BodySink sink, DoneHandler on_done)
        : pool_(std::move(pool)),
          opts_(opts),
          req_(std::move(req)),
          on_head_(std::move(on_head)),
          sink_(std::move(sink)),
          on_done_(std::move(on_done)),
          url_(req_.url),
          body_buf_(opts_.body_buffer_bytes ? opts_.body_buffer_bytes : 256 * 1024) {}

    void start() { acquire(false); }

    void cancel() override {
        std::shared_ptr<Connection> c;
        {
            std::lock_guard lk(m_);
            if (cancelled_.exchange(true)) return;
            c = conn_;
        }
        if (c) {
            asio::post(c->executor(), [c, self = shared_from_this()] {
                c->close();
                if (self->paused_) {  // no read is pending, so nothing else would notice the close
                    self->paused_ = false;
                    self->fail(Error::cancelled);
                }
            });
        }
    }

private:
    enum class Step { Continue, Complete, Stopped, Failed };

    void acquire(bool fresh) {
        pool_->acquire(url_, fresh, req_.connect_timeout,
                       [self = shared_from_this()](std::error_code ec, std::shared_ptr<Connection> c) {
                           self->on_connection(ec, std::move(c));
                       });
    }

    void on_connection(std::error_code ec, std::shared_ptr<Connection> c) {
        if (ec) return finish(cancelled_ ? make_error_code(Error::cancelled) : ec, false);
        {
            std::lock_guard lk(m_);
            conn_ = c;
        }
        if (cancelled_) return finish(Error::cancelled, false);
        timer_.emplace(c->executor());
        timed_out_ = false;
        head_buf_.clear();
        wbuf_ = HttpClient::serialize(req_, url_, opts_.user_agent);
        arm();
        c->async_write(asio::buffer(wbuf_), [self = shared_from_this()](std::error_code e, std::size_t) {
            if (e) return self->fail(e);
            self->read_head();
        });
    }

    void arm() {
        timer_->expires_after(req_.idle_timeout);
        timer_->async_wait([self = shared_from_this(), c = conn_](std::error_code ec) {
            if (ec) return;  // re-armed or cancelled
            self->timed_out_ = true;
            c->close();
        });
    }

    void read_head() {
        arm();
        conn_->async_read_some(asio::buffer(head_tmp_),
                               [self = shared_from_this()](std::error_code ec, std::size_t n) {
                                   if (n) self->head_buf_.append(self->head_tmp_.data(), n);
                                   if (ec) return self->fail(ec);
                                   self->parse_head();
                               });
    }

    void parse_head() {
        ParsedHead h;
        std::size_t consumed = 0;
        std::error_code ec;
        switch (parse_response_head(head_buf_, h, consumed, ec)) {
            case ParseStatus::NeedMore: return read_head();
            case ParseStatus::Error: return fail(ec);
            case ParseStatus::Done: break;
        }
        if (h.status < 200 && h.status != 101) {  // 1xx informational: skip, keep parsing
            head_buf_.erase(0, consumed);
            if (head_buf_.empty()) return read_head();
            return parse_head();
        }
        head_ = std::move(h);
        std::string leftover = head_buf_.substr(consumed);

        if (is_redirect(head_.status)) {
            const std::string* loc = nullptr;
            for (const auto& hd : head_.headers)
                if (core::iequals(hd.name, "Location")) loc = &hd.value;
            if (loc) {
                if (redirects_ >= req_.max_redirects) return finish(Error::too_many_redirects, false);
                auto next = core::resolve_location(url_, *loc);
                if (!next) return fail(Error::bad_response);
                ++redirects_;
                url_ = std::move(*next);
                if (head_.status == 303 && req_.method != "HEAD") req_.method = "GET";
                // The redirect body is not worth draining: drop the connection and follow.
                release_connection(false);
                retried_stale_ = false;
                return acquire(false);
            }
        }

        ResponseHead rh;
        rh.status = head_.status;
        rh.headers = head_.headers;
        rh.final_url = url_;
        rh.content_length = head_.content_length;
        rh.chunked = head_.chunked;
        kind_ = body_kind(head_, req_.method);
        if (kind_ == BodyKind::Length) remaining_ = *head_.content_length;
        if (on_head_ && !on_head_(rh)) return finish(Error::aborted_by_handler, false);
        if (kind_ == BodyKind::None) return finish({}, head_.keep_alive && leftover.empty());

        if (!leftover.empty()) {
            const auto step = deliver(std::as_bytes(std::span<const char>(leftover)));
            if (step != Step::Continue) return on_step(step);
        }
        read_body();
    }

    Step deliver(std::span<const std::byte> data) {
        auto sink = [this](std::span<const std::byte> s) -> std::size_t {
            const std::size_t took = sink_ ? std::min(sink_(s), s.size()) : s.size();
            consumed_ += took;
            return took;
        };
        switch (kind_) {
            case BodyKind::Length: {
                const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(remaining_, data.size()));
                const auto took = sink(data.first(n));
                remaining_ -= took;
                if (took < n) return Step::Stopped;
                if (remaining_ == 0) {
                    extra_bytes_ = data.size() > n;
                    return Step::Complete;
                }
                return Step::Continue;
            }
            case BodyKind::Chunked: {
                const auto r = chunked_.feed(data, sink);
                if (r.ec) {
                    step_error_ = r.ec;
                    return Step::Failed;
                }
                if (r.stopped) return Step::Stopped;
                if (r.done) {
                    extra_bytes_ = r.consumed < data.size();
                    return Step::Complete;
                }
                return Step::Continue;
            }
            case BodyKind::UntilClose:
                return sink(data) < data.size() ? Step::Stopped : Step::Continue;
            case BodyKind::None:
                return Step::Complete;
        }
        return Step::Complete;
    }

    void on_step(Step s) {
        switch (s) {
            case Step::Complete: return finish({}, head_.keep_alive && !extra_bytes_);
            case Step::Stopped: return finish({}, false);  // caller reached its boundary
            case Step::Failed: return fail(step_error_);
            case Step::Continue: return read_body();
        }
    }

    void read_body() {
        if (req_.read_gate) {
            auto self = shared_from_this();
            auto ex = conn_->executor();
            paused_ = true;
            const bool now = req_.read_gate([self, ex] {
                asio::post(ex, [self] {
                    if (!self->paused_ || self->finished_) return;
                    self->paused_ = false;
                    self->read_body();  // re-check the gate: another limit may still apply
                });
            });
            if (!now) {
                if (timer_) timer_->cancel();
                return;
            }
            paused_ = false;
        }
        read_body_now();
    }

    void read_body_now() {
        arm();
        std::size_t want = body_buf_.size();
        if (req_.read_limit) {
            const std::size_t cap = req_.read_limit();
            if (cap > 0 && cap < want) want = cap;
        }
        conn_->async_read_some(
            asio::buffer(body_buf_.data(), want),
            [self = shared_from_this()](std::error_code ec, std::size_t n) {
                if (n) {
                    const auto step = self->deliver(std::span<const std::byte>(self->body_buf_.data(), n));
                    if (step != Step::Continue) return self->on_step(step);
                }
                if (ec) {
                    const bool eof = ec == asio::error::eof || ec == asio::ssl::error::stream_truncated;
                    if (eof && self->kind_ == BodyKind::UntilClose && !self->cancelled_ && !self->timed_out_)
                        return self->finish({}, false);
                    return self->fail(ec);
                }
                self->read_body();
            });
    }

    void fail(std::error_code ec) {
        if (cancelled_) ec = Error::cancelled;
        else if (timed_out_) ec = Error::timeout;
        else if (ec == asio::error::eof || ec == asio::ssl::error::stream_truncated ||
                 ec == asio::error::connection_reset || ec == asio::error::connection_aborted) {
            // A pooled keep-alive connection the server already closed: retry once on a new one.
            if (conn_ && conn_->reused && !retried_stale_ && head_buf_.empty()) {
                retried_stale_ = true;
                release_connection(false);
                return acquire(true);
            }
            if (ec == asio::error::eof || ec == asio::ssl::error::stream_truncated) ec = Error::connection_closed;
        }
        finish(ec, false);
    }

    void release_connection(bool reusable) {
        if (timer_) timer_->cancel();
        std::shared_ptr<Connection> c;
        {
            std::lock_guard lk(m_);
            c = std::move(conn_);
            conn_.reset();
        }
        if (c) pool_->release(c, reusable);
    }

    void finish(std::error_code ec, bool reusable) {
        if (finished_) return;
        finished_ = true;
        release_connection(reusable && !ec);
        auto done = std::move(on_done_);
        if (done) done(ec, consumed_);
    }

    std::shared_ptr<ConnectionManager> pool_;
    ClientOptions opts_;
    Request req_;
    HeadHandler on_head_;
    BodySink sink_;
    DoneHandler on_done_;
    core::Url url_;

    std::mutex m_;                       // guards conn_ against cancel()
    std::shared_ptr<Connection> conn_;
    std::atomic<bool> cancelled_{false};

    std::optional<asio::steady_timer> timer_;
    bool timed_out_ = false;
    bool finished_ = false;
    bool paused_ = false;  // waiting for read_gate's resume (strand-only)
    bool retried_stale_ = false;
    int redirects_ = 0;

    std::string wbuf_;
    std::string head_buf_;
    std::array<char, 16 * 1024> head_tmp_{};
    std::vector<std::byte> body_buf_;

    ParsedHead head_;
    BodyKind kind_ = BodyKind::None;
    std::uint64_t remaining_ = 0;
    std::uint64_t consumed_ = 0;
    bool extra_bytes_ = false;
    ChunkedDecoder chunked_;
    std::error_code step_error_;
};

}  // namespace

HttpClient::HttpClient(std::shared_ptr<ConnectionManager> pool, ClientOptions opts)
    : pool_(std::move(pool)), opts_(std::move(opts)) {}

std::shared_ptr<FetchHandle> HttpClient::fetch(Request req, HeadHandler on_head, BodySink sink, DoneHandler on_done) {
    auto ex = std::make_shared<Exchange>(pool_, opts_, std::move(req), std::move(on_head), std::move(sink),
                                         std::move(on_done));
    ex->start();
    return ex;
}

std::string HttpClient::serialize(const Request& req, const core::Url& url, const std::string& user_agent) {
    std::string s;
    s.reserve(256);
    s += req.method;
    s += ' ';
    s += url.target;
    s += " HTTP/1.1\r\nHost: ";
    s += url.host_port();
    s += "\r\n";
    bool has_ua = false;
    for (const auto& h : req.headers)
        if (core::iequals(h.name, "User-Agent")) has_ua = true;
    if (!has_ua) s += "User-Agent: " + user_agent + "\r\n";
    s += "Accept: */*\r\nAccept-Encoding: identity\r\nConnection: keep-alive\r\n";
    if (req.range_from) s += "Range: bytes=" + std::to_string(*req.range_from) + "-\r\n";
    if (!req.if_range.empty() && header_safe(req.if_range)) s += "If-Range: " + req.if_range + "\r\n";
    for (const auto& h : req.headers) {
        if (h.name.empty() || !header_safe(h.name) || !header_safe(h.value)) continue;  // no header injection
        if (core::iequals(h.name, "Host") || core::iequals(h.name, "Range") || core::iequals(h.name, "If-Range") ||
            core::iequals(h.name, "Accept-Encoding") || core::iequals(h.name, "Connection"))
            continue;  // owned by the client
        s += h.name + ": " + h.value + "\r\n";
    }
    s += "\r\n";
    return s;
}

}  // namespace tdm::net
