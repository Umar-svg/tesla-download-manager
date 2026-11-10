// Makes sure interface-only module headers stay compilable until their phase lands.
#include "core/version.hpp"
#include "engine/download_engine.hpp"
#include "fs/file_writer.hpp"
#include "net/http_types.hpp"
#include "scheduler/scheduler.hpp"

#include <gtest/gtest.h>

TEST(Headers, Compile) {
    tdm::net::ResponseHead h;
    h.headers.push_back({"Content-Length", "10"});
    ASSERT_NE(h.find("content-length"), nullptr);
    EXPECT_EQ(*h.find("CONTENT-LENGTH"), "10");
    const std::string ua = tdm::kUserAgent;
    EXPECT_TRUE(ua.starts_with("Mozilla/5.0 (Windows NT 10.0; Win64; x64)")) << ua;  // browser-compatible
    EXPECT_TRUE(ua.ends_with(std::string(" TeslaDM/") + tdm::kVersion)) << ua;      // still identifies us
    EXPECT_EQ(std::string(tdm::kProductToken), std::string("TeslaDM/") + tdm::kVersion);
}
