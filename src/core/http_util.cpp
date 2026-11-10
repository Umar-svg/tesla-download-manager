#include "core/http_util.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <limits>
#include <vector>

namespace tdm::core {
namespace {

bool is_ws(char c) noexcept { return c == ' ' || c == '\t'; }

char lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string to_lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = lower(c);
    return out;
}

template <class T>
std::optional<T> parse_uint(std::string_view s) noexcept {
    if (s.empty()) return std::nullopt;
    T v{};
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || p != s.data() + s.size()) return std::nullopt;
    return v;
}

bool all_digits(std::string_view s) noexcept {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

constexpr std::array<const char*, 12> kMonths = {"jan", "feb", "mar", "apr", "may", "jun",
                                                 "jul", "aug", "sep", "oct", "nov", "dec"};
constexpr std::array<const char*, 12> kMonthNames = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
constexpr std::array<const char*, 7> kDayNames = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

// Howard Hinnant's days_from_civil / civil_from_days.
constexpr std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) noexcept {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

struct Civil { std::int64_t y; unsigned m, d; };
constexpr Civil civil_from_days(std::int64_t z) noexcept {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    return {y + (m <= 2), m, d};
}

std::string remove_dot_segments(std::string_view path) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    const bool trailing = path.ends_with("/") || path.ends_with("/.") || path.ends_with("/..");
    while (i <= path.size()) {
        std::size_t j = path.find('/', i);
        if (j == std::string_view::npos) j = path.size();
        std::string_view seg = path.substr(i, j - i);
        if (seg == "..") {
            if (!out.empty()) out.pop_back();
        } else if (!seg.empty() && seg != ".") {
            out.push_back(seg);
        }
        i = j + 1;
    }
    std::string r;
    for (auto s : out) {
        r += '/';
        r += s;
    }
    if (r.empty() || (trailing && !out.empty())) r += '/';
    return r;
}

std::string normalise_target(std::string_view t) {
    if (auto h = t.find('#'); h != std::string_view::npos) t = t.substr(0, h);
    std::string_view path = t, query;
    if (auto q = t.find('?'); q != std::string_view::npos) {
        path = t.substr(0, q);
        query = t.substr(q);
    }
    std::string out = path.empty() ? std::string("/") : remove_dot_segments(path);
    out += query;
    std::string enc;
    enc.reserve(out.size());
    for (char c : out) {
        if (c == ' ') enc += "%20";
        else enc += c;
    }
    return enc;
}

std::string latin1_to_utf8(std::string_view s) {
    std::string out;
    for (unsigned char c : s) {
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

}  // namespace

std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && is_ws(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_ws(s.back())) s.remove_suffix(1);
    return s;
}

bool iequals(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return lower(x) == lower(y); });
}

std::optional<ContentRange> parse_content_range(std::string_view value) {
    value = trim(value);
    if (value.size() < 6 || !iequals(value.substr(0, 5), "bytes") || !is_ws(value[5])) return std::nullopt;
    value = trim(value.substr(6));
    const auto slash = value.find('/');
    if (slash == std::string_view::npos) return std::nullopt;
    const auto left = trim(value.substr(0, slash));
    const auto right = trim(value.substr(slash + 1));

    ContentRange cr;
    if (right != "*") {
        cr.complete_length = parse_uint<std::uint64_t>(right);
        if (!cr.complete_length) return std::nullopt;
    }
    if (left == "*") {
        if (!cr.complete_length) return std::nullopt;  // "bytes */*" is invalid
        return cr;
    }
    const auto dash = left.find('-');
    if (dash == std::string_view::npos) return std::nullopt;
    cr.first = parse_uint<std::uint64_t>(left.substr(0, dash));
    cr.last = parse_uint<std::uint64_t>(left.substr(dash + 1));
    if (!cr.first || !cr.last || *cr.first > *cr.last) return std::nullopt;
    if (cr.complete_length && *cr.last >= *cr.complete_length) return std::nullopt;
    return cr;
}

