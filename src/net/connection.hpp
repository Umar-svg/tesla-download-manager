#pragma once
// One TCP (optionally TLS) connection. All operations must run on executor()
// (a strand); ConnectionManager and HttpClient guarantee that.

#include <asio.hpp>
#include <asio/ssl.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace tdm::net {

class Connection {
public:
    using tcp = asio::ip::tcp;

    Connection(asio::any_io_executor ex, std::string key, asio::ssl::context* tls_ctx)
        : ex_(ex), key_(std::move(key)), plain_(ex) {
        if (tls_ctx) tls_ = std::make_unique<asio::ssl::stream<tcp::socket>>(ex, *tls_ctx);
    }

    const asio::any_io_executor& executor() const noexcept { return ex_; }
    const std::string& key() const noexcept { return key_; }
    bool tls() const noexcept { return tls_ != nullptr; }
    tcp::socket& socket() noexcept { return tls_ ? tls_->next_layer() : plain_; }
    asio::ssl::stream<tcp::socket>* tls_stream() noexcept { return tls_.get(); }
    bool is_open() noexcept { return socket().is_open(); }

    template <class MutableBuffers, class Handler>
    void async_read_some(const MutableBuffers& b, Handler&& h) {
        if (tls_) tls_->async_read_some(b, std::forward<Handler>(h));
        else plain_.async_read_some(b, std::forward<Handler>(h));
    }

    template <class ConstBuffers, class Handler>
    void async_write(const ConstBuffers& b, Handler&& h) {
        if (tls_) asio::async_write(*tls_, b, std::forward<Handler>(h));
        else asio::async_write(plain_, b, std::forward<Handler>(h));
    }

    void close() noexcept {
        std::error_code ec;
        socket().shutdown(tcp::socket::shutdown_both, ec);
        socket().close(ec);
    }

    bool reused = false;                                   // served at least one response before
    std::chrono::steady_clock::time_point idle_since{};    // set while parked in the pool

private:
    asio::any_io_executor ex_;
    std::string key_;
    tcp::socket plain_;
    std::unique_ptr<asio::ssl::stream<tcp::socket>> tls_;
};

}  // namespace tdm::net
