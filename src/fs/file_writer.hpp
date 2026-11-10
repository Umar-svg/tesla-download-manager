#pragma once
// File manager (architecture module 4).
//
// - Output goes to "<final>.tdmpart". When the size is known the file is
//   marked sparse and extended to the full size up front.
// - write() copies the bytes into a queue drained by one writer thread with
//   positional writes, so network threads never block on the disk.
// - request_room() is the back-pressure hook: when more than `budget` bytes
//   are queued it returns false and calls `on_room` once the queue has
//   drained to half the budget.
// - finalize(): flush, FlushFileBuffers, close, then an atomic rename that
//   never overwrites (" (1)", " (2)" ... on collision).

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>

namespace tdm::fs {

class FileWriter {
public:
    virtual ~FileWriter() = default;

    // keep_existing: reopen a part file for resume. It must exist and already have `size` bytes.
    virtual std::error_code open(const std::filesystem::path& part_path, std::optional<std::uint64_t> size,
                                 bool keep_existing) = 0;
    virtual void write(std::uint64_t offset, std::span<const std::byte> data) = 0;  // thread-safe, never blocks
    virtual bool request_room(std::function<void()> on_room) = 0;                  // thread-safe
    virtual std::uint64_t queued_bytes() const = 0;
    virtual std::error_code error() const = 0;  // first write error (sticky)
    virtual std::error_code flush() = 0;        // waits for the queue, then FlushFileBuffers
    virtual std::error_code close() = 0;        // flush + close; the part file stays
    virtual std::error_code finalize(const std::filesystem::path& final_path, std::filesystem::path& actual_path) = 0;
    virtual void discard() = 0;                 // close and delete the part file
    virtual const std::filesystem::path& part_path() const = 0;
};

std::unique_ptr<FileWriter> make_file_writer(std::size_t budget_bytes = 64u << 20);

// "dir/name.ext" → first of "name.ext", "name (1).ext", ... that does not exist.
std::filesystem::path unique_path(const std::filesystem::path& desired);

// Streaming SHA-256 of a file, lower-case hex. Empty string on error.
std::string sha256_file(const std::filesystem::path& path, std::error_code& ec);

}  // namespace tdm::fs
