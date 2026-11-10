#pragma once
// Thread-safe token bucket for bandwidth limits (global and per download).
//
// Preferred use (smooth pacing): reserve(n) *before* a read of at most n
// bytes and sleep for the returned time; refund() what was not used. Because
// reservations queue up one after another, concurrent readers are staggered
// instead of all firing when a shared debt clears.
// consume()/wait_time() charge after the fact (used for unreserved bytes).

#include <chrono>
#include <cstdint>
#include <mutex>

namespace tdm::scheduler {

class TokenBucket {
public:
    using clock = std::chrono::steady_clock;

    explicit TokenBucket(std::uint64_t rate_bps = 0, std::uint64_t burst_bytes = 0) { set_rate(rate_bps, burst_bytes); }

    // rate 0 = unlimited. burst 0 = max(rate / 8, 64 KiB).
    void set_rate(std::uint64_t rate_bps, std::uint64_t burst_bytes = 0);
    std::uint64_t rate() const;
    void consume(std::uint64_t bytes);
    std::chrono::nanoseconds reserve(std::uint64_t bytes);  // charge now; returns how long to wait
    void refund(std::uint64_t bytes);                       // give back an unused reservation
    std::chrono::nanoseconds wait_time();  // 0 when tokens are available or unlimited

private:
    void refill(clock::time_point now);  // requires m_

    mutable std::mutex m_;
    std::uint64_t rate_ = 0;
    double burst_ = 0;
    double tokens_ = 0;
    clock::time_point last_ = clock::now();
};

}  // namespace tdm::scheduler
