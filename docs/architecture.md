# Tesla Download Manager: architecture

Status: Phase 2, 2026-11-10. This is our own design. It is based on the black-box findings in [research/phase1-research.md](research/phase1-research.md) (decisions D1–D10) and on public HTTP standards (RFC 9110/9111/9112).

## Goals, in priority order

1. **Correct bytes.** We never write bytes from two versions of a file into one output. Every finished file gets a SHA-256, which is checked whenever an expected hash is known.
2. **Throughput.** Async network I/O, dynamic Range segmentation, connection reuse, streaming writes and no per-byte copies on the hot path.
3. **Survive interruption.** Crashes, reboots and network loss resume from the persisted segment state, or restart cleanly when the validators say the file changed.
4. **Behave well toward servers.** Per-host connection caps, `Retry-After`, backoff with jitter, and our own User-Agent.

## Process and threads

```
┌──────────────── UI thread (Qt event loop) ────────────────┐
│  tdm_ui: MainWindow, models, dialogs                       │
│      ▲ queued Qt signals (snapshots, ~4 Hz)    │ commands  │
└──────┼─────────────────────────────────────────┼──────────┘
       │                                         ▼
┌──────┴──────────── Engine strand(s) on asio::io_context ───┐
│ tdm_engine ── tdm_queue ── tdm_scheduler ── tdm_retry      │
│     │                                                       │
│ tdm_multipart (SegmentPlanner per download)                 │
│     │                                                       │
│ tdm_net: HttpClient ── ConnectionManager (pool, host caps)  │
│        N network threads run io_context (default = cores/2) │
└─────┬───────────────────────────────────────────────────────┘
      │ buffers (pooled 256 KiB)            │ state rows
      ▼                                     ▼
┌─ tdm_fs writer thread ─┐        ┌─ tdm_db thread ─────────┐
│ positional writes into │        │ SQLite WAL, batched      │
│ sparse preallocated    │        │ commits ≤1 s / 4 MiB     │
│ <name>.tdmpart         │        └──────────────────────────┘
└────────────────────────┘
```

- The engine core (`tdm_core` … `tdm_engine`) has **no Qt dependency**. It can be tested headless, and a CLI can be added later.
- Each download's state is changed only on its own `asio::strand`, so no locks are needed on the hot path.
- Disk writes are off the network threads. If the write queue grows past its limit (default 64 MiB), sockets stop reading, so a slow disk never piles up unbounded memory.

## Modules (CMake targets)

| # | Module | Target | Responsibility | Phase |
|---|---|---|---|---|
| 0 | Core | `tdm_core` | Shared types, URL / header parsing (`Content-Range`, `Retry-After`, `Content-Disposition`), filename sanitising | 2 ✅ |
| 1 | HTTP client | `tdm_net` | Asio HTTP/1.1 over TCP and TLS (OpenSSL, system cert store), streaming body callbacks, chunked decoding, redirects (≤10, absolute and relative), timeouts | 3 ✅ |
| 2 | Connection manager | `tdm_net` | Keep-alive pool keyed by scheme+host+port, per-host cap (net default 8, engine default 32) with waiter queue, idle TTL (DNS caching left to the OS) | 3 ✅ |
| 3 | Multipart controller | `tdm_multipart` | `SegmentPlanner`: dynamic bisection (D2), adaptive min split, work stealing, persisted segment table | 2 ✅ (planner), 4 ✅ (TransferJob) |
| 4 | File manager | `tdm_fs` | `.tdmpart` preallocation (sparse), positional async writes, fsync policy, atomic rename on completion, streaming SHA-256, free-space check | 4 ✅ |
| 5 | Engine | `tdm_engine` | Per-download state machine: probe (D1) → validate → segment → write → verify → finalise | 5 ✅ |
| 6 | Retry and recovery | `tdm_retry` | Failure classification, exponential backoff + jitter, `Retry-After`, per-host cool-down, resume validation (D3) | 2 ✅ (policy), 5 (wiring) |
| 7 | DB | `tdm_db` | SQLite (WAL) store: downloads, segments, queues, settings (JSON); schema migrations | 2 ✅ (schema v1) |
| 8 | Queue | `tdm_queue` | Named queues, priorities, max concurrent downloads per queue | 6 ✅ |
| 9 | Scheduler | `tdm_scheduler` | Start/stop times, global and per-download token-bucket bandwidth limits | 6 ✅ |
| 10 | UI | `tdm_ui_core`, `tdm_ui_widgets`, `TeslaDM` (exe) | Qt Widgets: list, add-URL dialog, progress / segment map, settings, queues/scheduler, tray; `EngineBridge` coalesces engine snapshots into one queued batch signal | 7 ✅ |

