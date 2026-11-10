#include "queue/queue_selector.hpp"
#include "scheduler/scheduler.hpp"
#include "scheduler/token_bucket.hpp"

#include <gtest/gtest.h>

#include <thread>

using namespace tdm;
using namespace std::chrono_literals;

TEST(TokenBucket, UnlimitedNeverWaits) {
    scheduler::TokenBucket b;
    b.consume(100u << 20);
    EXPECT_EQ(b.wait_time().count(), 0);
}

TEST(TokenBucket, DebtTranslatesIntoWaitTime) {
    scheduler::TokenBucket b(1'000'000);  // 1 MB/s, starts empty
    b.consume(500'000);
    const auto w = b.wait_time();
    EXPECT_GT(w, 450ms);
    EXPECT_LE(w, 500ms);
    std::this_thread::sleep_for(w);
    EXPECT_LT(b.wait_time(), 5ms);
}

TEST(TokenBucket, BurstCapsSavedTokens) {
    scheduler::TokenBucket b(1'000'000, 100'000);
    std::this_thread::sleep_for(300ms);  // would refill 300 KB, capped at 100 KB
    b.consume(300'000);
    EXPECT_GT(b.wait_time(), 180ms);  // 200 KB debt at 1 MB/s
}

TEST(TokenBucket, RateChangesApplyImmediately) {
    scheduler::TokenBucket b(1'000'000);
    b.consume(1'000'000);
    EXPECT_GT(b.wait_time(), 900ms);
    b.set_rate(10'000'000);
    EXPECT_LT(b.wait_time(), 110ms);
    b.set_rate(0);
    EXPECT_EQ(b.wait_time().count(), 0);
}

TEST(Schedule, DaytimeWindowAndWeekdays) {
    scheduler::ScheduleRule r;
    r.start_minute = 9 * 60;
    r.stop_minute = 17 * 60;
    r.weekdays_mask = 0b0111110;  // Monday..Friday
    EXPECT_TRUE(schedule_allows(r, {1, 9 * 60}));
    EXPECT_TRUE(schedule_allows(r, {5, 16 * 60 + 59}));
    EXPECT_FALSE(schedule_allows(r, {1, 17 * 60}));  // stop is exclusive
    EXPECT_FALSE(schedule_allows(r, {1, 8 * 60 + 59}));
    EXPECT_FALSE(schedule_allows(r, {0, 12 * 60}));  // Sunday
    EXPECT_FALSE(schedule_allows(r, {6, 12 * 60}));  // Saturday
}

TEST(Schedule, OvernightWindowBelongsToTheDayItOpens) {
    scheduler::ScheduleRule r;
    r.start_minute = 23 * 60;
    r.stop_minute = 6 * 60;
    r.weekdays_mask = 1 << 5;  // Friday night only
    EXPECT_TRUE(schedule_allows(r, {5, 23 * 60 + 30}));  // Friday 23:30
    EXPECT_TRUE(schedule_allows(r, {6, 5 * 60}));        // Saturday 05:00, still Friday's window
    EXPECT_FALSE(schedule_allows(r, {6, 23 * 60 + 30}));  // Saturday night
    EXPECT_FALSE(schedule_allows(r, {5, 5 * 60}));        // Friday 05:00 belongs to Thursday
    EXPECT_FALSE(schedule_allows(r, {6, 12 * 60}));
}

TEST(Schedule, DisabledOrOpenEndedRules) {
    scheduler::ScheduleRule r;
    r.enabled = false;
    r.start_minute = 1;
    r.stop_minute = 2;
    EXPECT_TRUE(schedule_allows(r, {3, 600}));
    scheduler::ScheduleRule start_only;
    start_only.start_minute = 20 * 60;
    EXPECT_FALSE(schedule_allows(start_only, {3, 19 * 60}));
    EXPECT_TRUE(schedule_allows(start_only, {3, 23 * 60 + 59}));
}

TEST(Schedule, JsonRoundTripAndValidation) {
    scheduler::ScheduleRule r;
    r.start_minute = 60;
    r.stop_minute = 120;
    r.weekdays_mask = 0x41;
    auto back = scheduler::schedule_from_json(scheduler::to_json(r));
    ASSERT_TRUE(back);
    EXPECT_EQ(back->start_minute, 60);
    EXPECT_EQ(back->stop_minute, 120);
    EXPECT_EQ(back->weekdays_mask, 0x41);
    EXPECT_FALSE(scheduler::schedule_from_json("null"));
    EXPECT_FALSE(scheduler::schedule_from_json("{\"start\": 5000}"));
    EXPECT_FALSE(scheduler::schedule_from_json("not json"));
}

TEST(QueueSelector, PriorityThenFifoWithinLimits) {
    std::vector<queue::Candidate> q = {{5, 1, 0}, {3, 1, 0}, {9, 1, 10}, {4, 2, 0}, {7, 2, 0}};
    std::map<std::int64_t, queue::QueueState> qs = {{1, {2, true}}, {2, {1, true}}};
    const auto picked = queue::select_to_start(q, qs, {}, 0, 8);
    EXPECT_EQ(picked, (std::vector<std::int64_t>{9, 3, 4}));  // q1: prio 10 then lowest id; q2: one slot
}

TEST(QueueSelector, RespectsRunningCountsGlobalCapAndClosedQueues) {
    std::vector<queue::Candidate> q = {{1, 1, 0}, {2, 1, 0}, {3, 2, 0}, {4, 3, 0}};
    std::map<std::int64_t, queue::QueueState> qs = {{1, {2, true}}, {2, {5, false}}, {3, {5, true}}};
    EXPECT_EQ(queue::select_to_start(q, qs, {{1, 1}}, 1, 8), (std::vector<std::int64_t>{1, 4}));
    EXPECT_EQ(queue::select_to_start(q, qs, {}, 7, 8), (std::vector<std::int64_t>{1}));  // global cap
    EXPECT_TRUE(queue::select_to_start(q, qs, {}, 8, 8).empty());
}
