#include "db/store.hpp"

#include <gtest/gtest.h>

#include <filesystem>

#include <sqlite3.h>

using namespace tdm::db;

TEST(Store, CreatesLatestSchema) {
    Store s(":memory:");
    EXPECT_EQ(s.schema_version(), 4);
    EXPECT_TRUE(s.list_downloads().empty());
}

TEST(Store, DownloadRoundTrip) {
    Store s(":memory:");
    DownloadRow r;
    r.url = "http://127.0.0.1:8765/f/a.bin";
    r.file_path = "C:\\Downloads\\a.bin";
    r.total_size = 5'000'000'000ull;  // > 4 GiB must survive
    r.etag = "\"abc\"";
    r.accept_ranges = true;
    const auto id = s.add_download(r);
    ASSERT_GT(id, 0);

    auto got = s.get_download(id);
    ASSERT_TRUE(got);
    EXPECT_EQ(got->url, r.url);
    EXPECT_EQ(*got->total_size, 5'000'000'000ull);
    EXPECT_EQ(got->etag, "\"abc\"");
    EXPECT_TRUE(got->accept_ranges);
    EXPECT_EQ(got->state, State::Queued);
    EXPECT_GT(got->created_at, 0);

    got->state = State::Completed;
    got->sha256_actual = "deadbeef";
    got->total_size.reset();
    s.update_download(*got);
    auto again = s.get_download(id);
    EXPECT_EQ(again->state, State::Completed);
    EXPECT_EQ(again->sha256_actual, "deadbeef");
    EXPECT_FALSE(again->total_size);

    DownloadRow missing;
    missing.id = 999;
    EXPECT_THROW(s.update_download(missing), DbError);
}

TEST(Store, SegmentsReplacedAtomicallyAndCascade) {
    Store s(":memory:");
    DownloadRow r;
    r.url = "http://x/y";
    const auto id = s.add_download(r);
    s.save_segments(id, {{0, 10, 100}, {100, 150, 200}});
    s.save_segments(id, {{100, 120, 200}, {0, 50, 100}});
    auto segs = s.load_segments(id);
    ASSERT_EQ(segs.size(), 2u);
    EXPECT_EQ(segs[0].begin, 0u);  // ordered by begin
    EXPECT_EQ(segs[0].pos, 50u);
    EXPECT_EQ(segs[1].pos, 120u);

    s.remove_download(id);
    EXPECT_TRUE(s.load_segments(id).empty());
}

TEST(Store, SettingsAreJson) {
    Store s(":memory:");
    s.set_setting("connections", "8");
    s.set_setting("connections", R"({"default":8,"max":32})");
    EXPECT_EQ(*s.get_setting("connections"), R"({"default":8,"max":32})");
    EXPECT_FALSE(s.get_setting("nope"));
    EXPECT_ANY_THROW(s.set_setting("bad", "{not json"));
}

TEST(Store, PersistsAcrossReopen) {
    const auto path = std::filesystem::temp_directory_path() / "tdm_store_test.sqlite";
    std::filesystem::remove(path);
    std::int64_t id = 0;
    {
        Store s(path.string());
        DownloadRow r;
        r.url = "http://x/y";
        id = s.add_download(r);
        s.save_segments(id, {{0, 7, 9}});
    }
    {
        Store s(path.string());
        EXPECT_EQ(s.schema_version(), 4);
        ASSERT_TRUE(s.get_download(id));
        EXPECT_EQ(s.load_segments(id).at(0).pos, 7u);
    }
    for (auto ext : {"", "-wal", "-shm"}) std::filesystem::remove(path.string() + ext);
}

TEST(Store, V2FieldsRoundTrip) {
    Store s(":memory:");
    DownloadRow r;
    r.url = "http://x/y";
    r.directory = "C:\\Users\\me\\Downloads\\\xE2\x82\xAC";  // UTF-8 (euro sign)
    r.file_name = "y.bin";
    r.part_path = "C:\\Users\\me\\Downloads\\y.bin.tdmpart";
    r.connections = 16;
    const auto id = s.add_download(r);
    auto got = s.get_download(id);
    ASSERT_TRUE(got);
    EXPECT_EQ(got->directory, r.directory);
    EXPECT_EQ(got->file_name, "y.bin");
    EXPECT_EQ(got->part_path, r.part_path);
    EXPECT_EQ(got->connections, 16);
    got->connections = 4;
    got->part_path.clear();
    s.update_download(*got);
    EXPECT_EQ(s.get_download(id)->connections, 4);
    EXPECT_EQ(s.get_download(id)->part_path, "");
}

// A database written by schema v1 (Phase 2) must migrate in place without losing rows.
TEST(Store, MigratesV1DatabaseToLatest) {
    const auto path = std::filesystem::temp_directory_path() / "tdm_store_v1.sqlite";
    for (auto ext : {"", "-wal", "-shm"}) std::filesystem::remove(path.string() + ext);
    {
        sqlite3* db = nullptr;
        ASSERT_EQ(sqlite3_open(path.string().c_str(), &db), SQLITE_OK);
        const char* v1 =
            "CREATE TABLE queues (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE, max_active INTEGER NOT NULL "
            "DEFAULT 3);"
            "INSERT INTO queues VALUES (1,'Main',3);"
            "CREATE TABLE downloads (id INTEGER PRIMARY KEY AUTOINCREMENT, url TEXT NOT NULL, final_url TEXT NOT NULL "
            "DEFAULT '', file_path TEXT NOT NULL DEFAULT '', total_size INTEGER, etag TEXT NOT NULL DEFAULT '', "
            "last_modified TEXT NOT NULL DEFAULT '', accept_ranges INTEGER NOT NULL DEFAULT 0, sha256_expected TEXT "
            "NOT NULL DEFAULT '', sha256_actual TEXT NOT NULL DEFAULT '', state INTEGER NOT NULL DEFAULT 0, error TEXT "
            "NOT NULL DEFAULT '', queue_id INTEGER NOT NULL DEFAULT 1 REFERENCES queues(id), priority INTEGER NOT NULL "
            "DEFAULT 0, created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL);"
            "CREATE TABLE segments (download_id INTEGER NOT NULL REFERENCES downloads(id) ON DELETE CASCADE, idx "
            "INTEGER NOT NULL, begin INTEGER NOT NULL, pos INTEGER NOT NULL, end INTEGER NOT NULL, PRIMARY KEY "
            "(download_id, idx)) WITHOUT ROWID;"
            "CREATE TABLE settings (key TEXT PRIMARY KEY, json TEXT NOT NULL) WITHOUT ROWID;"
            "INSERT INTO downloads(url,total_size,etag,state,created_at,updated_at) "
            "VALUES('http://old/file.iso',1000,'\"e\"',2,1,1);"
            "INSERT INTO segments VALUES(1,0,0,400,1000);"
            "PRAGMA user_version=1;";
        ASSERT_EQ(sqlite3_exec(db, v1, nullptr, nullptr, nullptr), SQLITE_OK) << sqlite3_errmsg(db);
        sqlite3_close(db);
    }
    {
        Store s(path.string());
        EXPECT_EQ(s.schema_version(), 4);
        auto row = s.get_download(1);
        ASSERT_TRUE(row);
        EXPECT_EQ(row->url, "http://old/file.iso");
        EXPECT_EQ(*row->total_size, 1000u);
        EXPECT_EQ(row->etag, "\"e\"");
        EXPECT_EQ(row->state, State::Downloading);
        EXPECT_EQ(row->connections, 8);  // column default
        EXPECT_EQ(row->directory, "");
        EXPECT_EQ(s.load_segments(1).at(0).pos, 400u);
    }
    for (auto ext : {"", "-wal", "-shm"}) std::filesystem::remove(path.string() + ext);
}

TEST(Store, QueuesCrudAndSpeedLimit) {
    Store s(":memory:");
    auto qs = s.list_queues();
    ASSERT_EQ(qs.size(), 1u);
    EXPECT_EQ(qs[0].name, "Main");
    const auto night = s.add_queue("Night", 2);
    s.update_queue({night, "Night owls", 4});
    qs = s.list_queues();
    ASSERT_EQ(qs.size(), 2u);
    EXPECT_EQ(qs[1].name, "Night owls");
    EXPECT_EQ(qs[1].max_active, 4);

    DownloadRow r;
    r.url = "http://x/y";
    r.queue_id = night;
    r.speed_limit = 123456;
    const auto id = s.add_download(r);
    EXPECT_EQ(s.get_download(id)->speed_limit, 123456u);
    s.remove_queue(night);
    EXPECT_EQ(s.get_download(id)->queue_id, 1);  // moved back to Main
    EXPECT_EQ(s.list_queues().size(), 1u);
    EXPECT_THROW(s.remove_queue(1), DbError);
}

TEST(Store, AllowWebPageRoundTrip) {
    Store s(":memory:");
    DownloadRow r;
    r.url = "http://x/page";
    r.allow_web_page = true;
    const auto id = s.add_download(r);
    EXPECT_TRUE(s.get_download(id)->allow_web_page);
    auto got = *s.get_download(id);
    got.allow_web_page = false;
    s.update_download(got);
    EXPECT_FALSE(s.get_download(id)->allow_web_page);
}
