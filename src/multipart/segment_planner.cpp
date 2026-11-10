#include "multipart/segment_planner.hpp"

#include <algorithm>
#include <stdexcept>

namespace tdm::multipart {

SegmentPlanner::SegmentPlanner(std::uint64_t total, Options opts) : total_(total), opts_(opts) {
    if (opts_.min_split == 0) opts_.min_split = 1;
    if (opts_.align == 0) opts_.align = 1;
    reset();
}

void SegmentPlanner::reset() {
    segs_.clear();
    segs_.push_back(Segment{0, 0, 0, total_, false});
}

std::optional<SegmentPlanner> SegmentPlanner::restore(std::uint64_t total, Options opts, std::vector<Segment> rows) {
    if (rows.empty()) return std::nullopt;
    std::sort(rows.begin(), rows.end(), [](const Segment& a, const Segment& b) { return a.begin < b.begin; });
    std::uint64_t expect = 0;
    for (const auto& r : rows) {
        if (r.begin != expect || r.pos < r.begin || r.pos > r.end || r.end < r.begin) return std::nullopt;
        expect = r.end;
    }
    if (expect != total) return std::nullopt;

    SegmentPlanner p(total, opts);
    p.segs_.clear();
    for (std::size_t i = 0; i < rows.size(); ++i) {
        auto s = rows[i];
        s.id = static_cast<SegmentId>(i);
        s.assigned = false;
        p.segs_.push_back(s);
    }
    return p;
}

Segment& SegmentPlanner::at(SegmentId id) {
    if (id >= segs_.size()) throw std::out_of_range("bad segment id");
    return segs_[id];
}
const Segment& SegmentPlanner::at(SegmentId id) const {
    if (id >= segs_.size()) throw std::out_of_range("bad segment id");
    return segs_[id];
}

std::optional<Assignment> SegmentPlanner::acquire() {
    // 1. Idle work first (largest remaining), e.g. after a dropped connection or a restore.
    Segment* best = nullptr;
    for (auto& s : segs_)
        if (!s.assigned && !s.done() && (!best || s.remaining() > best->remaining())) best = &s;
    if (best) {
        best->assigned = true;
        return Assignment{best->id, best->pos, best->end};
    }

    // 2. Split the assigned segment with the most work left.
    for (auto& s : segs_)
        if (s.assigned && !s.done() && (!best || s.remaining() > best->remaining())) best = &s;
    if (!best || best->remaining() < 2 * opts_.min_split) return std::nullopt;

    std::uint64_t mid = best->pos + best->remaining() / 2;
    mid -= mid % opts_.align;
    if (mid < best->pos + opts_.min_split || best->end - mid < opts_.min_split) {
        mid = best->pos + best->remaining() / 2;  // alignment would starve one side; split unaligned
    }
    Segment ns{static_cast<SegmentId>(segs_.size()), mid, mid, best->end, true};
    best->end = mid;  // `best` must not be used after push_back (reallocation)
    segs_.push_back(ns);
    return Assignment{ns.id, ns.pos, ns.end};
}

std::uint64_t SegmentPlanner::limit(SegmentId id) const { return at(id).remaining(); }
std::uint64_t SegmentPlanner::position(SegmentId id) const { return at(id).pos; }

std::uint64_t SegmentPlanner::commit(SegmentId id, std::uint64_t n) {
    auto& s = at(id);
    if (n > s.remaining()) throw std::logic_error("commit past segment end");
    s.pos += n;
    return s.remaining();
}

void SegmentPlanner::release(SegmentId id) { at(id).assigned = false; }

bool SegmentPlanner::complete() const noexcept {
    return std::all_of(segs_.begin(), segs_.end(), [](const Segment& s) { return s.done(); });
}

std::uint64_t SegmentPlanner::downloaded() const noexcept {
    std::uint64_t n = 0;
    for (const auto& s : segs_) n += s.pos - s.begin;
    return n;
}

std::size_t SegmentPlanner::assigned_count() const noexcept {
    return static_cast<std::size_t>(std::count_if(segs_.begin(), segs_.end(),
                                                  [](const Segment& s) { return s.assigned && !s.done(); }));
}

std::vector<Segment> SegmentPlanner::snapshot() const {
    auto v = segs_;
    std::sort(v.begin(), v.end(), [](const Segment& a, const Segment& b) { return a.begin < b.begin; });
    return v;
}

bool SegmentPlanner::invariant_holds() const {
    std::uint64_t expect = 0;
    for (const auto& s : snapshot()) {
        if (s.begin != expect || s.pos < s.begin || s.pos > s.end) return false;
        expect = s.end;
    }
    return expect == total_;
}

}  // namespace tdm::multipart
