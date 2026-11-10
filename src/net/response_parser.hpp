#pragma once
// Incremental HTTP/1.1 response-head parser and chunked-body decoder (RFC 9112).
// Pure code (no I/O) so it can be unit-tested byte by byte.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "net/errors.hpp"
#include "net/http_types.hpp"

namespace tdm::net {

inline constexpr std::size_t kMaxHeadBytes = 64 * 1024;

struct ParsedHead {
    int status = 0;
    int minor_version = 1;
    std::vector<Header> headers;
    std::optional<std::uint64_t> content_length;
    bool chunked = false;              // Transfer-Encoding ends in "chunked"
    bool other_transfer_coding = false;  // TE present but not chunked → read until close
    bool keep_alive = true;
};

enum class ParseStatus { NeedMore, Done, Error };

// Parses one response head from the start of `buf`. On Done, `consumed` is the
// number of bytes up to and including the blank line.
ParseStatus parse_response_head(std::string_view buf, ParsedHead& out, std::size_t& consumed,
                                std::error_code& ec);

// Bytes of body expected for this response to a request with `method`.
// nullopt = read until the connection closes.
enum class BodyKind { None, Length, Chunked, UntilClose };
BodyKind body_kind(const ParsedHead& h, std::string_view method);

class ChunkedDecoder {
public:
    struct Result {
        std::size_t consumed = 0;  // input bytes used
        bool done = false;         // last chunk and trailers consumed
        bool stopped = false;      // sink took fewer bytes than offered
        std::error_code ec;
    };

    // Decodes `in`, passing chunk data to `sink` (returns bytes it accepted).
    template <class Sink>
    Result feed(std::span<const std::byte> in, Sink&& sink);

    bool done() const noexcept { return state_ == State::Done; }

private:
    enum class State { Size, Data, DataCR, DataLF, Trailer, Done };
    bool finish_size_line(std::error_code& ec);
    State state_ = State::Size;
    std::uint64_t remaining_ = 0;
    std::string line_;
    std::size_t trailer_bytes_ = 0;
};

template <class Sink>
ChunkedDecoder::Result ChunkedDecoder::feed(std::span<const std::byte> in, Sink&& sink) {
    Result r;
    std::size_t i = 0;
    while (i < in.size() && state_ != State::Done) {
        const char c = static_cast<char>(in[i]);
        switch (state_) {
            case State::Size:
                ++i;
                if (c == '\n') {
                    if (!finish_size_line(r.ec)) { r.consumed = i; return r; }
                } else if (line_.size() > 1024) {
                    r.ec = make_error_code(Error::bad_response);
                    r.consumed = i;
                    return r;
                } else {
                    line_ += c;
                }
                break;
            case State::Data: {
                const auto n = static_cast<std::size_t>(
                    std::min<std::uint64_t>(remaining_, in.size() - i));
                const std::size_t took = sink(in.subspan(i, n));
                i += took;
                remaining_ -= took;
                if (took < n) {
                    r.stopped = true;
                    r.consumed = i;
                    return r;
                }
                if (remaining_ == 0) state_ = State::DataCR;
                break;
            }
            case State::DataCR:
                ++i;
                if (c == '\r') state_ = State::DataLF;
                else if (c == '\n') state_ = State::Size;
                else { r.ec = make_error_code(Error::bad_response); r.consumed = i; return r; }
                break;
            case State::DataLF:
                ++i;
                if (c != '\n') { r.ec = make_error_code(Error::bad_response); r.consumed = i; return r; }
                state_ = State::Size;
                break;
            case State::Trailer:
                ++i;
                if (++trailer_bytes_ > kMaxHeadBytes) {
                    r.ec = make_error_code(Error::header_too_large);
                    r.consumed = i;
                    return r;
                }
                if (c == '\n') {
                    if (line_.empty() || line_ == "\r") state_ = State::Done;
                    line_.clear();
                } else {
                    line_ += c;
                }
                break;
            case State::Done:
                break;
        }
    }
    r.consumed = i;
    r.done = state_ == State::Done;
    return r;
}

}  // namespace tdm::net
