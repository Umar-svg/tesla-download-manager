// tdm-cli: command-line front end on the real (Qt-free) Engine.
//
//   tdm-cli get [-o DIR] [-c CONNECTIONS] [--max-active N] [--limit BYTES_PER_S]
//               [--db FILE] [--timeout SECONDS] [--allow-web-page] URL [URL...]
//
// Downloads every URL through the same Engine the GUI uses (probe, segments,
// If-Range validation, checkpoints, SHA-256) and prints one JSON object:
//   {"seconds":..., "total_bytes":..., "bytes_per_sec":..., "max_connections":...,
//    "files":[{"url","state","path","bytes","sha256","error"}...]}
// Exit code: 0 when every download completed, 1 otherwise, 2 on usage errors.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "core/version.hpp"
#include "db/store.hpp"
#include "engine/engine.hpp"

namespace {

using namespace tdm;
namespace stdfs = std::filesystem;

int usage() {
    std::fprintf(stderr,
                 "usage: tdm-cli get [-o DIR] [-c CONNECTIONS] [--max-active N] [--limit BYTES_PER_S]\n"
                 "                   [--db FILE] [--timeout SECONDS] [--allow-web-page] URL [URL...]\n");
    return 2;
}

std::string utf8(const stdfs::path& p) {
    const auto u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

bool finished(db::State s) {
    return s == db::State::Completed || s == db::State::Failed || s == db::State::Cancelled;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty() || args[0] != "get") return usage();

    stdfs::path out_dir = stdfs::current_path();
    stdfs::path db_path;
    int connections = 8, max_active = 8, timeout_s = 3600;
    bool allow_web_page = false;
    std::uint64_t limit = 0;
    std::vector<std::string> urls;
    try {
        for (std::size_t i = 1; i < args.size(); ++i) {
            const auto& a = args[i];
            auto next = [&]() -> const std::string& {
                if (i + 1 >= args.size()) throw std::invalid_argument("missing value for " + a);
                return args[++i];
            };
            if (a == "-o") out_dir = stdfs::path(std::u8string(reinterpret_cast<const char8_t*>(next().c_str())));
            else if (a == "-c") connections = std::stoi(next());
            else if (a == "--max-active") max_active = std::stoi(next());
            else if (a == "--limit") limit = std::stoull(next());
            else if (a == "--db") db_path = next();
            else if (a == "--timeout") timeout_s = std::stoi(next());
            else if (a == "--allow-web-page") allow_web_page = true;
            else if (!a.empty() && a[0] == '-') throw std::invalid_argument("unknown option " + a);
            else urls.push_back(a);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "tdm-cli: %s\n", e.what());
        return usage();
    }
    if (urls.empty()) return usage();

    std::error_code ec;
    stdfs::create_directories(out_dir, ec);
    const bool temp_db = db_path.empty();
    if (temp_db) {
        db_path = stdfs::temp_directory_path() /
                  ("tdm-cli-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".sqlite");
    }

    engine::EngineOptions opts;
    opts.db_path = db_path;
    opts.max_active = max_active;
    opts.snapshot_interval = std::chrono::milliseconds(50);
    std::vector<engine::DownloadId> ids;
    int max_conns = 0;
    double seconds = 0;
    double bytes_done_seconds = -1;  // first moment every download had all its bytes (50 ms granularity)
    try {
        engine::Engine e(opts);
        if (limit) e.set_global_speed_limit(limit);
        // Queue limit for the main queue follows --max-active too.
        e.set_queue_limit(1, max_active);
        const auto t0 = std::chrono::steady_clock::now();
        for (const auto& u : urls) {
            engine::AddRequest r;
            r.url = u;
            r.directory = out_dir;
            r.connections = connections;
            r.allow_web_page = allow_web_page;
            ids.push_back(e.add(r));
        }
        const auto deadline = t0 + std::chrono::seconds(timeout_s);
        for (;;) {
            bool all = true, all_bytes = true;
            int conns = 0;
            for (auto id : ids) {
                const auto s = e.snapshot(id);
                if (!s || !finished(s->state)) all = false;
                if (!s || !(s->state == db::State::Completed || (s->total && s->downloaded >= *s->total)))
                    all_bytes = false;
                if (s) conns += s->active_connections;
            }
            max_conns = std::max(max_conns, conns);
            if (all_bytes && bytes_done_seconds < 0)
                bytes_done_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (all) break;
            if (std::chrono::steady_clock::now() > deadline) {
                std::fprintf(stderr, "tdm-cli: timeout\n");
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        e.shutdown();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "tdm-cli: %s\n", ex.what());
        return 1;
    }

    // Read the final rows (SHA-256 computed by the engine during finalize).
    nlohmann::json out;
    std::uint64_t total = 0;
    bool ok = true;
    {
        db::Store st(utf8(db_path));
        for (auto id : ids) {
            const auto row = st.get_download(id);
            nlohmann::json f;
            f["url"] = row ? row->url : "";
            f["state"] = row ? static_cast<int>(row->state) : -1;
            f["path"] = row ? row->file_path : "";
            f["bytes"] = row && row->total_size ? *row->total_size : 0;
            f["sha256"] = row ? row->sha256_actual : "";
            f["error"] = row ? row->error : "missing";
            if (!row || row->state != db::State::Completed) ok = false;
            else total += row->total_size.value_or(0);
            out["files"].push_back(f);
        }
    }
    if (temp_db)
        for (const char* ext : {"", "-wal", "-shm"}) stdfs::remove(utf8(db_path) + ext, ec);

    out["tool"] = std::string("TeslaDM/") + kVersion;
    out["seconds"] = seconds;
    out["bytes_done_seconds"] = bytes_done_seconds < 0 ? seconds : bytes_done_seconds;
    out["total_bytes"] = total;
    out["bytes_per_sec"] = seconds > 0 ? static_cast<double>(total) / seconds : 0.0;
    out["max_connections"] = max_conns;
    out["connections_setting"] = connections;
    std::cout << out.dump() << std::endl;
    return ok ? 0 : 1;
}
