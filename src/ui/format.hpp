#pragma once
// Human-readable formatting for the UI. Pure functions (tested).

#include <QString>

#include <cstdint>
#include <optional>

#include "db/store.hpp"

namespace tdm::ui {

QString formatBytes(std::uint64_t bytes);                 // "1.50 MB" (decimal units, like the speed)
QString formatSpeed(double bytes_per_sec);                 // "2.40 MB/s", "" when 0
QString formatEta(std::optional<std::uint64_t> total, std::uint64_t downloaded, double bytes_per_sec);  // "1:05", "2:03:04", ""
QString stateName(db::State s);
QString fromUtf8(const std::string& s);
std::string toUtf8(const QString& s);

// Parses "512K", "2M", "1.5 MB", "300" (bytes/s) → bytes/s; nullopt on error; 0 = unlimited.
std::optional<std::uint64_t> parseRate(const QString& text);

bool looksLikeUrl(const QString& text);    // http(s)://... with a host
bool isSha256Hex(const QString& text);     // 64 hex characters

}  // namespace tdm::ui
