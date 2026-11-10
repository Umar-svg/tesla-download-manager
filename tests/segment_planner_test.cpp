#include "multipart/segment_planner.hpp"

#include <gtest/gtest.h>

#include <random>

using namespace tdm::multipart;

namespace {
constexpr std::uint64_t MiB = 1ull << 20;
SegmentPlanner::Options opts(std::uint64_t min_split = MiB) { return {min_split, 64 * 1024}; }
}  // namespace

TEST(SegmentPlanner, FirstAssignmentCoversWholeFile) {
    SegmentPlanner p(32 * MiB, opts());
    auto a = p.acquire();
    ASSERT_TRUE(a);
    EXPECT_EQ(a->offset, 0u);
    EXPECT_EQ(a->end, 32 * MiB);
    EXPECT_TRUE(p.invariant_holds());
}

TEST(SegmentPlanner, BisectsLargestRemaining) {
    SegmentPlanner p(32 * MiB, opts());
    auto a = *p.acquire();
    auto b = *p.acquire();  // splits a at 16 MiB
    EXPECT_EQ(b.offset, 16 * MiB);
    EXPECT_EQ(b.end, 32 * MiB);
    EXPECT_EQ(p.limit(a.id), 16 * MiB);
    auto c = *p.acquire();  // both halves are 16 MiB; one of them is split at 8 MiB
    EXPECT_EQ(c.end - c.offset, 8 * MiB);
    EXPECT_EQ(p.assigned_count(), 3u);
    EXPECT_TRUE(p.invariant_holds());
}

TEST(SegmentPlanner, SplitAccountsForProgressAndAlignment) {
    SegmentPlanner p(10 * MiB, opts());
    auto a = *p.acquire();
    p.commit(a.id, 1'000'001);  // odd progress
    auto b = *p.acquire();
    EXPECT_EQ(b.offset % (64 * 1024), 0u);
    EXPECT_GT(b.offset, 1'000'001u);
    EXPECT_EQ(p.limit(a.id), b.offset - 1'000'001u);
    EXPECT_TRUE(p.invariant_holds());
}

TEST(SegmentPlanner, RespectsMinSplit) {
    SegmentPlanner p(3 * MiB, opts(2 * MiB));
    ASSERT_TRUE(p.acquire());
    EXPECT_FALSE(p.acquire());  // 3 MiB < 2 * 2 MiB
}

TEST(SegmentPlanner, ZeroLengthFileIsComplete) {
    SegmentPlanner p(0, opts());
    EXPECT_TRUE(p.complete());
    EXPECT_FALSE(p.acquire());
}

TEST(SegmentPlanner, CommitPastEndThrows) {
    SegmentPlanner p(2 * MiB, opts());
    auto a = *p.acquire();
    EXPECT_THROW(p.commit(a.id, 2 * MiB + 1), std::logic_error);
}

TEST(SegmentPlanner, ReleasedSegmentIsReusedBeforeSplitting) {
    SegmentPlanner p(16 * MiB, opts());
    auto a = *p.acquire();
    auto b = *p.acquire();
    p.commit(b.id, MiB);
    p.release(b.id);  // connection dropped
    auto c = *p.acquire();
    EXPECT_EQ(c.id, b.id);
    EXPECT_EQ(c.offset, 8 * MiB + MiB);  // resumes where b stopped
    (void)a;
}

TEST(SegmentPlanner, RestoreValidatesCoverage) {
    std::vector<Segment> ok = {{0, 0, 100, 400, true}, {1, 400, 400, 1000, false}};
    auto p = SegmentPlanner::restore(1000, opts(1), ok);
    ASSERT_TRUE(p);
    EXPECT_EQ(p->downloaded(), 100u);
    EXPECT_EQ(p->assigned_count(), 0u);  // nothing is assigned after a restore

    std::vector<Segment> gap = {{0, 0, 0, 400, false}, {1, 500, 500, 1000, false}};
    EXPECT_FALSE(SegmentPlanner::restore(1000, opts(), gap));
    std::vector<Segment> short_ = {{0, 0, 0, 400, false}};
    EXPECT_FALSE(SegmentPlanner::restore(1000, opts(), short_));
}

TEST(SegmentPlanner, ResetForgetsProgress) {
    SegmentPlanner p(8 * MiB, opts());
    auto a = *p.acquire();
    p.acquire();
    p.commit(a.id, MiB);
    p.reset();
    EXPECT_EQ(p.downloaded(), 0u);
    EXPECT_EQ(p.snapshot().size(), 1u);
}

// Simulates 8 connections with random speeds and random drops; the file must
// always end up exactly covered, with every byte committed once.
TEST(SegmentPlanner, RandomisedSimulationCompletesExactly) {
    std::mt19937_64 rng(12345);
    for (int round = 0; round < 50; ++round) {
        const std::uint64_t total = std::uniform_int_distribution<std::uint64_t>(1, 64 * MiB)(rng);
        SegmentPlanner p(total, opts(256 * 1024));
        std::vector<std::optional<SegmentId>> conns(8);
        std::uint64_t committed = 0;
        int guard = 0;
        while (!p.complete() && ++guard < 1'000'000) {
            for (auto& c : conns) {
                if (!c) {
                    if (auto a = p.acquire()) c = a->id;
                    continue;
                }
                if (std::uniform_int_distribution<int>(0, 200)(rng) == 0) {  // drop
                    p.release(*c);
                    c.reset();
                    continue;
                }
                const auto want = std::uniform_int_distribution<std::uint64_t>(1, 512 * 1024)(rng);
                const auto n = std::min(want, p.limit(*c));
                p.commit(*c, n);
                committed += n;
                if (p.limit(*c) == 0) {
                    p.release(*c);
                    c.reset();
                }
            }
            ASSERT_TRUE(p.invariant_holds());
        }
        ASSERT_TRUE(p.complete()) << "round " << round;
        EXPECT_EQ(committed, total);
        EXPECT_EQ(p.downloaded(), total);
    }
}