std::optional<std::chrono::system_clock::time_point> parse_http_date(std::string_view value) {
    // Tokenise on space, comma and dash: works for IMF-fixdate, RFC 850 and asctime.
    std::vector<std::string_view> tok;
    std::size_t i = 0;
    value = trim(value);
    while (i < value.size()) {
        while (i < value.size() && (value[i] == ' ' || value[i] == ',' || value[i] == '-')) ++i;
        std::size_t j = i;
        while (j < value.size() && value[j] != ' ' && value[j] != ',' && value[j] != '-') ++j;
        if (j > i) tok.push_back(value.substr(i, j - i));
        i = j;
    }
    int month = -1;
    std::optional<unsigned> hh, mm, ss;
    std::vector<std::string_view> nums;
    for (auto t : tok) {
        if (t.find(':') != std::string_view::npos) {
            if (t.size() != 8 || t[2] != ':' || t[5] != ':') return std::nullopt;
            hh = parse_uint<unsigned>(t.substr(0, 2));
            mm = parse_uint<unsigned>(t.substr(3, 2));
            ss = parse_uint<unsigned>(t.substr(6, 2));
        } else if (all_digits(t)) {
            nums.push_back(t);
        } else if (t.size() == 3) {
            const auto l = to_lower(t);
            for (int m = 0; m < 12; ++m)
                if (l == kMonths[static_cast<std::size_t>(m)]) month = m + 1;
        }
    }
    if (month < 0 || !hh || !mm || !ss || nums.size() != 2) return std::nullopt;
    const auto day = parse_uint<unsigned>(nums[0]);
    auto year = parse_uint<unsigned>(nums[1]);
    if (!day || !year || nums[0].size() > 2) return std::nullopt;
    if (nums[1].size() == 2) *year += (*year < 70) ? 2000u : 1900u;
    else if (nums[1].size() != 4) return std::nullopt;
    if (*day < 1 || *day > 31 || *hh > 23 || *mm > 59 || *ss > 60) return std::nullopt;

    const auto days = days_from_civil(*year, static_cast<unsigned>(month), *day);
    const auto secs = days * 86400 + *hh * 3600 + *mm * 60 + *ss;
    return std::chrono::system_clock::time_point{std::chrono::seconds{secs}};
}

std::string format_http_date(std::chrono::system_clock::time_point tp) {
    const auto secs = std::chrono::floor<std::chrono::seconds>(tp.time_since_epoch()).count();
    std::int64_t days = secs / 86400;
    std::int64_t rem = secs % 86400;
    if (rem < 0) { rem += 86400; --days; }
    const auto c = civil_from_days(days);
    const auto wd = static_cast<std::size_t>(((days % 7) + 11) % 7);  // 1970-01-01 was a Thursday
    char buf[40];
    std::snprintf(buf, sizeof buf, "%s, %02u %s %04lld %02lld:%02lld:%02lld GMT", kDayNames[wd], c.d,
                  kMonthNames[c.m - 1], static_cast<long long>(c.y), static_cast<long long>(rem / 3600),
                  static_cast<long long>((rem / 60) % 60), static_cast<long long>(rem % 60));
    return buf;
}

std::optional<std::chrono::seconds> parse_retry_after(std::string_view value,
                                                      std::chrono::system_clock::time_point now) {
    value = trim(value);
    if (all_digits(value)) {
        auto v = parse_uint<std::uint64_t>(value);
        if (!v) return std::chrono::seconds{std::numeric_limits<std::int32_t>::max()};  // overflow
        return std::chrono::seconds{static_cast<std::int64_t>(
            std::min<std::uint64_t>(*v, std::numeric_limits<std::int32_t>::max()))};
    }
    auto when = parse_http_date(value);
    if (!when) return std::nullopt;
    if (*when <= now) return std::chrono::seconds{0};
    return std::chrono::ceil<std::chrono::seconds>(*when - now);
}

std::string percent_decode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const int h = hex_value(s[i + 1]), l = hex_value(s[i + 2]);
            if (h >= 0 && l >= 0) {
                out += static_cast<char>(h * 16 + l);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

std::optional<std::string> content_disposition_filename(std::string_view value) {
    // Split on ';' outside quoted strings.
    std::vector<std::string_view> parts;
    bool inq = false;
    std::size_t start = 0;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (inq && c == '\\') { ++i; continue; }
        if (c == '"') inq = !inq;
        else if (c == ';' && !inq) {
            parts.push_back(value.substr(start, i - start));
            start = i + 1;
        }
    }
    parts.push_back(value.substr(start));

    std::optional<std::string> plain, extended;
    for (std::size_t p = 1; p < parts.size(); ++p) {  // parts[0] is the disposition type
        const auto eq = parts[p].find('=');
        if (eq == std::string_view::npos) continue;
        const auto name = to_lower(trim(parts[p].substr(0, eq)));
        auto val = trim(parts[p].substr(eq + 1));
        if (name == "filename*") {
            const auto q1 = val.find('\'');
            const auto q2 = q1 == std::string_view::npos ? q1 : val.find('\'', q1 + 1);
            if (q2 == std::string_view::npos) continue;
            const auto charset = to_lower(val.substr(0, q1));
            const auto decoded = percent_decode(val.substr(q2 + 1));
            if (charset == "utf-8") extended = decoded;
            else if (charset == "iso-8859-1") extended = latin1_to_utf8(decoded);
        } else if (name == "filename") {
            std::string out;
            if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
                val = val.substr(1, val.size() - 2);
                for (std::size_t i = 0; i < val.size(); ++i) {
                    if (val[i] == '\\' && i + 1 < val.size()) ++i;
                    out += val[i];
                }
            } else {
                out = std::string(val);
            }
            plain = std::move(out);
        }
    }
    if (extended && !extended->empty()) return extended;
    if (plain && !plain->empty()) return plain;
    return std::nullopt;
}

