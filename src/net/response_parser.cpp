#include "net/response_parser.hpp"

#include <charconv>

#include "core/http_util.hpp"
#include "net/errors.hpp"

namespace tdm::net {
namespace {

class ErrorCategory : public std::error_category {
public:
    const char* name() const noexcept override { return "tdm.net"; }
    std::string message(int ev) const override {
        switch (static_cast<Error>(ev)) {
            case Error::timeout: return "timed out";
            case Error::too_many_redirects: return "too many redirects";
            case Error::bad_response: return "malformed HTTP response";
            case Error::header_too_large: return "response header too large";
            case Error::cancelled: return "cancelled";
            case Error::connection_closed: return "connection closed before the response was complete";
            case Error::aborted_by_handler: return "aborted by response handler";
        }
        return "unknown network error";
    }
};

bool has_token(std::string_view list, std::string_view token) {
    while (!list.empty()) {
        const auto comma = list.find(',');
        if (core::iequals(core::trim(list.substr(0, comma)), token)) return true;
        if (comma == std::string_view::npos) break;
        list.remove_prefix(comma + 1);
    }
    return false;
}

std::string_view last_token(std::string_view list) {
    const auto comma = list.rfind(',');
    return core::trim(comma == std::string_view::npos ? list : list.substr(comma + 1));
}

bool is_tchar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
}

}  // namespace

const std::error_category& error_category() noexcept {
    static ErrorCategory cat;
    return cat;
}

ParseStatus parse_response_head(std::string_view buf, ParsedHead& out, std::size_t& consumed,
                                std::error_code& ec) {
    const auto end = buf.find("\r\n\r\n");
    if (end == std::string_view::npos) {
        if (buf.size() > kMaxHeadBytes) {
            ec = Error::header_too_large;
            return ParseStatus::Error;
        }
        return ParseStatus::NeedMore;
    }
    if (end + 4 > kMaxHeadBytes) {
        ec = Error::header_too_large;
        return ParseStatus::Error;
    }
    out = ParsedHead{};
    std::string_view head = buf.substr(0, end);

    // Status line: HTTP/1.x SP 3DIGIT [SP reason]
    const auto eol = head.find("\r\n");
    const auto status_line = head.substr(0, eol);
    if (status_line.size() < 12 || !status_line.starts_with("HTTP/1.") || status_line[8] != ' ' ||
        status_line[7] < '0' || status_line[7] > '9') {
        ec = Error::bad_response;
        return ParseStatus::Error;
    }
    out.minor_version = status_line[7] - '0';
    int status = 0;
    auto [p, e] = std::from_chars(status_line.data() + 9, status_line.data() + 12, status);
    if (e != std::errc{} || p != status_line.data() + 12 || status < 100 || status > 999 ||
        (status_line.size() > 12 && status_line[12] != ' ')) {
        ec = Error::bad_response;
        return ParseStatus::Error;
    }
    out.status = status;

    std::string_view rest = eol == std::string_view::npos ? std::string_view{} : head.substr(eol + 2);
    std::optional<std::string_view> connection, transfer_encoding;
    while (!rest.empty()) {
        const auto nl = rest.find("\r\n");
        const auto line = rest.substr(0, nl);
        rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 2);
        if (line.empty()) continue;
        if (line.front() == ' ' || line.front() == '\t') {  // obs-fold is not allowed in responses we accept
            ec = Error::bad_response;
            return ParseStatus::Error;
        }
        const auto colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) {
            ec = Error::bad_response;
            return ParseStatus::Error;
        }
        const auto name = line.substr(0, colon);
        for (char c : name)
            if (!is_tchar(c)) {
                ec = Error::bad_response;
                return ParseStatus::Error;
            }
        const auto value = core::trim(line.substr(colon + 1));
        out.headers.push_back({std::string(name), std::string(value)});

        if (core::iequals(name, "Content-Length")) {
            // "10" or "10, 10"; all values must agree (RFC 9110 §8.6).
            std::string_view list = value;
            while (true) {
                const auto comma = list.find(',');
                const auto item = core::trim(list.substr(0, comma));
                std::uint64_t v = 0;
                auto [pp, ee] = std::from_chars(item.data(), item.data() + item.size(), v);
                if (item.empty() || ee != std::errc{} || pp != item.data() + item.size() ||
                    (out.content_length && *out.content_length != v)) {
                    ec = Error::bad_response;
                    return ParseStatus::Error;
                }
                out.content_length = v;
                if (comma == std::string_view::npos) break;
                list.remove_prefix(comma + 1);
            }
        } else if (core::iequals(name, "Transfer-Encoding")) {
            transfer_encoding = value;
        } else if (core::iequals(name, "Connection")) {
            connection = value;
        }
    }

    if (transfer_encoding) {
        out.content_length.reset();  // TE overrides Content-Length (RFC 9112 §6.3)
        if (core::iequals(last_token(*transfer_encoding), "chunked")) out.chunked = true;
        else out.other_transfer_coding = true;
    }
    if (out.minor_version >= 1) out.keep_alive = !(connection && has_token(*connection, "close"));
    else out.keep_alive = connection && has_token(*connection, "keep-alive");
    if (out.other_transfer_coding) out.keep_alive = false;

    consumed = end + 4;
    return ParseStatus::Done;
}

BodyKind body_kind(const ParsedHead& h, std::string_view method) {
    if (method == "HEAD" || (h.status >= 100 && h.status < 200) || h.status == 204 || h.status == 304)
        return BodyKind::None;
    if (h.chunked) return BodyKind::Chunked;
    if (h.other_transfer_coding) return BodyKind::UntilClose;
    if (h.content_length) return *h.content_length == 0 ? BodyKind::None : BodyKind::Length;
    return BodyKind::UntilClose;
}

bool ChunkedDecoder::finish_size_line(std::error_code& ec) {
    std::string_view l = line_;
    if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
    if (auto semi = l.find(';'); semi != std::string_view::npos) l = l.substr(0, semi);  // chunk-ext
    l = core::trim(l);
    std::uint64_t size = 0;
    auto [p, e] = std::from_chars(l.data(), l.data() + l.size(), size, 16);
    if (l.empty() || l.size() > 16 || e != std::errc{} || p != l.data() + l.size()) {
        ec = Error::bad_response;
        return false;
    }
    line_.clear();
    if (size == 0) {
        state_ = State::Trailer;
    } else {
        remaining_ = size;
        state_ = State::Data;
    }
    return true;
}

}  // namespace tdm::net
