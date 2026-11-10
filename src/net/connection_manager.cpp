#include "net/connection_manager.hpp"

#include <asio/ssl/host_name_verification.hpp>

#include "net/errors.hpp"

namespace tdm::net {

using tcp = asio::ip::tcp;

std::shared_ptr<ConnectionManager> ConnectionManager::create(asio::io_context& io, PoolOptions opts) {
    return std::shared_ptr<ConnectionManager>(new ConnectionManager(io, std::move(opts)));
}

ConnectionManager::ConnectionManager(asio::io_context& io, PoolOptions opts)
    : io_(io), opts_(std::move(opts)), tls_(make_tls_context(opts_.tls)) {
    if (opts_.max_per_host == 0) opts_.max_per_host = 1;
}

void ConnectionManager::acquire(const core::Url& url, bool fresh, std::chrono::seconds connect_timeout,
                                AcquireHandler h) {
    const auto key = url.pool_key();
    const auto now = std::chrono::steady_clock::now();
    std::shared_ptr<Connection> reuse;
    std::shared_ptr<Connection> evict;
    bool start_new = false;
    {
        std::lock_guard lk(m_);
        auto& host = hosts_[key];
        if (!fresh) {
            while (!host.idle.empty()) {
                auto c = std::move(host.idle.back());
                host.idle.pop_back();
                if (now - c->idle_since > opts_.idle_ttl || !c->is_open()) {
                    asio::post(c->executor(), [c] { c->close(); });
                    --host.open;
                    continue;
                }
                reuse = std::move(c);
                break;
            }
        }
        if (!reuse) {
            if (host.open < opts_.max_per_host) {
                ++host.open;
                start_new = true;
            } else if (fresh && !host.idle.empty()) {
                evict = std::move(host.idle.front());  // swap an idle slot for a fresh connection
                host.idle.erase(host.idle.begin());
                start_new = true;
            } else {
                host.waiters.push_back(Waiter{url, fresh, connect_timeout, std::move(h)});
                return;
            }
        }
    }
    if (evict) asio::post(evict->executor(), [evict] { evict->close(); });
    if (reuse) {
        ++reuses_;
        reuse->reused = true;
        asio::post(reuse->executor(), [reuse, h = std::move(h)] { h({}, reuse); });
        return;
    }
    if (start_new) connect(Waiter{url, fresh, connect_timeout, std::move(h)});
}

void ConnectionManager::release(const std::shared_ptr<Connection>& conn, bool reusable) {
    if (!conn) return;
    std::shared_ptr<Connection> give;
    AcquireHandler give_to;
    std::optional<Waiter> start;
    bool close_it = false;
    {
        std::lock_guard lk(m_);
        auto& host = hosts_[conn->key()];
        if (reusable && conn->is_open()) {
            if (!host.waiters.empty() && !host.waiters.front().fresh) {
                give = conn;
                give_to = std::move(host.waiters.front().handler);
                host.waiters.pop_front();
            } else if (!host.waiters.empty()) {
                close_it = true;  // a fresh waiter takes over this slot
                start = std::move(host.waiters.front());
                host.waiters.pop_front();
            } else {
                conn->idle_since = std::chrono::steady_clock::now();
                host.idle.push_back(conn);
            }
        } else {
            close_it = true;
            if (!host.waiters.empty()) {
                start = std::move(host.waiters.front());
                host.waiters.pop_front();
            } else if (host.open > 0) {
                --host.open;
            }
        }
    }
    if (close_it) asio::post(conn->executor(), [conn] { conn->close(); });
    if (give) {
        ++reuses_;
        give->reused = true;
        asio::post(give->executor(), [give, h = std::move(give_to)] { h({}, give); });
    }
    if (start) connect(std::move(*start));
}

void ConnectionManager::connect(Waiter w) {
    auto ex = asio::make_strand(io_);
    const bool https = w.url.scheme == "https";
    auto conn = std::make_shared<Connection>(ex, w.url.pool_key(), https ? tls_.get() : nullptr);
    auto resolver = std::make_shared<tcp::resolver>(ex);
    auto timer = std::make_shared<asio::steady_timer>(ex);
    auto timed_out = std::make_shared<bool>(false);
    auto self = shared_from_this();
    auto handler = std::make_shared<AcquireHandler>(std::move(w.handler));
    const std::string key = conn->key();
    const std::string host = w.url.host;

    auto done = [self, conn, timer, timed_out, handler, key](std::error_code ec) {
        timer->cancel();
        if (*timed_out) ec = Error::timeout;
        if (ec) return self->connect_failed(key, ec, conn, std::move(*handler));
        ++self->connects_;
        (*handler)({}, conn);  // already on conn's strand
    };

    asio::post(ex, [=, url = w.url, timeout = w.timeout]() mutable {
        timer->expires_after(timeout);
        timer->async_wait([conn, resolver, timed_out](std::error_code ec) {
            if (ec) return;
            *timed_out = true;
            resolver->cancel();
            conn->close();
        });
        resolver->async_resolve(
            url.host, std::to_string(url.port),
            [=](std::error_code ec, tcp::resolver::results_type results) {
                if (ec) return done(ec);
                asio::async_connect(conn->socket(), results, [=](std::error_code ec2, const tcp::endpoint&) {
                    if (ec2) return done(ec2);
                    std::error_code ignore;
                    conn->socket().set_option(tcp::no_delay(true), ignore);
                    if (self->opts_.receive_buffer_bytes > 0)
                        conn->socket().set_option(
                            asio::socket_base::receive_buffer_size(self->opts_.receive_buffer_bytes), ignore);
                    if (!conn->tls()) return done({});

                    auto* s = conn->tls_stream();
                    std::error_code addr_ec;
                    (void)asio::ip::make_address(host, addr_ec);
                    if (addr_ec) SSL_set_tlsext_host_name(s->native_handle(), host.c_str());  // SNI for names only
                    if (self->opts_.tls.verify_peer)
                        s->set_verify_callback(asio::ssl::host_name_verification(host));
                    s->async_handshake(asio::ssl::stream_base::client, [=](std::error_code ec3) { done(ec3); });
                });
            });
    });
}

void ConnectionManager::connect_failed(const std::string& key, std::error_code ec,
                                       const std::shared_ptr<Connection>& conn, AcquireHandler h) {
    ++failures_;
    conn->close();
    std::optional<Waiter> next;
    {
        std::lock_guard lk(m_);
        auto& host = hosts_[key];
        if (!host.waiters.empty()) {
            next = std::move(host.waiters.front());  // keep the slot for the next waiter
            host.waiters.pop_front();
        } else if (host.open > 0) {
            --host.open;
        }
    }
    h(ec, nullptr);
    if (next) connect(std::move(*next));
}

void ConnectionManager::close_idle() {
    std::vector<std::shared_ptr<Connection>> drop;
    {
        std::lock_guard lk(m_);
        for (auto& [k, host] : hosts_) {
            host.open -= std::min(host.open, host.idle.size());
            for (auto& c : host.idle) drop.push_back(std::move(c));
            host.idle.clear();
        }
    }
    for (auto& c : drop) asio::post(c->executor(), [c] { c->close(); });
}

ConnectionManager::Stats ConnectionManager::stats() const noexcept {
    return {connects_.load(), reuses_.load(), failures_.load()};
}

std::size_t ConnectionManager::idle_count(const core::Url& url) const {
    std::lock_guard lk(m_);
    auto it = hosts_.find(url.pool_key());
    return it == hosts_.end() ? 0 : it->second.idle.size();
}

std::size_t ConnectionManager::open_count(const core::Url& url) const {
    std::lock_guard lk(m_);
    auto it = hosts_.find(url.pool_key());
    return it == hosts_.end() ? 0 : it->second.open;
}

}  // namespace tdm::net
