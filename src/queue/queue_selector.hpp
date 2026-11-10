#pragma once
// Decides which queued downloads start next. Pure function, no I/O.
//
// Order: higher priority first, then lower id (FIFO). A download starts only
// if the global cap and its queue's max_active allow it and the queue's
// schedule window is open.

#include <cstdint>
#include <map>
#include <vector>

namespace tdm::queue {

struct Candidate {
    std::int64_t id = 0;
    std::int64_t queue_id = 1;
    int priority = 0;
};

struct QueueState {
    int max_active = 3;
    bool open = true;  // schedule window
};

std::vector<std::int64_t> select_to_start(std::vector<Candidate> queued,
                                          const std::map<std::int64_t, QueueState>& queues,
                                          std::map<std::int64_t, int> running_per_queue, int running_total,
                                          int global_max);

}  // namespace tdm::queue
