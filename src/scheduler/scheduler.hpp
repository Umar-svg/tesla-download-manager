#pragma once
// Queue schedules: a daily time window plus a weekday mask, evaluated in
// local time by the engine's maintenance thread.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace tdm::scheduler {

struct ScheduleRule {
    bool enabled = true;
    std::optional<int> start_minute;   // minutes after local midnight, 0..1439; none = 00:00
    std::optional<int> stop_minute;    // none = end of day; start > stop crosses midnight
    std::uint8_t weekdays_mask = 0x7F; // bit 0 = Sunday ... bit 6 = Saturday (day the window opens)
};

struct LocalTime {
    int weekday = 0;  // 0 = Sunday
    int minute = 0;   // 0..1439
};

LocalTime to_local(std::chrono::system_clock::time_point tp);

// Whether a queue with this rule may run at `t`. A disabled rule always allows.
bool schedule_allows(const ScheduleRule& rule, LocalTime t);

std::string to_json(const ScheduleRule& rule);
std::optional<ScheduleRule> schedule_from_json(const std::string& json);

}  // namespace tdm::scheduler
