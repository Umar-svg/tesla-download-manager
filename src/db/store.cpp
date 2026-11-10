#include "db/store.hpp"

#include <sqlite3.h>

#include <chrono>
#include <nlohmann/json.hpp>

namespace tdm::db {
namespace {

constexpr int kSchemaVersion = 4;

constexpr const char* kSchemaV1 = R"sql(
CREATE TABLE IF NOT EXISTS queues (
    id          INTEGER PRIMARY KEY,
    name        TEXT NOT NULL UNIQUE,
    max_active  INTEGER NOT NULL DEFAULT 3
);
INSERT OR IGNORE INTO queues(id, name, max_active) VALUES (1, 'Main', 3);

CREATE TABLE IF NOT EXISTS downloads (
    id               INTEGER PRIMARY KEY AUTOINCREMENT,
    url              TEXT NOT NULL,
    final_url        TEXT NOT NULL DEFAULT '',
    file_path        TEXT NOT NULL DEFAULT '',
    total_size       INTEGER,
    etag             TEXT NOT NULL DEFAULT '',
    last_modified    TEXT NOT NULL DEFAULT '',
    accept_ranges    INTEGER NOT NULL DEFAULT 0,
    sha256_expected  TEXT NOT NULL DEFAULT '',
    sha256_actual    TEXT NOT NULL DEFAULT '',
    state            INTEGER NOT NULL DEFAULT 0,
    error            TEXT NOT NULL DEFAULT '',
    queue_id         INTEGER NOT NULL DEFAULT 1 REFERENCES queues(id),
    priority         INTEGER NOT NULL DEFAULT 0,
    created_at       INTEGER NOT NULL,
    updated_at       INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS downloads_queue ON downloads(queue_id, state, priority);

CREATE TABLE IF NOT EXISTS segments (
    download_id  INTEGER NOT NULL REFERENCES downloads(id) ON DELETE CASCADE,
    idx          INTEGER NOT NULL,
    begin        INTEGER NOT NULL,
    pos          INTEGER NOT NULL,
    end          INTEGER NOT NULL,
    PRIMARY KEY (download_id, idx)
) WITHOUT ROWID;

CREATE TABLE IF NOT EXISTS settings (
    key   TEXT PRIMARY KEY,
    json  TEXT NOT NULL
) WITHOUT ROWID;
)sql";

std::int64_t now_unix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql, -1, &s_, nullptr) != SQLITE_OK)
            throw DbError(std::string("prepare: ") + sqlite3_errmsg(db));
    }
    ~Stmt() { sqlite3_finalize(s_); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    Stmt& bind(int i, std::int64_t v) { check(sqlite3_bind_int64(s_, i, v)); return *this; }
    Stmt& bind(int i, const std::string& v) {
        check(sqlite3_bind_text(s_, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT));
        return *this;
    }
    Stmt& bind_null(int i) { check(sqlite3_bind_null(s_, i)); return *this; }

    bool step() {
        const int rc = sqlite3_step(s_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        throw DbError(std::string("step: ") + sqlite3_errmsg(db_));
    }
    void reset() { sqlite3_reset(s_); sqlite3_clear_bindings(s_); }

    std::int64_t i64(int c) const { return sqlite3_column_int64(s_, c); }
    bool is_null(int c) const { return sqlite3_column_type(s_, c) == SQLITE_NULL; }
    std::string text(int c) const {
        auto p = reinterpret_cast<const char*>(sqlite3_column_text(s_, c));
        return p ? std::string(p, static_cast<std::size_t>(sqlite3_column_bytes(s_, c))) : std::string();
    }

private:
    void check(int rc) {
        if (rc != SQLITE_OK) throw DbError(std::string("bind: ") + sqlite3_errmsg(db_));
    }
    sqlite3* db_;
    sqlite3_stmt* s_ = nullptr;
};

class Txn {
public:
    explicit Txn(sqlite3* db) : db_(db) { run("BEGIN IMMEDIATE"); }
    ~Txn() { if (!done_) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); }
    void commit() { run("COMMIT"); done_ = true; }

private:
    void run(const char* sql) {
        char* err = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
            std::string m = err ? err : "?";
            sqlite3_free(err);
            throw DbError(m);
        }
    }
    sqlite3* db_;
    bool done_ = false;
};

constexpr const char* kDownloadCols =
    "id,url,final_url,file_path,total_size,etag,last_modified,accept_ranges,sha256_expected,"
    "sha256_actual,state,error,queue_id,priority,created_at,updated_at,directory,file_name,part_path,connections,speed_limit,allow_web_page";

