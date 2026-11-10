#pragma once
// SQLite persistence (schema v1, see docs/architecture.md "Persistence").
// One Store per thread; the engine owns it on the DB thread.

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

struct sqlite3;

namespace tdm::db {

class DbError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class State : int {
    Queued = 0, Probing, Downloading, Waiting, Paused, Verifying, Completed, Failed, Cancelled
};

struct DownloadRow {
    std::int64_t id = 0;
    std::string url;
    std::string final_url;
    std::string file_path;
    std::optional<std::uint64_t> total_size;
    std::string etag;
    std::string last_modified;
    bool accept_ranges = false;
    std::string sha256_expected;
    std::string sha256_actual;
    State state = State::Queued;
    std::string error;
    std::int64_t queue_id = 1;
    int priority = 0;
    std::int64_t created_at = 0;  // unix seconds
    std::int64_t updated_at = 0;
    // schema v2
    std::string directory;        // target folder (UTF-8)
    std::string file_name;        // chosen name; empty until known (or user-fixed at add time)
    std::string part_path;        // ".tdmpart" while in progress
    int connections = 8;
    // schema v3
    std::uint64_t speed_limit = 0;  // bytes/s, 0 = unlimited
};

struct QueueRow {
    std::int64_t id = 0;
    std::string name;
    int max_active = 3;
};

struct SegmentRow {
    std::uint64_t begin = 0, pos = 0, end = 0;
};

class Store {
public:
    // ":memory:" works for tests. Creates/migrates the schema.
    explicit Store(const std::string& path_utf8);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    int schema_version() const;

    std::int64_t add_download(const DownloadRow& row);   // returns new id
    void update_download(const DownloadRow& row);        // by row.id
    std::optional<DownloadRow> get_download(std::int64_t id) const;
    std::vector<DownloadRow> list_downloads() const;
    void remove_download(std::int64_t id);               // also removes its segments

    // Replace the whole segment table of a download atomically.
    void save_segments(std::int64_t download_id, const std::vector<SegmentRow>& rows);
    std::vector<SegmentRow> load_segments(std::int64_t download_id) const;

    // Queues. Queue 1 ("Main") always exists and cannot be removed.
    std::int64_t add_queue(const std::string& name, int max_active);
    void update_queue(const QueueRow& q);
    std::vector<QueueRow> list_queues() const;
    void remove_queue(std::int64_t id);  // its downloads move to queue 1

    // Settings are stored as JSON text.
    void set_setting(const std::string& key, const std::string& json);
    std::optional<std::string> get_setting(const std::string& key) const;

private:
    void exec(const char* sql) const;
    void migrate();

    sqlite3* db_ = nullptr;
};

}  // namespace tdm::db
