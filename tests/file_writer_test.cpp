#include "fs/file_writer.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <fstream>
#include <thread>

using namespace tdm::fs;
namespace stdfs = std::filesystem;

namespace {

stdfs::path fresh_dir(const char* name) {
    auto d = stdfs::temp_directory_path() / "tdm_tests" / name;
    stdfs::remove_all(d);
    stdfs::create_directories(d);
    return d;
}

std::string read_all(const stdfs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

std::span<const std::byte> bytes(const std::string& s) { return std::as_bytes(std::span<const char>(s)); }

}  // namespace

TEST(FileWriter, OutOfOrderPositionalWritesAndFinalize) {
    const auto dir = fresh_dir("fw_order");
    auto w = make_file_writer();
    ASSERT_FALSE(w->open(dir / "a.bin.tdmpart", 10, false));
    EXPECT_EQ(stdfs::file_size(dir / "a.bin.tdmpart"), 10u);  // preallocated
    w->write(5, bytes("56789"));
    w->write(0, bytes("01234"));
    stdfs::path actual;
    ASSERT_FALSE(w->finalize(dir / "a.bin", actual));
    EXPECT_EQ(actual, dir / "a.bin");
    EXPECT_EQ(read_all(actual), "0123456789");
    EXPECT_FALSE(stdfs::exists(dir / "a.bin.tdmpart"));
}

TEST(FileWriter, FinalizeNeverOverwrites) {
    const auto dir = fresh_dir("fw_collide");
    { std::ofstream(dir / "x.txt") << "old"; }
    { std::ofstream(dir / "x (1).txt") << "old1"; }
    auto w = make_file_writer();
    ASSERT_FALSE(w->open(dir / "x.txt.tdmpart", 3, false));
    w->write(0, bytes("new"));
    stdfs::path actual;
    ASSERT_FALSE(w->finalize(dir / "x.txt", actual));
    EXPECT_EQ(actual.filename(), "x (2).txt");
    EXPECT_EQ(read_all(dir / "x.txt"), "old");
    EXPECT_EQ(read_all(actual), "new");
}

TEST(FileWriter, UnknownSizeGrowsAndDiscardDeletes) {
    const auto dir = fresh_dir("fw_unknown");
    auto w = make_file_writer();
    ASSERT_FALSE(w->open(dir / "u.tdmpart", std::nullopt, false));
    w->write(0, bytes("abc"));
    w->write(3, bytes("def"));
    ASSERT_FALSE(w->flush());
    EXPECT_EQ(read_all(dir / "u.tdmpart"), "abcdef");
    w->discard();
    EXPECT_FALSE(stdfs::exists(dir / "u.tdmpart"));
}

TEST(FileWriter, ReopenForResumeRequiresMatchingSize) {
    const auto dir = fresh_dir("fw_resume");
    {
        auto w = make_file_writer();
        ASSERT_FALSE(w->open(dir / "r.tdmpart", 8, false));
        w->write(0, bytes("ABCD"));
        ASSERT_FALSE(w->close());
    }
    auto w = make_file_writer();
    EXPECT_TRUE(w->open(dir / "missing.tdmpart", 8, true));  // must exist
    EXPECT_TRUE(make_file_writer()->open(dir / "r.tdmpart", 9, true));  // wrong size
    ASSERT_FALSE(w->open(dir / "r.tdmpart", 8, true));
    w->write(4, bytes("EFGH"));
    stdfs::path actual;
    ASSERT_FALSE(w->finalize(dir / "r", actual));
    EXPECT_EQ(read_all(actual), "ABCDEFGH");
}

TEST(FileWriter, BackPressureWakesWaiter) {
    const auto dir = fresh_dir("fw_budget");
    auto w = make_file_writer(1024);  // tiny budget
    ASSERT_FALSE(w->open(dir / "b.tdmpart", 4u << 20, false));
    std::string block(4096, 'z');
    std::atomic<int> woken{0};
    bool paused = false;
    for (std::uint64_t off = 0; off < (4u << 20); off += block.size()) {
        w->write(off, bytes(block));
        if (!w->request_room([&] { ++woken; })) paused = true;
    }
    ASSERT_FALSE(w->flush());
    EXPECT_TRUE(paused);
    EXPECT_GE(woken.load(), 1);
    EXPECT_EQ(w->queued_bytes(), 0u);
    w->discard();
}

TEST(FileWriter, ConcurrentWritersProduceExactFile) {
    const auto dir = fresh_dir("fw_threads");
    const std::uint64_t size = 8u << 20;
    auto w = make_file_writer(1u << 20);
    ASSERT_FALSE(w->open(dir / "c.tdmpart", size, false));
    std::vector<std::thread> ts;
    for (int t = 0; t < 8; ++t)
        ts.emplace_back([&, t] {
            std::string chunk(64 * 1024, static_cast<char>('a' + t));
            const std::uint64_t base = size / 8 * t;
            for (std::uint64_t off = 0; off < size / 8; off += chunk.size()) w->write(base + off, bytes(chunk));
        });
    for (auto& t : ts) t.join();
    stdfs::path actual;
    ASSERT_FALSE(w->finalize(dir / "c.bin", actual));
    const auto data = read_all(actual);
    ASSERT_EQ(data.size(), size);
    for (int t = 0; t < 8; ++t) EXPECT_EQ(data[size / 8 * t + 12345], 'a' + t);
}

TEST(FileWriter, Sha256OfKnownContent) {
    const auto dir = fresh_dir("fw_sha");
    { std::ofstream(dir / "abc.txt", std::ios::binary) << "abc"; }
    std::error_code ec;
    EXPECT_EQ(sha256_file(dir / "abc.txt", ec), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_FALSE(ec);
    EXPECT_EQ(sha256_file(dir / "nope", ec), "");
    EXPECT_TRUE(ec);
}

TEST(FileWriter, UniquePath) {
    const auto dir = fresh_dir("fw_unique");
    EXPECT_EQ(unique_path(dir / "n.zip"), dir / "n.zip");
    { std::ofstream(dir / "n.zip") << 1; }
    EXPECT_EQ(unique_path(dir / "n.zip"), dir / "n (1).zip");
}

// Regression: flush() used to wait for an empty queue, which never happens
// while connections keep writing. It must only wait for earlier writes.
TEST(FileWriter, FlushDoesNotStarveUnderContinuousWrites) {
    const auto dir = fresh_dir("fw_flush_load");
    const std::uint64_t size = 256u << 20;
    auto w = make_file_writer(8u << 20);
    ASSERT_FALSE(w->open(dir / "f.tdmpart", size, false));
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> produced{0};
    std::mutex pm;
    std::condition_variable pcv;
    std::atomic<bool> room{true};
    // Behaves like the network side: writes continuously, but honours
    // request_room() back-pressure exactly as read_gate does, so the queue
    // stays bounded and (with the producer faster than the disk) rarely empty.
    std::thread producer([&] {
        std::string chunk(256 * 1024, 'p');
        std::uint64_t off = 0;
        while (!stop) {
            w->write(off, bytes(chunk));
            produced += chunk.size();
            off = (off + chunk.size()) % size;
            room = false;
            if (w->request_room([&] {
                    room = true;
                    pcv.notify_all();
                })) {
                room = true;
            } else {
                std::unique_lock l(pm);
                pcv.wait_for(l, std::chrono::milliseconds(100), [&] { return room.load() || stop.load(); });
            }
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    for (int i = 0; i < 5; ++i) {
        const auto before = produced.load();
        const auto t0 = std::chrono::steady_clock::now();
        EXPECT_FALSE(w->flush());
        EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(5)) << "flush starved";
        EXPECT_GT(produced.load(), before) << "producer should keep writing during flush";
    }
    EXPECT_LE(w->queued_bytes(), (8u << 20) + (256u * 1024));  // back-pressure kept the queue bounded
    stop = true;
    pcv.notify_all();
    producer.join();
    w->discard();
}
