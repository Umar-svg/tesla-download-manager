#include "ui/format.hpp"

#include <gtest/gtest.h>

using namespace tdm::ui;

TEST(UiFormat, Bytes) {
    EXPECT_EQ(formatBytes(0), "0 B");
    EXPECT_EQ(formatBytes(999), "999 B");
    EXPECT_EQ(formatBytes(1500), "1.50 KB");
    EXPECT_EQ(formatBytes(25'300'000), "25.3 MB");
    EXPECT_EQ(formatBytes(1'073'741'824), "1.07 GB");
}

TEST(UiFormat, SpeedAndEta) {
    EXPECT_EQ(formatSpeed(0), "");
    EXPECT_EQ(formatSpeed(2'400'000), "2.40 MB/s");
    EXPECT_EQ(formatEta(100'000'000, 40'000'000, 1'000'000), "1:00");
    EXPECT_EQ(formatEta(10'000'000'000ull, 0, 1'000'000), "2:46:40");
    EXPECT_EQ(formatEta(std::nullopt, 0, 1'000'000), "");
    EXPECT_EQ(formatEta(100, 100, 5), "");
}

TEST(UiFormat, ParseRate) {
    EXPECT_EQ(parseRate(""), 0u);
    EXPECT_EQ(parseRate("300"), 300u);
    EXPECT_EQ(parseRate("500K"), 500'000u);
    EXPECT_EQ(parseRate("1.5 MB/s"), 1'500'000u);
    EXPECT_EQ(parseRate("2m"), 2'000'000u);
    EXPECT_FALSE(parseRate("fast"));
    EXPECT_FALSE(parseRate("-1"));
}

TEST(UiFormat, UrlAndShaValidation) {
    EXPECT_TRUE(looksLikeUrl("https://example.com/a.zip"));
    EXPECT_TRUE(looksLikeUrl(" http://127.0.0.1:8080/x "));
    EXPECT_FALSE(looksLikeUrl("ftp://example.com/a"));
    EXPECT_FALSE(looksLikeUrl("example.com/a"));
    EXPECT_FALSE(looksLikeUrl("https://"));
    EXPECT_TRUE(isSha256Hex(QString(64, 'a')));
    EXPECT_FALSE(isSha256Hex(QString(63, 'a')));
    EXPECT_FALSE(isSha256Hex(QString(64, 'g')));
}

TEST(UiFormat, Utf8RoundTrip) {
    const std::string s = "caf\xC3\xA9 \xE2\x82\xAC.zip";
    EXPECT_EQ(toUtf8(fromUtf8(s)), s);
}
