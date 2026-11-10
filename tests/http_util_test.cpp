#include "core/http_util.hpp"

#include <gtest/gtest.h>

using namespace tdm::core;
using namespace std::chrono;

TEST(ContentRange, Normal) {
    auto cr = parse_content_range("bytes 100-199/1000000");
    ASSERT_TRUE(cr);
    EXPECT_EQ(*cr->first, 100u);
    EXPECT_EQ(*cr->last, 199u);
    EXPECT_EQ(*cr->complete_length, 1000000u);
}

TEST(ContentRange, UnknownLengthAndUnsatisfied) {
    auto a = parse_content_range("bytes 0-0/*");
    ASSERT_TRUE(a);
    EXPECT_FALSE(a->complete_length);
    auto b = parse_content_range("BYTES */500");
    ASSERT_TRUE(b);
    EXPECT_FALSE(b->first);
    EXPECT_EQ(*b->complete_length, 500u);
}

TEST(ContentRange, Rejects) {
    EXPECT_FALSE(parse_content_range("bytes 5-4/10"));
    EXPECT_FALSE(parse_content_range("bytes 0-10/10"));  // last must be < length
    EXPECT_FALSE(parse_content_range("bytes */*"));
    EXPECT_FALSE(parse_content_range("items 0-1/2"));
    EXPECT_FALSE(parse_content_range("bytes 0-1"));
    EXPECT_FALSE(parse_content_range("bytes -1-2/3"));
}

TEST(HttpDate, AllThreeFormats) {
    const auto expect = sys_seconds{seconds{784111777}};  // 1994-11-06 08:49:37 UTC
    EXPECT_EQ(parse_http_date("Sun, 06 Nov 1994 08:49:37 GMT"), expect);
    EXPECT_EQ(parse_http_date("Sunday, 06-Nov-94 08:49:37 GMT"), expect);
    EXPECT_EQ(parse_http_date("Sun Nov  6 08:49:37 1994"), expect);
    EXPECT_FALSE(parse_http_date("yesterday"));
    EXPECT_FALSE(parse_http_date("Sun, 06 Foo 1994 08:49:37 GMT"));
}