DownloadRow read_row(const Stmt& s) {
    DownloadRow r;
    r.id = s.i64(0);
    r.url = s.text(1);
    r.final_url = s.text(2);
    r.file_path = s.text(3);
    if (!s.is_null(4)) r.total_size = static_cast<std::uint64_t>(s.i64(4));
    r.etag = s.text(5);
    r.last_modified = s.text(6);
    r.accept_ranges = s.i64(7) != 0;
    r.sha256_expected = s.text(8);
    r.sha256_actual = s.text(9);
    r.state = static_cast<State>(s.i64(10));
    r.error = s.text(11);
    r.queue_id = s.i64(12);
    r.priority = static_cast<int>(s.i64(13));
    r.created_at = s.i64(14);
    r.updated_at = s.i64(15);
    r.directory = s.text(16);
    r.file_name = s.text(17);
    r.part_path = s.text(18);
    r.connections = static_cast<int>(s.i64(19));
    r.speed_limit = static_cast<std::uint64_t>(s.i64(20));
    r.allow_web_page = s.i64(21) != 0;
    return r;
}

// Binds columns 1..14 shared by INSERT and UPDATE (url .. updated_at, excluding id/created_at order below).
void bind_common(Stmt& s, const DownloadRow& r, std::int64_t updated) {
    s.bind(1, r.url).bind(2, r.final_url).bind(3, r.file_path);
    if (r.total_size) s.bind(4, static_cast<std::int64_t>(*r.total_size));
    else s.bind_null(4);
    s.bind(5, r.etag).bind(6, r.last_modified).bind(7, std::int64_t{r.accept_ranges ? 1 : 0});
    s.bind(8, r.sha256_expected).bind(9, r.sha256_actual).bind(10, static_cast<std::int64_t>(r.state));
    s.bind(11, r.error).bind(12, r.queue_id).bind(13, std::int64_t{r.priority}).bind(14, updated);
    s.bind(16, r.directory).bind(17, r.file_name).bind(18, r.part_path).bind(19, std::int64_t{r.connections});
    s.bind(20, static_cast<std::int64_t>(r.speed_limit));
    s.bind(21, std::int64_t{r.allow_web_page ? 1 : 0});
}

}  // namespace

Store::Store(const std::string& path_utf8) {
    if (sqlite3_open_v2(path_utf8.c_str(), &db_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        std::string m = db_ ? sqlite3_errmsg(db_) : "open failed";
        sqlite3_close(db_);
        db_ = nullptr;
        throw DbError(m);
    }
    sqlite3_busy_timeout(db_, 5000);
    exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON;");
    migrate();
}

Store::~Store() { sqlite3_close(db_); }

void Store::exec(const char* sql) const {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string m = err ? err : "exec failed";
        sqlite3_free(err);
        throw DbError(m);
    }
}

int Store::schema_version() const {
    Stmt s(db_, "PRAGMA user_version");
    s.step();
    return static_cast<int>(s.i64(0));
}

void Store::migrate() {
    const int v = schema_version();
    if (v > kSchemaVersion) throw DbError("database is from a newer Tesla Download Manager");
    if (v < 1) {
        Txn t(db_);
        exec(kSchemaV1);
        exec("PRAGMA user_version = 1");
        t.commit();
    }
    if (v < 2) {
        Txn t(db_);
        exec("ALTER TABLE downloads ADD COLUMN directory TEXT NOT NULL DEFAULT '';"
             "ALTER TABLE downloads ADD COLUMN file_name TEXT NOT NULL DEFAULT '';"
             "ALTER TABLE downloads ADD COLUMN part_path TEXT NOT NULL DEFAULT '';"
             "ALTER TABLE downloads ADD COLUMN connections INTEGER NOT NULL DEFAULT 8;"
             "PRAGMA user_version = 2;");
        t.commit();
    }
    if (v < 3) {
        Txn t(db_);
        exec("ALTER TABLE downloads ADD COLUMN speed_limit INTEGER NOT NULL DEFAULT 0;"
             "PRAGMA user_version = 3;");
        t.commit();
    }
    if (v < 4) {
        Txn t(db_);
        exec("ALTER TABLE downloads ADD COLUMN allow_web_page INTEGER NOT NULL DEFAULT 0;"
             "PRAGMA user_version = 4;");
        t.commit();
    }
}

std::int64_t Store::add_download(const DownloadRow& row) {
    const auto now = now_unix();
    Stmt s(db_,
           "INSERT INTO downloads(url,final_url,file_path,total_size,etag,last_modified,accept_ranges,"
           "sha256_expected,sha256_actual,state,error,queue_id,priority,updated_at,created_at,directory,file_name,"
           "part_path,connections,speed_limit,allow_web_page) "
           "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,?21)");
    bind_common(s, row, now);
    s.bind(15, row.created_at ? row.created_at : now);
    s.step();
    return sqlite3_last_insert_rowid(db_);
}

