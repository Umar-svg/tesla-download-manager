#pragma once

#include <system_error>

namespace tdm::net {

enum class Error {
    timeout = 1,          // connect / handshake / idle-read timeout
    too_many_redirects,
    bad_response,         // malformed status line, headers or chunk framing
    header_too_large,
    cancelled,            // FetchHandle::cancel()
    connection_closed,    // peer closed before the response was complete
    aborted_by_handler,   // HeadHandler returned false
};

const std::error_category& error_category() noexcept;

inline std::error_code make_error_code(Error e) noexcept {
    return {static_cast<int>(e), error_category()};
}

}  // namespace tdm::net

template <>
struct std::is_error_code_enum<tdm::net::Error> : std::true_type {};
