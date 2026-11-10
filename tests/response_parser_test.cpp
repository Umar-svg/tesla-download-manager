#include "net/http_client.hpp"
#include "net/response_parser.hpp"

#include <gtest/gtest.h>

using namespace tdm::net;

namespace {
std::span<const std::byte> bytes(std::string_view s) { return std::as_bytes(std::span<const char>(s)); }
}  // namespace

TEST(ResponseHead, ParsesStatusHeadersAndFraming) {
    const std::string raw =
        "HTTP/1.1 206 Partial Content\r\nContent-Length: 100\r\nETag: \"x\"\r\n"
        "Content-Range: bytes 0-99/1000\r\n\r\nBODY";
    ParsedHead h;
    std::size_t used = 0;
    std::error_code ec;
    ASSERT_EQ(parse_response_head(raw, h, used, ec), ParseStatus::Done);
    EXPECT_EQ(h.status, 206);
    EXPECT_EQ(*h.content_length, 100u);
    EXPECT_TRUE(h.keep_alive);
    EXPECT_EQ(raw.substr(used), "BODY");
    EXPECT_EQ(h.headers.size(), 3u);
    EXPECT_EQ(body_kind(h, "GET"), BodyKind::Length);
    EXPECT_EQ(body_kind(h, "HEAD"), BodyKind::None);
}

TEST(ResponseHead, IncrementalAndLimits) {
    ParsedHead h;
    std::size_t used = 0;
    std::error_code ec;
    EXPECT_EQ(parse_response_head("HTTP/1.1 200 OK\r\nA: b\r\n", h, used, ec), ParseStatus::NeedMore);
    EXPECT_EQ(parse_response_head(std::string(kMaxHeadBytes + 1, 'x'), h, used, ec), ParseStatus::Error);
    EXPECT_EQ(ec, make_error_code(Error::header_too_large));
}

TEST(ResponseHead, ConnectionSemantics) {
    ParsedHead h;
    std::size_t used = 0;
    std::error_code ec;
    ASSERT_EQ(parse_response_head("HTTP/1.1 200 OK\r\nConnection: Close\r\n\r\n", h, used, ec), ParseStatus::Done);
    EXPECT_FALSE(h.keep_alive);
    EXPECT_EQ(body_kind(h, "GET"), BodyKind::UntilClose);  // no length, no chunking
    ASSERT_EQ(parse_response_head("HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n", h, used, ec), ParseStatus::Done);
    EXPECT_FALSE(h.keep_alive);
    EXPECT_EQ(body_kind(h, "GET"), BodyKind::None);
    ASSERT_EQ(parse_response_head("HTTP/1.0 200 OK\r\nConnection: keep-alive\r\n\r\n", h, used, ec),
              ParseStatus::Done);
    EXPECT_TRUE(h.keep_alive);
    ASSERT_EQ(parse_response_head("HTTP/1.1 304 Not Modified\r\nContent-Length: 50\r\n\r\n", h, used, ec),
              ParseStatus::Done);
    EXPECT_EQ(body_kind(h, "GET"), BodyKind::None);
}

TEST(ResponseHead, TransferEncodingOverridesLength) {
    ParsedHead h;
    std::size_t used = 0;
    std::error_code ec;
    ASSERT_EQ(parse_response_head(
                  "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nTransfer-Encoding: gzip, chunked\r\n\r\n", h, used, ec),
              ParseStatus::Done);
    EXPECT_TRUE(h.chunked);
    EXPECT_FALSE(h.content_length);
    ASSERT_EQ(parse_response_head("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n", h, used, ec),
              ParseStatus::Done);
    EXPECT_EQ(body_kind(h, "GET"), BodyKind::UntilClose);
    EXPECT_FALSE(h.keep_alive);
}

