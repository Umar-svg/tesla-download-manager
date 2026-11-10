#include "scheduler/token_bucket.hpp"

#include <algorithm>

namespace tdm::scheduler {

void TokenBucket::set_rate(std::uint64_t rate_bps, std::uint64_t burst_bytes) {
    std::lock_guard lk(m_);
    refill(clock::now());
    rate_ = rate_bps;
    burst_ = burst_bytes ? static_cast<double>(burst_bytes)
                         : std::max(static_cast<double>(rate_bps) / 8.0, 64.0 * 1024.0);
    tokens_ = std::min(tokens_, burst_);  // a lower limit takes effect immediately
}

std::uint64_t TokenBucket::rate() const {
    std::lock_guard lk(m_);
    return rate_;
}

void TokenBucket::refill(clock::time_point now) {
    if (rate_ > 0) {
        const double dt = std::chrono::duration<double>(now - last_).count();
        tokens_ = std::min(burst_, tokens_ + dt * static_cast<double>(rate_));
    }
    last_ = now;
}

void TokenBucket::consume(std::uint64_t bytes) {
    std::lock_guard lk(m_);
    if (rate_ == 0) return;
    refill(clock::now());
    tokens_ -= static_cast<double>(bytes);
}

std::chrono::nanoseconds TokenBucket::reserve(std::uint64_t bytes) {
    std::lock_guard lk(m_);
    if (rate_ == 0) return std::chrono::nanoseconds{0};
    refill(clock::now());
    tokens_ -= static_cast<double>(bytes);
    if (tokens_ >= 0) return std::chrono::nanoseconds{0};
    return std::chrono::nanoseconds{static_cast<long long>(-tokens_ / static_cast<double>(rate_) * 1e9)};
}

void TokenBucket::refund(std::uint64_t bytes) {
    std::lock_guard lk(m_);
    if (rate_ == 0) return;
    refill(clock::now());
    tokens_ = std::min(burst_, tokens_ + static_cast<double>(bytes));
}

std::chrono::nanoseconds TokenBucket::wait_time() {
    std::lock_guard lk(m_);
    if (rate_ == 0) return std::chrono::nanoseconds{0};
    refill(clock::now());
    if (tokens_ >= 0) return std::chrono::nanoseconds{0};
    return std::chrono::nanoseconds{static_cast<long long>(-tokens_ / static_cast<double>(rate_) * 1e9)};
}

}  // namespace tdm::scheduler
