#pragma once
// Failure classification and retry timing (architecture: "Error policy").

#include <chrono>
#include <optional>
#include <random>

namespace tdm::retry {

enum class FailureKind {
    Network,            // reset, refused, DNS, TLS handshake failure
    Timeout,            // connect / idle read timeout
    HttpStatus,         // unexpected status; see http_status
    ValidatorMismatch,  // ETag/Last-Modified/size changed: restart from zero
    Protocol,           // malformed response, bad Content-Range
    Disk,               // disk full, access denied
    Cancelled,          // user action
};

struct Failure {
    FailureKind kind = FailureKind::Network;
    int http_status = 0;
    std::optional<std::chrono::seconds> retry_after;  // parsed Retry-After, if any
};

enum class Action { RetrySameState, RestartFromZero, GiveUp };

struct Decision {
    Action action = Action::GiveUp;
    std::chrono::milliseconds delay{0};
};

struct RetryPolicy {
    int max_attempts = 8;                              // per segment / request
    std::chrono::milliseconds base{500};
    std::chrono::milliseconds cap{60'000};
    double jitter = 0.5;                               // delay *= U[1-jitter, 1]
    std::chrono::seconds max_retry_after{3600};        // ignore absurd server hints

    static bool retryable_status(int status) noexcept;
    std::chrono::milliseconds backoff(int attempt, std::mt19937_64& rng) const;

    // attempt = number of failures so far for this unit of work, starting at 1.
    Decision decide(const Failure& f, int attempt, std::mt19937_64& rng) const;
};

}  // namespace tdm::retry