std::string sanitize_filename(std::string_view name) {
    if (auto p = name.find_last_of("/\\"); p != std::string_view::npos) name = name.substr(p + 1);
    std::string out;
    out.reserve(name.size());
    for (unsigned char c : name) {
        if (c < 0x20 || c == 0x7F || std::string_view("<>:\"|?*").find(static_cast<char>(c)) != std::string_view::npos)
            out += '_';
        else
            out += static_cast<char>(c);
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());

    constexpr std::size_t kMaxBytes = 200;
    if (out.size() > kMaxBytes) {
        std::size_t cut = kMaxBytes;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
        out.resize(cut);
    }
    if (out.empty()) return "download";

    const auto stem = to_lower(std::string_view(out).substr(0, out.find('.')));
    static constexpr std::array<std::string_view, 4> fixed = {"con", "prn", "aux", "nul"};
    bool reserved = std::find(fixed.begin(), fixed.end(), stem) != fixed.end();
    if (stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) && stem[3] >= '1' && stem[3] <= '9')
        reserved = true;
    if (reserved) out.insert(out.begin(), '_');
    return out;
}

bool Url::default_port() const noexcept {
    return (scheme == "http" && port == 80) || (scheme == "https" && port == 443);
}

std::string Url::host_port() const {
    std::string h = host.find(':') != std::string::npos ? "[" + host + "]" : host;
    if (!default_port()) h += ":" + std::to_string(port);
    return h;
}

std::string Url::to_string() const { return scheme + "://" + host_port() + target; }

std::string Url::pool_key() const {
    return scheme + "://" + (host.find(':') != std::string::npos ? "[" + host + "]" : host) + ":" +
           std::to_string(port);
}

std::optional<Url> parse_url(std::string_view s) {
    s = trim(s);
    const auto sep = s.find("://");
    if (sep == std::string_view::npos) return std::nullopt;
    Url u;
    u.scheme = to_lower(s.substr(0, sep));
    if (u.scheme != "http" && u.scheme != "https") return std::nullopt;
    s.remove_prefix(sep + 3);

    const auto auth_end = s.find_first_of("/?#");
    auto authority = s.substr(0, auth_end);
    const auto rest = auth_end == std::string_view::npos ? std::string_view{} : s.substr(auth_end);
    if (auto at = authority.rfind('@'); at != std::string_view::npos) authority.remove_prefix(at + 1);

    std::string_view host, port;
    if (!authority.empty() && authority.front() == '[') {
        const auto rb = authority.find(']');
        if (rb == std::string_view::npos) return std::nullopt;
        host = authority.substr(1, rb - 1);
        const auto after = authority.substr(rb + 1);
        if (!after.empty()) {
            if (after.front() != ':') return std::nullopt;
            port = after.substr(1);
        }
    } else {
        const auto colon = authority.rfind(':');
        host = authority.substr(0, colon);
        if (colon != std::string_view::npos) port = authority.substr(colon + 1);
    }
    if (host.empty()) return std::nullopt;
    u.host = to_lower(host);
    if (port.empty()) {
        u.port = u.scheme == "https" ? 443 : 80;
    } else {
        const auto p = parse_uint<unsigned>(port);
        if (!p || *p == 0 || *p > 65535) return std::nullopt;
        u.port = static_cast<std::uint16_t>(*p);
    }
    u.target = normalise_target(rest);
    return u;
}

std::optional<Url> resolve_location(const Url& base, std::string_view location) {
    location = trim(location);
    if (location.empty()) return std::nullopt;
    const auto colon = location.find("://");
    const auto first_slash = location.find('/');
    if (colon != std::string_view::npos && (first_slash == std::string_view::npos || colon < first_slash))
        return parse_url(location);
    if (location.starts_with("//")) return parse_url(base.scheme + ":" + std::string(location));

    const std::string origin = base.scheme + "://" + base.host_port();
    if (location.front() == '/') return parse_url(origin + std::string(location));

    std::string_view base_path = base.target;
    if (auto q = base_path.find('?'); q != std::string_view::npos) base_path = base_path.substr(0, q);
    if (location.front() == '?') return parse_url(origin + std::string(base_path) + std::string(location));
    const auto dir = base_path.substr(0, base_path.rfind('/') + 1);
    return parse_url(origin + std::string(dir) + std::string(location));
}

std::string filename_from_url(const Url& url) {
    std::string_view path = url.target;
    if (auto q = path.find('?'); q != std::string_view::npos) path = path.substr(0, q);
    return percent_decode(path.substr(path.rfind('/') + 1));
}

}  // namespace tdm::core
