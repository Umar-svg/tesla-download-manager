#pragma once
// Dynamic bisection segment planner (architecture decision D2).
//
// The file [0, total) is covered by contiguous segments. A connection owns at
// most one segment and downloads it from `pos` towards `end`. When a slot is
// free and no idle segment is left, the planner splits the assigned segment
// with the most bytes remaining at its midpoint: the new connection takes the
// back half, and the owner's `end` shrinks. Connections ask `limit()` before
// consuming more bytes, so an open-ended `Range: bytes=pos-` request is cut
// at the current boundary (work stealing without a fixed segment table).
//
// Not thread-safe: the engine drives one planner from one strand.

#include <cstdint>
#include <optional>
#include <vector>

namespace tdm::multipart {

using SegmentId = std::uint32_t;

struct Segment {
    SegmentId id = 0;
    std::uint64_t begin = 0;
    std::uint64_t pos = 0;   // next byte to download
    std::uint64_t end = 0;   // exclusive
    bool assigned = false;
    std::uint64_t remaining() const noexcept { return end - pos; }
    bool done() const noexcept { return pos >= end; }
};

struct Assignment {
    SegmentId id;
    std::uint64_t offset;  // send Range: bytes=offset-
    std::uint64_t end;     // current exclusive end; may shrink later, re-check limit()
};

class SegmentPlanner {
public:
    struct Options {
        std::uint64_t min_split = 1ull << 20;  // never create a piece smaller than this
        std::uint64_t align = 64 * 1024;       // split points are aligned to this
    };

    SegmentPlanner(std::uint64_t total, Options opts);

    // Rebuild from persisted rows. Rows must cover [0,total) contiguously when
    // sorted by begin; they all come back unassigned. Returns nullopt if invalid.
    static std::optional<SegmentPlanner> restore(std::uint64_t total, Options opts, std::vector<Segment> rows);

    // Work for a new or freed connection: an idle unfinished segment if any,
    // otherwise a split of the largest assigned segment. nullopt = nothing worth doing.
    std::optional<Assignment> acquire();

    // Bytes the owner of `id` may still consume (end - pos). 0 means stop.
    std::uint64_t limit(SegmentId id) const;
    std::uint64_t position(SegmentId id) const;  // file offset of the segment's next byte

    // Record `n` bytes written at the segment's current pos. n must be <= limit(id).
    // Returns the new limit.
    std::uint64_t commit(SegmentId id, std::uint64_t n);

    // The connection that owned `id` is gone; progress is kept, segment becomes idle.
    void release(SegmentId id);

    // Forget all progress (used when validators show the file changed).
    void reset();

    void set_min_split(std::uint64_t bytes) noexcept { opts_.min_split = bytes ? bytes : 1; }

    bool complete() const noexcept;
    std::uint64_t total() const noexcept { return total_; }
    std::uint64_t downloaded() const noexcept;
    std::size_t assigned_count() const noexcept;
    std::vector<Segment> snapshot() const;  // sorted by begin
    bool invariant_holds() const;           // contiguity check, for tests and debug asserts

private:
    Segment& at(SegmentId id);
    const Segment& at(SegmentId id) const;

    std::uint64_t total_;
    Options opts_;
    std::vector<Segment> segs_;  // index == id; never shrinks except on reset()
};

}  // namespace tdm::multipart
