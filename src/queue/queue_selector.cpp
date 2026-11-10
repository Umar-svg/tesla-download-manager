#include "queue/queue_selector.hpp"

#include <algorithm>

namespace tdm::queue {

std::vector<std::int64_t> select_to_start(std::vector<Candidate> queued,
                                          const std::map<std::int64_t, QueueState>& queues,
                                          std::map<std::int64_t, int> running, int running_total, int global_max) {
    std::sort(queued.begin(), queued.end(), [](const Candidate& a, const Candidate& b) {
        return a.priority != b.priority ? a.priority > b.priority : a.id < b.id;
    });
    std::vector<std::int64_t> out;
    for (const auto& c : queued) {
        if (running_total >= global_max) break;
        const auto q = queues.find(c.queue_id);
        const QueueState qs = q != queues.end() ? q->second : QueueState{global_max, true};  // unknown queue
        if (!qs.open || running[c.queue_id] >= qs.max_active) continue;
        out.push_back(c.id);
        ++running[c.queue_id];
        ++running_total;
    }
    return out;
}

}  // namespace tdm::queue