## Download lifecycle

```
Queued → Probing → (Segmented | SingleStream) → Verifying → Completed
              ↘ Waiting(retry at T) ↗           ↘ Failed / Paused / Cancelled
```

1. **Probe (D1).** A GET without `Range`, with `Accept-Encoding: identity`. Store the final URL after redirects, the status, `Content-Length`, `Accept-Ranges`, `ETag`, `Last-Modified` and `Content-Disposition`. The probe's body continues as segment 0, so no round trip is wasted.
2. **Choose a mode.** Use segmented mode when the length is known, there is no `Transfer-Encoding: chunked`, and `Accept-Ranges: bytes` is present (or a later Range request returns a valid 206). Otherwise use single-stream mode (D6).
3. **Segment (D2).** `SegmentPlanner` hands out work. Each new connection sends `Range: bytes=<begin>-` (open-ended) plus **`If-Range: <strong ETag | Last-Modified>`** (D3), and reads until the planner's current `end` for that segment.
4. **Validate every 206.**
   - `Content-Range` must start where we asked and its total must equal the probe's total.
   - Any `ETag` or `Last-Modified` must equal the stored values.
   - A 200 in reply to a Range request means the representation changed, or the server ignores Range.
   - On any mismatch: stop all connections, discard the `.tdmpart` and the segment table, and restart from the probe. There is no attempt to merge versions.
5. **Resume after restart.** Reload the segments from the DB and send one Range request with `If-Range`. A 206 that passes validation resumes; anything else restarts from zero.
6. **Finish.** Flush, fsync, compute and verify the SHA-256, then do an atomic `MoveFileEx` from `.tdmpart` to the final name (with a collision suffix like ` (1)`).

## Adaptive behaviour

- **Min split size** = max(1 MiB, smoothed per-connection throughput × 2 s). Small files never fan out to more connections than they can use.
- **Connections per file:** start at the configured value (default 8). If extra connections don't raise total throughput by ≥5 % over 3 s, stop adding them. A 429/503 halves the host cap.
- **Bandwidth limit:** a token bucket per download plus a global one. Each connection reserves a small quantum (rate / (16 × connections)) before every read and caps the read to it, so connections are staggered and the aggregate is smooth. Network threads pause *reads*, not writes, so TCP flow control pushes back on the server.

## Persistence (schema v1)

- `downloads(id, url, final_url, file_path, total_size, etag, last_modified, accept_ranges, sha256_expected, sha256_actual, state, error, queue_id, priority, created_at, updated_at)`
- `segments(download_id, idx, begin, pos, end)`, primary key `(download_id, idx)`. This table is replaced as a whole inside one transaction.
- `queues(id, name, max_active)`
- `settings(key, json)`
- `PRAGMA user_version` drives migrations.

## Error policy (`tdm_retry`)

| Failure | Retry? | Delay |
|---|---|---|
| network reset, timeout, DNS failure | yes | backoff |
| 408, 425, 429, 500, 502, 503, 504 | yes | `Retry-After` if present (capped at 1 h), else backoff |
| other 4xx, 501, 505 | no | — |
| validator mismatch | restart from zero (counts as an attempt) | 0 |
| disk full / access denied | no (paused, user action) | — |

Backoff: `min(cap, base·2^attempt) × U[1−jitter, 1]`, with base 500 ms, cap 60 s, jitter 0.5 and at most 8 attempts per segment.

## Phase plan

1. Research ✅
2. Architecture, skeleton, core utilities, planner, retry policy, DB schema ✅
3. HTTP client and connection manager on Asio, including TLS ✅
4. File manager and multipart driver ✅
5. Engine state machine, recovery and validation (with probe-server scenarios) ✅
6. Queue, scheduler and bandwidth limits ✅
7. Qt UI ✅
8. Test-server scenarios and real benchmarks, compared with curl (and with IDM only if the user agrees) ✅ (see docs/benchmarks/)
9. Windows installer (Inno Setup or NSIS) with `windeployqt` ✅ (Inno Setup, per-user; `scripts/package.ps1`)

## Build

```powershell
.\scripts\build.ps1                # configures the msvc-release preset, builds, runs the tests
.\scripts\build.ps1 -Preset msvc-debug
```

You need the toolchain from the research doc: MSVC 2022, CMake ≥ 3.25, Ninja, vcpkg at `C:\vcpkg` (or set `VCPKG_ROOT`), and Qt 6 at `C:\Qt\6.10.2\msvc2022_64` (or set `TDM_QT_DIR`).
