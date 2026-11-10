#pragma once
// Asynchronous HTTP/1.1 client on top of ConnectionManager.
//
// fetch() streams one response: on_head runs once with the final (post-redirect)
// head; body bytes go to the sink as they arrive; on_done runs exactly once.
// All three callbacks run on the connection's strand: never concurrently for
// one fetch, but different fetches may run in parallel on different threads.

#include <memory>
#include <string>

#include "core/version.hpp"
#include "net/connection_manager.hpp"
#include "net/errors.hpp"
#include "net/http_types.hpp"

namespace tdm::net {

class FetchHandle {
public:
    virtual ~FetchHandle() = default;
    virtual void cancel() = 0;  // thread-safe; on_done receives Error::cancelled
};

struct ClientOptions {
    std::string user_agent = kUserAgent;
    std::size_t body_buffer_bytes = 256 * 1024;
};

class HttpClient {
public:
    explicit HttpClient(std::shared_ptr<ConnectionManager> pool, ClientOptions opts = {});

    std::shared_ptr<FetchHandle> fetch(Request req, HeadHandler on_head, BodySink sink, DoneHandler on_done);

    ConnectionManager& pool() noexcept { return *pool_; }

    // Exposed for tests: the exact bytes sent for a request.
    static std::string serialize(const Request& req, const core::Url& url, const std::string& user_agent);

private:
    std::shared_ptr<ConnectionManager> pool_;
    ClientOptions opts_;
};

}  // namespace tdm::net