TEST(HttpDate, FormatRoundTrip) {
    // Value used by the probe server for version 1: 1_700_000_000 + 3600. The
    // expected string is what Python's email.utils.formatdate produced on the wire.
    const auto tp = sys_seconds{seconds{1'700'003'600}};
    EXPECT_EQ(format_http_date(tp), "Tue, 14 Nov 2023 23:13:20 GMT");
    EXPECT_EQ(parse_http_date(format_http_date(tp)), tp);
    EXPECT_EQ(format_http_date(sys_seconds{}), "Thu, 01 Jan 1970 00:00:00 GMT");
}

TEST(RetryAfter, SecondsAndDate) {
    const auto now = sys_seconds{seconds{784111777}};
    EXPECT_EQ(parse_retry_after("120", now), seconds{120});
    EXPECT_EQ(parse_retry_after(" 0 ", now), seconds{0});
    EXPECT_EQ(parse_retry_after("Sun, 06 Nov 1994 08:50:07 GMT", now), seconds{30});
    EXPECT_EQ(parse_retry_after("Sun, 06 Nov 1994 08:00:00 GMT", now), seconds{0});  // past
    EXPECT_FALSE(parse_retry_after("soon", now));
    EXPECT_FALSE(parse_retry_after("-5", now));
    EXPECT_EQ(parse_retry_after("99999999999999999999999", now), seconds{2147483647});
}

TEST(ContentDisposition, Variants) {
    EXPECT_EQ(content_disposition_filename(R"(attachment; filename="a.bin")"), "a.bin");
    EXPECT_EQ(content_disposition_filename("attachment; filename=plain.zip"), "plain.zip");
    const std::string quoted = "attachment; filename=\"we\\\"ird;name.txt\"";  // filename="we\"ird;name.txt"
    EXPECT_EQ(content_disposition_filename(quoted), "we\"ird;name.txt");
    EXPECT_EQ(content_disposition_filename(
                  "attachment; filename=\"fallback.txt\"; filename*=UTF-8''%E2%82%AC%20rates.txt"),
              "\xE2\x82\xAC rates.txt");
    EXPECT_EQ(content_disposition_filename("attachment; FILENAME*=iso-8859-1''caf%E9.txt"), "caf\xC3\xA9.txt");
    EXPECT_FALSE(content_disposition_filename("inline"));
}

TEST(SanitizeFilename, WindowsRules) {
    EXPECT_EQ(sanitize_filename("..\\..\\evil.exe"), "evil.exe");
    EXPECT_EQ(sanitize_filename("/etc/passwd"), "passwd");
    EXPECT_EQ(sanitize_filename("a<b>c:d\"e|f?g*h.txt"), "a_b_c_d_e_f_g_h.txt");
    EXPECT_EQ(sanitize_filename("CON"), "_CON");
    EXPECT_EQ(sanitize_filename("com1.txt"), "_com1.txt");
    EXPECT_EQ(sanitize_filename("console.txt"), "console.txt");
    EXPECT_EQ(sanitize_filename("name. . "), "name");
    EXPECT_EQ(sanitize_filename(".."), "download");
    EXPECT_EQ(sanitize_filename(""), "download");
    EXPECT_LE(sanitize_filename(std::string(500, 'x')).size(), 200u);
}

TEST(Url, Parse) {
    auto u = parse_url("HTTPS://User:pw@Example.COM/a/./b/../c.zip?x=1#frag");
    ASSERT_TRUE(u);
    EXPECT_EQ(u->scheme, "https");
    EXPECT_EQ(u->host, "example.com");
    EXPECT_EQ(u->port, 443);
    EXPECT_EQ(u->target, "/a/c.zip?x=1");
    EXPECT_EQ(u->to_string(), "https://example.com/a/c.zip?x=1");

    auto v = parse_url("http://127.0.0.1:8765");
    ASSERT_TRUE(v);
    EXPECT_EQ(v->target, "/");
    EXPECT_EQ(v->pool_key(), "http://127.0.0.1:8765");

    auto w = parse_url("http://[::1]:8080/x y");
    ASSERT_TRUE(w);
    EXPECT_EQ(w->host, "::1");
    EXPECT_EQ(w->target, "/x%20y");
    EXPECT_EQ(w->to_string(), "http://[::1]:8080/x%20y");

    EXPECT_FALSE(parse_url("ftp://x/y"));
    EXPECT_FALSE(parse_url("http://:80/"));
    EXPECT_FALSE(parse_url("http://h:99999/"));
    EXPECT_FALSE(parse_url("not a url"));
}

TEST(Url, ResolveLocation) {
    auto base = *parse_url("http://h:81/dir/sub/file?q=1");
    EXPECT_EQ(resolve_location(base, "https://other/x")->to_string(), "https://other/x");
    EXPECT_EQ(resolve_location(base, "//cdn.example/y")->to_string(), "http://cdn.example/y");
    EXPECT_EQ(resolve_location(base, "/root")->to_string(), "http://h:81/root");
    EXPECT_EQ(resolve_location(base, "next.bin")->to_string(), "http://h:81/dir/sub/next.bin");
    EXPECT_EQ(resolve_location(base, "../up.bin")->to_string(), "http://h:81/dir/up.bin");
    EXPECT_EQ(resolve_location(base, "?q=2")->to_string(), "http://h:81/dir/sub/file?q=2");
    // The probe server's redirect chain uses absolute paths with a query.
    EXPECT_EQ(resolve_location(base, "/redirect/2/f/a.bin?size=10")->target, "/redirect/2/f/a.bin?size=10");
}

TEST(Url, FilenameFromUrl) {
    EXPECT_EQ(filename_from_url(*parse_url("http://h/a/b/My%20File.iso?x=1")), "My File.iso");
    EXPECT_EQ(filename_from_url(*parse_url("http://h/")), "");
    EXPECT_EQ(percent_decode("100%"), "100%");
    EXPECT_EQ(percent_decode("%zz%41"), "%zzA");
}

TEST(ContentType, MediaTypeAndWebPageDetection) {
    EXPECT_EQ(media_type("Text/HTML; charset=utf-8"), "text/html");
    EXPECT_EQ(media_type("  application/octet-stream "), "application/octet-stream");
    EXPECT_TRUE(is_web_page_type("text/html"));
    EXPECT_TRUE(is_web_page_type("text/html; charset=UTF-8"));
    EXPECT_TRUE(is_web_page_type("application/xhtml+xml"));
    EXPECT_FALSE(is_web_page_type("video/mp4"));
    EXPECT_FALSE(is_web_page_type("application/octet-stream"));
    EXPECT_FALSE(is_web_page_type("text/plain"));
    EXPECT_FALSE(is_web_page_type(""));
}
