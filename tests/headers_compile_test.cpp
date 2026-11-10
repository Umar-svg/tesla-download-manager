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
    EXPECT_STREQ(tdm::kUserAgent, "TeslaDM/0.1.0");
}