void Store::update_download(const DownloadRow& row) {
    Stmt s(db_,
           "UPDATE downloads SET url=?1,final_url=?2,file_path=?3,total_size=?4,etag=?5,last_modified=?6,"
           "accept_ranges=?7,sha256_expected=?8,sha256_actual=?9,state=?10,error=?11,queue_id=?12,"
           "priority=?13,updated_at=?14,directory=?16,file_name=?17,part_path=?18,connections=?19,speed_limit=?20,allow_web_page=?21 WHERE id=?15");
    bind_common(s, row, now_unix());
    s.bind(15, row.id);
    s.step();
    if (sqlite3_changes(db_) != 1) throw DbError("update_download: no such id");
}

std::optional<DownloadRow> Store::get_download(std::int64_t id) const {
    Stmt s(db_, (std::string("SELECT ") + kDownloadCols + " FROM downloads WHERE id=?1").c_str());
    s.bind(1, id);
    if (!s.step()) return std::nullopt;
    return read_row(s);
}

std::vector<DownloadRow> Store::list_downloads() const {
    Stmt s(db_, (std::string("SELECT ") + kDownloadCols + " FROM downloads ORDER BY id").c_str());
    std::vector<DownloadRow> out;
    while (s.step()) out.push_back(read_row(s));
    return out;
}

void Store::remove_download(std::int64_t id) {
    Stmt s(db_, "DELETE FROM downloads WHERE id=?1");
    s.bind(1, id);
    s.step();
}

void Store::save_segments(std::int64_t download_id, const std::vector<SegmentRow>& rows) {
    Txn t(db_);
    {
        Stmt del(db_, "DELETE FROM segments WHERE download_id=?1");
        del.bind(1, download_id);
        del.step();
        Stmt ins(db_, "INSERT INTO segments(download_id,idx,begin,pos,end) VALUES(?1,?2,?3,?4,?5)");
        std::int64_t idx = 0;
        for (const auto& r : rows) {
            ins.bind(1, download_id).bind(2, idx++);
            ins.bind(3, static_cast<std::int64_t>(r.begin));
            ins.bind(4, static_cast<std::int64_t>(r.pos));
            ins.bind(5, static_cast<std::int64_t>(r.end));
            ins.step();
            ins.reset();
        }
    }
    t.commit();
}

std::vector<SegmentRow> Store::load_segments(std::int64_t download_id) const {
    Stmt s(db_, "SELECT begin,pos,end FROM segments WHERE download_id=?1 ORDER BY begin");
    s.bind(1, download_id);
    std::vector<SegmentRow> out;
    while (s.step())
        out.push_back({static_cast<std::uint64_t>(s.i64(0)), static_cast<std::uint64_t>(s.i64(1)),
                       static_cast<std::uint64_t>(s.i64(2))});
    return out;
}

std::int64_t Store::add_queue(const std::string& name, int max_active) {
    Stmt s(db_, "INSERT INTO queues(name, max_active) VALUES(?1, ?2)");
    s.bind(1, name).bind(2, std::int64_t{max_active});
    s.step();
    return sqlite3_last_insert_rowid(db_);
}

void Store::update_queue(const QueueRow& q) {
    Stmt s(db_, "UPDATE queues SET name=?1, max_active=?2 WHERE id=?3");
    s.bind(1, q.name).bind(2, std::int64_t{q.max_active}).bind(3, q.id);
    s.step();
    if (sqlite3_changes(db_) != 1) throw DbError("update_queue: no such id");
}

std::vector<QueueRow> Store::list_queues() const {
    Stmt s(db_, "SELECT id, name, max_active FROM queues ORDER BY id");
    std::vector<QueueRow> out;
    while (s.step()) out.push_back({s.i64(0), s.text(1), static_cast<int>(s.i64(2))});
    return out;
}

void Store::remove_queue(std::int64_t id) {
    if (id == 1) throw DbError("the main queue cannot be removed");
    Txn t(db_);
    {
        Stmt mv(db_, "UPDATE downloads SET queue_id=1 WHERE queue_id=?1");
        mv.bind(1, id);
        mv.step();
        Stmt del(db_, "DELETE FROM queues WHERE id=?1");
        del.bind(1, id);
        del.step();
    }
    t.commit();
}

void Store::set_setting(const std::string& key, const std::string& json) {
    (void)nlohmann::json::parse(json);  // reject invalid JSON early (throws)
    Stmt s(db_, "INSERT INTO settings(key,json) VALUES(?1,?2) ON CONFLICT(key) DO UPDATE SET json=excluded.json");
    s.bind(1, key).bind(2, json);
    s.step();
}

std::optional<std::string> Store::get_setting(const std::string& key) const {
    Stmt s(db_, "SELECT json FROM settings WHERE key=?1");
    s.bind(1, key);
    if (!s.step()) return std::nullopt;
    return s.text(0);
}

}  // namespace tdm::db
