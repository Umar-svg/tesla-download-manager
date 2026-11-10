#pragma once
// Protocol helpers shared by every module. Pure functions: no I/O, no Qt.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace tdm::core {

std::string_view trim(std::string_view s) noexcept;
bool iequals(std::string_view a, std::string_view b) noexcept;

// RFC 9110 §14.4  Content-Range: "bytes 0-499/1234", "bytes 0-499/*", "bytes */1234"
struct ContentRange {
    std::optional<std::uint64_t> first;            // absent for unsatisfied-range
    std::optional<std::uint64_t> last;             // inclusive
    std::optional<std::uint64_t> complete_length;  // absent for "*"
};
std::optional<ContentRange> parse_content_range(std::string_view value);

// RFC 9110 §5.6.7 IMF-fixdate ("Sun, 06 Nov 1994 08:49:37 GMT").
// The obsolete RFC 850 and asctime forms are also accepted (§5.6.7 SHOULD).
std::optional<std::chrono::system_clock::time_point> parse_http_date(std::string_view value);
std::string format_http_date(std::chrono::system_clock::time_point tp);

// RFC 9110 §10.2.3 Retry-After: delay-seconds or HTTP-date. Returns the delay
// relative to `now`, never negative.
std::optional<std::chrono::seconds> parse_retry_after(
    std::string_view value, std::chrono::system_clock::time_point now);

// RFC 6266 Content-Disposition filename; filename* (RFC 8187, UTF-8) wins over
// filename. Result is NOT sanitised; pass it through sanitize_filename().
std::optional<std::string> content_disposition_filename(std::string_view value);

// Makes a UTF-8 name safe for Windows: strips path parts, replaces reserved
// characters, avoids reserved device names, trims trailing dots/spaces.
std::string sanitize_filename(std::string_view name);

struct Url {
    std::string scheme;   // "http" or "https", lower case
    std::string host;     // lower case; IPv6 without brackets
    std::uint16_t port = 0;
    std::string target;   // origin-form: path + optional "?query", never empty
    bool default_port() const noexcept;
    std::string host_port() const;  // "host:port" (port omitted when default)
    std::string to_string() const;
    std::string pool_key() const;   // scheme://host:port
};
std::optional<Url> parse_url(std::string_view s);

// Resolve a Location header against the URL it came from (RFC 3986 §5.2,
// simplified: absolute, scheme-relative, absolute-path, relative-path).
std::optional<Url> resolve_location(const Url& base, std::string_view location);

// Last path segment of the URL, percent-decoded, or empty.
std::string filename_from_url(const Url& url);

std::string percent_decode(std::string_view s);

}  // namespace tdm::core
