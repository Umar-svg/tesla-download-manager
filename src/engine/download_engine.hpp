#pragma once
// Engine facade (implemented in Phase 5). The UI and any future CLI talk only
// to this interface; everything below it is Qt-free.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "db/store.hpp"
#include "scheduler/scheduler.hpp"

namespace tdm::engine {

using DownloadId = std::int64_t;

struct AddRequest {
    std::string url;
    std::filesystem::path directory;
    std::string file_name;               // empty → from Content-Disposition / URL
    std::string sha256_expected;         // optional, lower-case hex
    std::int64_t queue_id = 1;
    int connections = 8;                 // 1..32
    std::optional<std::uint64_t> speed_limit_bps;
    bool start_now = true;
    bool allow_web_page = false;         // save an HTML page instead of failing with "web page"
};

struct SegmentView {
    std::uint64_t begin, pos, end;
    bool active;
};

struct Snapshot {                        // pushed to observers at ~4 Hz
    DownloadId id = 0;
    db::State state = db::State::Queued;
    std::string file_name;
    std::optional<std::uint64_t> total;
    std::uint64_t downloaded = 0;
    double bytes_per_sec = 0;
    int active_connections = 0;
    std::vector<SegmentView> segments;
    std::string error;
    // descriptive fields for the UI
    std::string url;
    std::string file_path;               // final path once completed
    std::int64_t queue_id = 1;
    int priority = 0;
    std::uint64_t speed_limit = 0;       // bytes/s, 0 = unlimited
    bool removed = false;                // the download was removed from the list
};

struct QueueInfo {
    std::int64_t id = 0;
    std::string name;
    int max_active = 3;
    std::optional<scheduler::ScheduleRule> schedule;
    bool open = true;  // schedule window open right now
};

class DownloadEngine {
public:
    using Observer = std::function<void(const Snapshot&)>;  // called on an engine thread
    virtual ~DownloadEngine() = default;

    virtual DownloadId add(const AddRequest& req) = 0;
    virtual void start(DownloadId) = 0;
    virtual void pause(DownloadId) = 0;
    virtual void cancel(DownloadId, bool delete_file) = 0;
    // Removes the download from the list (stops it first). delete_file also
    // deletes a completed file; a partial .tdmpart is always deleted.
    virtual void remove(DownloadId, bool delete_file) = 0;
    virtual void set_max_active(int max_active) = 0;  // global cap on concurrent downloads
    virtual void set_global_speed_limit(std::optional<std::uint64_t> bps) = 0;  // nullopt/0 = unlimited
    virtual void set_speed_limit(DownloadId, std::optional<std::uint64_t> bps) = 0;
    virtual void set_priority(DownloadId, int priority) = 0;
    virtual void move_to_queue(DownloadId, std::int64_t queue_id) = 0;
    virtual std::int64_t add_queue(const std::string& name, int max_active) = 0;
    virtual void set_queue_limit(std::int64_t queue_id, int max_active) = 0;
    virtual void set_queue_schedule(std::int64_t queue_id, std::optional<scheduler::ScheduleRule> rule) = 0;
    virtual std::vector<QueueInfo> queues() const = 0;
    virtual std::vector<Snapshot> snapshots() const = 0;
    virtual void subscribe(Observer obs) = 0;
    virtual void shutdown() = 0;         // persists state, closes connections
};

}  // namespace tdm::engine
