#include "scheduler/scheduler.hpp"

#include <ctime>
#include <nlohmann/json.hpp>

namespace tdm::scheduler {

LocalTime to_local(std::chrono::system_clock::time_point tp) {
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return {tm.tm_wday, tm.tm_hour * 60 + tm.tm_min};
}

bool schedule_allows(const ScheduleRule& rule, LocalTime t) {
    if (!rule.enabled) return true;
    const int start = rule.start_minute.value_or(0);
    const int stop = rule.stop_minute.value_or(24 * 60);
    auto day_ok = [&](int wd) { return (rule.weekdays_mask >> ((wd + 7) % 7)) & 1; };
    if (start == stop) return false;  // empty window
    if (start < stop) return t.minute >= start && t.minute < stop && day_ok(t.weekday);
    // Crosses midnight: the evening part belongs to today, the early-morning part to yesterday's window.
    if (t.minute >= start) return day_ok(t.weekday);
    if (t.minute < stop) return day_ok(t.weekday - 1);
    return false;
}

std::string to_json(const ScheduleRule& r) {
    nlohmann::json j;
    j["enabled"] = r.enabled;
    if (r.start_minute) j["start"] = *r.start_minute;
    if (r.stop_minute) j["stop"] = *r.stop_minute;
    j["days"] = r.weekdays_mask;
    return j.dump();
}

std::optional<ScheduleRule> schedule_from_json(const std::string& s) {
    const auto j = nlohmann::json::parse(s, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return std::nullopt;
    ScheduleRule r;
    r.enabled = j.value("enabled", true);
    if (j.contains("start") && j["start"].is_number_integer()) r.start_minute = j["start"].get<int>();
    if (j.contains("stop") && j["stop"].is_number_integer()) r.stop_minute = j["stop"].get<int>();
    r.weekdays_mask = static_cast<std::uint8_t>(j.value("days", 0x7F) & 0x7F);
    auto valid = [](const std::optional<int>& m) { return !m || (*m >= 0 && *m < 24 * 60); };
    if (!valid(r.start_minute) || !valid(r.stop_minute)) return std::nullopt;
    return r;
}

}  // namespace tdm::scheduler
