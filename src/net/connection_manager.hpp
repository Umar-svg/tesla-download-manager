#pragma once
// Keep-alive connection pool with a per-host connection cap.
//
// Thread-safe: acquire()/release() may be called from any thread. Handlers
// always run on the connection's own strand, never inline under the pool lock.

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "core/http_util.hpp"
#include "net/connection.hpp"
#include "net/tls_context.hpp"

namespace tdm::net {

struct PoolOptions {
    std::size_t max_per_host = 8;               // open (busy + idle) connections per scheme://host:port
    std::chrono::seconds idle_ttl{30};          // idle connections older than this are not reused
    int receive_buffer_bytes = 1 << 20;         // SO_RCVBUF; 0 = OS default
    TlsOptions tls;
};

class ConnectionManager : public std::enable_shared_from_this<ConnectionManager> {
public:
    using AcquireHandler = std::function<void(std::error_code, std::shared_ptr<Connection>)>;

    struct Stats {
        std::uint64_t connects = 0;   // new connections established
        std::uint64_t reuses = 0;     // requests served by a pooled connection
        std::uint64_t failures = 0;   // failed connection attempts
    };

    static std::shared_ptr<ConnectionManager> create(asio::io_context& io, PoolOptions opts = {});

    // fresh = bypass idle connections (used after a stale keep-alive failure).
    void acquire(const core::Url& url, bool fresh, std::chrono::seconds connect_timeout, AcquireHandler h);

    // Return a connection. reusable = the response was read completely and the
    // server allows keep-alive. Otherwise the connection is closed.
    void release(const std::shared_ptr<Connection>& conn, bool reusable);

    void close_idle();
    Stats stats() const noexcept;
    std::size_t idle_count(const core::Url& url) const;
    std::size_t open_count(const core::Url& url) const;
    const PoolOptions& options() const noexcept { return opts_; }

private:
    ConnectionManager(asio::io_context& io, PoolOptions opts);

    struct Waiter {
        core::Url url;
        bool fresh = false;
        std::chrono::seconds timeout{15};
        AcquireHandler handler;
    };
    struct Host {
        std::size_t open = 0;
        std::vector<std::shared_ptr<Connection>> idle;
        std::deque<Waiter> waiters;
    };

    void connect(Waiter w);  // caller has already reserved a slot
    void connect_failed(const std::string& key, std::error_code ec, const std::shared_ptr<Connection>& conn,
                        AcquireHandler h);

    asio::io_context& io_;
    PoolOptions opts_;
    std::shared_ptr<asio::ssl::context> tls_;
    mutable std::mutex m_;
    std::unordered_map<std::string, Host> hosts_;
    std::atomic<std::uint64_t> connects_{0}, reuses_{0}, failures_{0};
};

}  // namespace tdm::net
