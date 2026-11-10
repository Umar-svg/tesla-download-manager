// Regression: another handle reading the part file while the writer is busy
// (here std::ifstream, in real life an antivirus scanner or a file previewer)
// must not break writes or flushes.
#include "fs/file_writer.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <thread>

using namespace tdm::fs;
namespace stdfs = std::filesystem;

TEST(FileWriterSharing, ConcurrentReaderDoesNotBreakWritesOrFlush) {
    const auto dir = stdfs::temp_directory_path() / "tdm_tests" / "fw_sharing";
    stdfs::remove_all(dir);
    stdfs::create_directories(dir);
    const auto part = dir / "s.tdmpart";
    auto w = make_file_writer(8u << 20);
    ASSERT_FALSE(w->open(part, 32u << 20, false));
    std::string chunk(64 * 1024, 'x');
    const auto bytes = std::as_bytes(std::span<const char>(chunk));
    for (std::uint64_t off = 0; off < (8u << 20); off += chunk.size()) w->write(off, bytes);
    ASSERT_FALSE(w->flush()) << w->error().message();

    std::ifstream reader(part, std::ios::binary);  // a second handle, open while writing continues
    ASSERT_TRUE(reader.is_open());
    for (std::uint64_t off = 8u << 20; off < (16u << 20); off += chunk.size()) w->write(off, bytes);
    char buf[4096];
    reader.read(buf, sizeof buf);
    EXPECT_EQ(reader.gcount(), static_cast<std::streamsize>(sizeof buf));
    const auto fec = w->flush();
    EXPECT_FALSE(fec) << "flush with a reader open: " << fec.message();
    EXPECT_FALSE(w->error()) << "write with a reader open: " << w->error().message();
    reader.close();
    stdfs::path actual;
    EXPECT_FALSE(w->finalize(dir / "s.bin", actual));
    stdfs::remove_all(dir);
}

// Regression: finalize used to fail at once with ERROR_SHARING_VIOLATION when
// another handle (antivirus scan, previewer, a test hashing the file) had the
// part file open without FILE_SHARE_DELETE, turning a complete, verified
// download into a "disk error". It must wait for the other handle and retry.
TEST(FileWriterSharing, FinalizeWaitsForAnotherReaderToLetGo) {
    const auto dir = stdfs::temp_directory_path() / "tdm_tests" / "fw_sharing_rename";
    stdfs::remove_all(dir);
    stdfs::create_directories(dir);
    const auto part = dir / "r.tdmpart";
    auto w = make_file_writer();
    ASSERT_FALSE(w->open(part, 4096, false));
    std::string data(4096, 'r');
    w->write(0, std::as_bytes(std::span<const char>(data)));
    auto reader = std::make_unique<std::ifstream>(part, std::ios::binary);  // blocks rename while open
    ASSERT_TRUE(reader->is_open());
    std::thread release([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        reader.reset();
    });
    stdfs::path actual;
    const auto ec = w->finalize(dir / "r.bin", actual);
    release.join();
    EXPECT_FALSE(ec) << ec.message();
    EXPECT_EQ(actual, dir / "r.bin");
    EXPECT_TRUE(stdfs::exists(dir / "r.bin"));
    stdfs::remove_all(dir);
}
