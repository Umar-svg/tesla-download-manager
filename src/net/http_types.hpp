#pragma once
// HTTP client and connection-manager contracts (implemented in Phase 3).

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "core/http_util.hpp"

namespace tdm::net {

struct Header {
    std::string name, value;
};

struct Request {
    core::Url url;
    std::string method = "GET";
    std::vector<Header> headers;            // Host, User-Agent, Accept-Encoding added by the client
    std::optional<std::uint64_t> range_from; // emits "Range: bytes=N-"
    std::string if_range;                    // strong ETag or Last-Modified (D3)
    int max_redirects = 10;
    std::chrono::seconds connect_timeout{15};
    std::chrono::seconds idle_timeout{30};   // no bytes received for this long → Timeout
    // Optional back-pressure / rate-limit hook, consulted before each body read.
    // Return true to read now; or return false and call `resume` later (any
    // thread) to continue. The idle timeout is suspended while paused.
    std::function<bool(std::function<void()> resume)> read_gate;
    // Optional cap on the size of the next body read (0 = whole buffer). Used
    // with read_gate to pace reads in small reserved quanta.
    std::function<std::size_t()> read_limit;
};

struct ResponseHead {
    int status = 0;
    std::vector<Header> headers;
    core::Url final_url;                     // after redirects
    std::optional<std::uint64_t> content_length;
    bool chunked = false;
    const std::string* find(std::string_view name) const {  // case-insensitive
        for (const auto& h : headers)
            if (core::iequals(h.name, name)) return &h.value;
        return nullptr;
    }
};

// Body bytes are delivered as they arrive; the callback returns how many bytes
// it consumed. Returning fewer than offered closes the stream (segment boundary
// reached) and returns the connection only if the response was fully read.
using BodySink = std::function<std::size_t(std::span<const std::byte>)>;
using HeadHandler = std::function<bool(const ResponseHead&)>;  // false = abort before body
using DoneHandler = std::function<void(std::error_code, std::uint64_t bytes_consumed)>;

}  // namespace tdm::net