TEST(ResponseHead, RejectsMalformed) {
    ParsedHead h;
    std::size_t used = 0;
    for (std::string bad : {"HTTP/2 200 OK\r\n\r\n", "HTTP/1.1 20 OK\r\n\r\n", "HTTP/1.1 200OK\r\n\r\n",
                            "HTTP/1.1 200 OK\r\nNoColon\r\n\r\n", "HTTP/1.1 200 OK\r\n folded: x\r\n\r\n",
                            "HTTP/1.1 200 OK\r\nBad Name: x\r\n\r\n",
                            "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n",
                            "HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n"}) {
        std::error_code ec;
        EXPECT_EQ(parse_response_head(bad, h, used, ec), ParseStatus::Error) << bad;
        EXPECT_EQ(ec, make_error_code(Error::bad_response)) << bad;
    }
    std::error_code ec;
    EXPECT_EQ(parse_response_head("HTTP/1.1 200 OK\r\nContent-Length: 7, 7\r\n\r\n", h, used, ec),
              ParseStatus::Done);
    EXPECT_EQ(*h.content_length, 7u);
}

TEST(Chunked, DecodesByteByByteWithExtensionsAndTrailers) {
    const std::string wire = "4;ext=1\r\nWiki\r\n5\r\npedia\r\nE\r\n in\r\n\r\nchunks.\r\n0\r\nX-Trailer: 1\r\n\r\nNEXT";
    ChunkedDecoder d;
    std::string out;
    std::size_t pos = 0;
    while (!d.done()) {
        ASSERT_LT(pos, wire.size());
        auto r = d.feed(bytes(std::string_view(wire).substr(pos, 1)), [&](std::span<const std::byte> s) {
            out.append(reinterpret_cast<const char*>(s.data()), s.size());
            return s.size();
        });
        ASSERT_FALSE(r.ec);
        pos += r.consumed;
    }
    EXPECT_EQ(out, "Wikipedia in\r\n\r\nchunks.");
    EXPECT_EQ(wire.substr(pos), "NEXT");
}

TEST(Chunked, SinkCanStopAndBadFramingFails) {
    ChunkedDecoder d;
    auto r = d.feed(bytes("A\r\n0123456789\r\n0\r\n\r\n"), [](std::span<const std::byte> s) {
        return std::min<std::size_t>(s.size(), 3);
    });
    EXPECT_TRUE(r.stopped);
    EXPECT_EQ(r.consumed, 6u);  // "A\r\n" + 3 data bytes

    ChunkedDecoder bad;
    auto r2 = bad.feed(bytes("zz\r\n"), [](auto s) { return s.size(); });
    EXPECT_EQ(r2.ec, make_error_code(Error::bad_response));
    ChunkedDecoder bad2;
    auto r3 = bad2.feed(bytes("2\r\nabX"), [](auto s) { return s.size(); });
    EXPECT_EQ(r3.ec, make_error_code(Error::bad_response));
}

TEST(Request, SerializeAddsOwnedHeadersAndBlocksInjection) {
    Request r;
    r.url = *tdm::core::parse_url("http://example.com:8080/a?b=1");
    r.range_from = 1024;
    r.if_range = "\"etag\"";
    r.headers = {{"Referer", "http://x/"}, {"X-Evil", "a\r\nInjected: 1"}, {"Range", "bytes=0-1"}};
    const auto s = HttpClient::serialize(r, r.url, "TeslaDM/test");
    EXPECT_TRUE(s.starts_with("GET /a?b=1 HTTP/1.1\r\nHost: example.com:8080\r\nUser-Agent: TeslaDM/test\r\n"));
    EXPECT_NE(s.find("Accept-Encoding: identity\r\n"), std::string::npos);
    EXPECT_NE(s.find("Range: bytes=1024-\r\n"), std::string::npos);
    EXPECT_EQ(s.find("bytes=0-1"), std::string::npos);
    EXPECT_NE(s.find("If-Range: \"etag\"\r\n"), std::string::npos);
    EXPECT_NE(s.find("Referer: http://x/\r\n"), std::string::npos);
    EXPECT_EQ(s.find("Injected"), std::string::npos);
    EXPECT_TRUE(s.ends_with("\r\n\r\n"));
}
