#include "retry/retry_policy.hpp"

#include <gtest/gtest.h>

using namespace tdm::retry;
using namespace std::chrono;

TEST(RetryPolicy, BackoffGrowsAndIsCapped) {
    RetryPolicy p;
    p.jitter = 0;  // deterministic
    std::mt19937_64 rng(1);
    EXPECT_EQ(p.backoff(1, rng), milliseconds{500});
    EXPECT_EQ(p.backoff(2, rng), milliseconds{1000});
    EXPECT_EQ(p.backoff(4, rng), milliseconds{4000});
    EXPECT_EQ(p.backoff(30, rng), milliseconds{60'000});
}

TEST(RetryPolicy, JitterStaysInRange) {
    RetryPolicy p;
    std::mt19937_64 rng(7);
    for (int i = 0; i < 1000; ++i) {
        const auto d = p.backoff(3, rng);  // nominal 2000 ms, jitter 0.5
        EXPECT_GE(d, milliseconds{1000});
        EXPECT_LE(d, milliseconds{2000});
    }
}

TEST(RetryPolicy, HonoursRetryAfter) {
    RetryPolicy p;
    std::mt19937_64 rng(1);
    Failure f{FailureKind::HttpStatus, 503, seconds{3}};
    auto d = p.decide(f, 1, rng);
    EXPECT_EQ(d.action, Action::RetrySameState);
    EXPECT_EQ(d.delay, milliseconds{3000});

    f.retry_after = seconds{0};  // never hammer: at least `base`
    EXPECT_EQ(p.decide(f, 1, rng).delay, milliseconds{500});

    f.retry_after = hours{48};  // absurd hint is capped
    EXPECT_EQ(p.decide(f, 1, rng).delay, milliseconds{3'600'000});
}

TEST(RetryPolicy, Classification) {
    RetryPolicy p;
    std::mt19937_64 rng(1);
    EXPECT_EQ(p.decide({FailureKind::HttpStatus, 404, {}}, 1, rng).action, Action::GiveUp);
    EXPECT_EQ(p.decide({FailureKind::HttpStatus, 429, {}}, 1, rng).action, Action::RetrySameState);
    EXPECT_EQ(p.decide({FailureKind::Network, 0, {}}, 1, rng).action, Action::RetrySameState);
    EXPECT_EQ(p.decide({FailureKind::ValidatorMismatch, 0, {}}, 1, rng).action, Action::RestartFromZero);
    EXPECT_EQ(p.decide({FailureKind::Disk, 0, {}}, 1, rng).action, Action::GiveUp);
    EXPECT_EQ(p.decide({FailureKind::Cancelled, 0, {}}, 1, rng).action, Action::GiveUp);
    EXPECT_EQ(p.decide({FailureKind::Network, 0, {}}, 9, rng).action, Action::GiveUp);  // > max_attempts
}
