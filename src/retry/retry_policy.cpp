#include "retry/retry_policy.hpp"

#include <algorithm>
#include <cmath>

namespace tdm::retry {

bool RetryPolicy::retryable_status(int status) noexcept {
    switch (status) {
        case 408: case 425: case 429:
        case 500: case 502: case 503: case 504:
            return true;
        default:
            return false;
    }
}

std::chrono::milliseconds RetryPolicy::backoff(int attempt, std::mt19937_64& rng) const {
    const int exp = std::clamp(attempt - 1, 0, 30);
    const double raw = static_cast<double>(base.count()) * std::ldexp(1.0, exp);
    const double capped = std::min(raw, static_cast<double>(cap.count()));
    const double j = std::clamp(jitter, 0.0, 1.0);
    std::uniform_real_distribution<double> dist(1.0 - j, 1.0);
    return std::chrono::milliseconds{static_cast<long long>(capped * dist(rng))};
}

Decision RetryPolicy::decide(const Failure& f, int attempt, std::mt19937_64& rng) const {
    if (f.kind == FailureKind::Cancelled || f.kind == FailureKind::Disk) return {Action::GiveUp, {}};
    if (attempt > max_attempts) return {Action::GiveUp, {}};

    switch (f.kind) {
        case FailureKind::ValidatorMismatch:
            return {Action::RestartFromZero, std::chrono::milliseconds{0}};
        case FailureKind::HttpStatus:
            if (!retryable_status(f.http_status)) return {Action::GiveUp, {}};
            if (f.retry_after) {
                const auto ra = std::min(*f.retry_after, max_retry_after);
                // Honour the server's hint; never retry sooner than our own backoff floor of `base`.
                return {Action::RetrySameState,
                        std::max<std::chrono::milliseconds>(ra, base)};
            }
            return {Action::RetrySameState, backoff(attempt, rng)};
        case FailureKind::Network:
        case FailureKind::Timeout:
        case FailureKind::Protocol:
            return {Action::RetrySameState, backoff(attempt, rng)};
        default:
            return {Action::GiveUp, {}};
    }
}

}  // namespace tdm::retry
