# PROJECT_STATUS: Tesla Download Manager

Updated: 2026-11-10

## Current phase
Phase 9 (Windows installer) is IN PROGRESS. The installer is built and its install/run/uninstall cycle is verified. One robustness fix (finalize retry on sharing violations) is written but its verification run was interrupted: re-run the tests first (see "Phase 9 status").

## Completed
- Phase 1: research doc `docs/research/phase1-research.md` (black-box IDM probe, decisions D1–D10); probe server `tools/testserver/probe_server.py`
- Toolchain: MSVC 14.44, CMake 4.4.4, Ninja, vcpkg `C:\vcpkg`, Qt 6.10.2 `C:\Qt\6.10.2\msvc2022_64`
- Phase 2: `docs/architecture.md`; CMake/vcpkg skeleton (`CMakePresets.json` msvc-debug/msvc-release, `scripts/build.ps1`)
  - `tdm_core`: Content-Range, HTTP-date (3 formats), Retry-After, Content-Disposition (RFC 6266/8187), filename sanitising, URL parse/resolve
  - `tdm_retry`: `RetryPolicy`; `tdm_db`: SQLite `Store` schema v1 (WAL)
  - Interface headers: `engine/download_engine.hpp`, `scheduler/scheduler.hpp`; Qt shell `TeslaDM.exe` (actions disabled)
- Phase 3 `tdm_net`:
  - `response_parser`: incremental head parser and `ChunkedDecoder`
  - `tls_context`: TLS ≥1.2, Windows ROOT store, host-name verification, SNI
  - `ConnectionManager`: keep-alive pool, per-host cap + waiters, idle TTL, stats
  - `HttpClient`: streaming fetch, redirects, timeouts, cancel, stale keep-alive retry
- Phase 4:
  - `tdm_fs` `fs/file_writer.{hpp,cpp}`:
    - sparse preallocated `.tdmpart` (FSCTL_SET_SPARSE + EndOfFile), or growing when the size is unknown
    - one writer thread doing positional `WriteFile` with OVERLAPPED offsets
    - 64 MiB byte budget with `request_room()` back-pressure (wakes at half the budget)
    - sticky error; `flush`/`close` (FlushFileBuffers); `finalize` = MoveFileExW without overwrite + " (n)" suffix; `discard`
    - resume reopen requires the existing file at the exact size; `unique_path()`; streaming `sha256_file()` (EVP)
  - `net::Request::read_gate`: pauses socket reads until resumed; the idle timer is suspended while paused, and cancel works while paused
  - `SegmentPlanner::position()`
  - `tdm_multipart` `TransferJob` (`multipart/transfer_job.{hpp,cpp}`):
    - probe GET (D1); name from Content-Disposition → URL → sanitised; free-space check
    - segmented mode: the probe becomes segment 0, and up to N connections take bisection splits; open-ended Range + If-Range (strong ETag, else Last-Modified)
    - every 206 validated (Content-Range start/total, ETag, Last-Modified); 200/416/mismatch → cancel all, discard, re-probe (≤ `max_restarts`)
    - segment retry through `RetryPolicy` (Retry-After; 429/503 halve the connection count)
    - single-stream fallback (chunked / no ranges / unknown size)
    - stop() → flushed `ResumeState` (segments + validators), only when validators exist
    - resume validates the part file (falls back to a fresh start)
    - finalize on its own thread: rename, size check, SHA-256, optional expected hash
    - `TransferError` category
- Phase 5:
  - **Bug fix, `FileWriter::flush()`:**
    - Root cause: it waited for an *empty* queue, which never happens while connections keep writing, so a live flush could block indefinitely.
    - Fix: per-item sequence numbers. `flush()` now waits only for items enqueued before the call (FIFO), then runs FlushFileBuffers without holding the queue lock.
    - `close()` now drains the writer thread first, then flushes and closes.
    - A `shared_mutex` keeps `flush()` from racing `close()`/`discard()` on the handle, and the handle is atomic.
    - Proven by `FileWriter.FlushDoesNotStarveUnderContinuousWrites`: with the old predicate temporarily restored it fails (13.2 s and 19.1 s, "flush starved"); with the fix it passes in about 0.3 s.
  - `TransferJob::checkpoint()`: captures the segment table under the job lock, then flushes the shared writer without that lock; dropped if a restart happened meanwhile. Also added `TransferResult::disk_error` and `TransferProgress::file_name`.
  - DB schema v2 (migration v1→v2 in place): `directory`, `file_name`, `part_path`, `connections`.
  - `tdm_engine` `engine/engine.{hpp,cpp}` (`Engine` implements `DownloadEngine`):
    - io_context + N threads, connection pool, HttpClient, Store (`db_mu_`, lock order mu_ → db_mu_)
    - `max_active` scheduling by priority then id
    - states Queued/Downloading/Waiting/Paused/Completed/Failed/Cancelled
    - whole-download retry via `RetryPolicy`: 503/429/5xx honour Retry-After; timeouts and network errors back off; disk errors, hash/size mismatch, TLS and 4xx → Failed
    - pause, start, cancel (removes the `.tdmpart`; `delete_file` also removes a completed file)
    - maintenance thread: progress + smoothed speed every `snapshot_interval`, durable checkpoint every `checkpoint_interval` (saved only if the same job is still running)
    - startup reload: interrupted downloads → Queued (or Paused when `resume_on_start=false`), resume state rebuilt from rows + segments
    - graceful shutdown waits for every job and saves resume state

- Phase 6:
  - `scheduler::TokenBucket`: thread-safe; reserve-before-read + refund, consume/wait_time for unreserved bytes; live `set_rate` (lowering takes effect immediately)
  - limits are enforced through `net::Request::read_gate` + new `read_limit` (caps the size of the next read):
    - each connection holds one reserved quantum and tops up only the shortfall
    - quantum = rate / (16 × connections), clamped to 16–256 KiB
    - `TransferOptions::limiters` (the engine passes global + per-download)
    - the HttpClient resume now re-checks the gate
  - `queue::select_to_start`: priority, then FIFO; per-queue `max_active`, global cap, closed schedule windows
  - `scheduler::ScheduleRule` (start/stop minute, weekday mask, overnight windows belong to the opening day) + JSON; `to_local()`
  - DB schema v3: `downloads.speed_limit`; queue CRUD (`add/update/list/remove_queue`; queue 1 is permanent; removing a queue moves its downloads to Main)
  - Engine API: `set_global_speed_limit` (persisted in settings), `set_speed_limit`, `set_priority`, `move_to_queue`, `add_queue`, `set_queue_limit`, `set_queue_schedule` (persisted as JSON), `queues()`
    - per-queue limits replace the old single limit; `EngineOptions::max_active` (default 8) is now a global cap
    - the maintenance thread stops downloads whose window closed (back to Queued, progress saved) and starts newly allowed ones
    - injectable clock `EngineOptions::now`

## Phase 6 defects found and fixed (with evidence)
1. **Bursty rate limiting** (`EngineTest.GlobalLimitAppliesLiveAndPersists` measured 1.04 MB/s against a 2 MB/s limit).
   - Root cause: pay-after-read pacing synchronised all connections. They waited on the same debt, then each read a full 256 KiB at once (8 × 256 KiB ≈ 2 MiB steps every ~0.8 s; timeline captured with a temporary diagnostic test, since removed). The long-run average was right but the short-window speed swung between 0 and 2×.
   - Fix: reserve-before-read in small quanta with a capped read size, so reservations stagger the connections.
2. **Under-delivery after fix 1** (94–97%, engine 84–94%).
   - Root cause: at every read, unused reservations were refunded and a full new quantum reserved, losing the refunded bytes' time slot.
   - Fix: keep the unused reservation and reserve only the shortfall. Engine steady state became 99.3–100.7%.
3. **Tail latency** (`PerDownloadLimitHoldsWithinTenPercent` failed 4/4: 12 MB took 4.47 s at 3 MB/s).
   - Root cause: N connections × one quantum reserved ahead = 1.5 MB ≈ 0.5 s that the last connections wait behind at the end of a download.
   - Fix: quantum scaled by 1/N, so the backlog is about 1/16 s. Result: 96.9–97.8% end to end, including probe and finalize.

- Phase 7 (UI):
  - Engine additions (plain C++, no Qt): `Snapshot` gains url, file_path, queue_id, priority, speed_limit, `removed`; `remove(id, delete_file)` (stops a running job first, deletes the `.tdmpart`, notifies observers with `removed=true`); `set_max_active()`; `snapshots()` on the interface.
  - `tdm_ui_core` (Qt6::Core only):
    - `format` (bytes, speed, ETA, rate parsing, URL/SHA-256 validation)
    - `DownloadTableModel` (id index, display/progress/sort/state roles)
    - `EngineBridge`
  - `tdm_ui_widgets`:
    - `MainWindow`: sortable table, toolbar/menu/context actions (add, resume, pause, cancel, remove, keep or delete files, open, open folder), segment map, status bar with totals, tray icon with completion notifications
    - `AddDownloadDialog` (clipboard URL prefill, validation), `SettingsDialog` (QSettings plus engine limits), `QueuesDialog` (per-queue limits and schedule editor)
    - `ProgressDelegate`, `SegmentBar`
  - `TeslaDM.exe`: data in `%LOCALAPPDATA%\TeslaDM\TeslaDM\tdm.sqlite`; `TeslaDM.exe <url>` opens the Add dialog; a startup failure (e.g. DB locked) shows a message box
  - windeployqt runs post-build for `TeslaDM` and `tdm_ui_tests`

## UI architecture rules (Phase 7)
- The engine and every core library stay Qt-free. Verified: grep over `src/{core,net,fs,multipart,db,engine,queue,scheduler,retry}` finds no Qt include or type.
- The UI talks to the engine only through `EngineBridge` → public `DownloadEngine` API. No engine internals or locks are reachable from Qt code.
- **Event coalescing:** engine observers (engine threads) store the latest snapshot per id in a mutex-protected map, and at most one queued `flush()` is ever in flight. The flush runs on the UI thread and emits one `snapshotsReady(QList<Snapshot>)` batch. Measured: while the UI thread was blocked for 600 ms, 38–40 engine snapshots became 1 UI event (3 runs).
- **Lifetime/shutdown:** `~EngineBridge` runs `engine.shutdown()` (joins all engine threads) before any member is destroyed, so no observer can touch a dead bridge. A flush still queued is discarded by Qt because the bridge is its context object. `main()` creates the bridge before `MainWindow` and destroys the window first, then the bridge.
- Commands from the UI (add/pause/remove...) are synchronous engine calls (they only take the engine lock briefly and do small DB writes). Shutdown on exit can block for as long as a running finalize/hash takes.

- Phase 8 (benchmarks):
  - `tdm-cli` (`src/cli/tdm_cli.cpp`, Qt-free): `tdm-cli get [-o DIR] [-c N] [--max-active N] [--limit B/s] [--db FILE] URL...` runs the real Engine and prints JSON (seconds, time to last byte, bytes, observed connections, per-file SHA-256 from the engine)
  - probe server `latency_ms=` (delay before every response)
  - `tools/bench/run_bench.py`: Tesla DM vs `curl.exe` on identical URLs, REPS runs each, SHA-256 of every file verified against the server and re-hashed from disk, hardware info; writes `docs/benchmarks/<date>.{json,md}`. `--quick` is the ctest `bench_quick` (labels bench, engine)
  - curl baseline uses `--parallel --parallel-immediate` for multi-file runs (without `-immediate`, curl waits to multiplex and ran at about half speed)
  - Results: `docs/benchmarks/2026-11-10.md` (generated) and `docs/benchmarks/analysis-2026-11-10.md` (interpretation)

## Phase 8 findings
- Per-connection-capped server (4 MB/s/connection, 64 MiB): 1/2/4/8/16 connections = 0.99/1.97/3.89/7.60/13.58× curl. With 100 ms latency, 8 connections = 6.98×.
- **Defect fixed:** the engine's pool capped each host at 8 connections (`PoolOptions::max_per_host` default), so 16 configured connections gave 8; `active_connections` also counted queued fetches.
  - Fix: engine pool default 32; active = connections with a response head.
  - Regression test `EngineTest.SixteenConnectionsAreReallyUsed` passes (0.83 s, 16 active) and fails with the old cap (1.59 s, 8 active).
- **Slower than curl (honest):** unthrottled 1 GiB on loopback.
  - curl takes 1.84 s.
  - Tesla DM with 8 connections receives the last byte at 1.68 s but completes at 3.14 s: finalize = FlushFileBuffers of 1 GiB + rename + SHA-256 re-read.
  - With 1 connection, the receive path itself is slower (last byte at 2.83 s).
- **Small files:** 100 × 1 MiB took 7.10 s vs curl's 6.73 s (~15 ms per-file overhead).
- Follow-ups are listed in the analysis doc: zero-copy writer hand-off, hashing while downloading, optional durability flush, profiling the 1-connection path.
- Tooling incident: a PowerShell 5.1 read/write-back (used to temporarily revert a line for the regression proof) double-encoded the non-ASCII arrows in comments in `src/engine/engine.hpp`. It was detected by a scan and repaired; all sources were re-scanned clean. Rule: patch files only with Python and explicit UTF-8.

## Phase 9 status (2026-11-10)
Done and verified:
- Single-instance guard (`ui/instance_guard.*`, QLocalServer per user; a second launch forwards its URL argument). Tests `UiInstanceGuard.*` pass; a second launch of the staged app exited 0 and handed over.
- Install rules (`cmake --install`): `bin/` with TeslaDM.exe, tdm-cli.exe, Qt DLLs and plugins (windeployqt via `qt_generate_deploy_app_script`), vcpkg DLLs, MSVC runtime (InstallRequiredSystemLibraries); `THIRD_PARTY_NOTICES.txt`; `licenses/` (asio, nlohmann-json, openssl, sqlite3 from vcpkg).
- Packaging defects found and fixed:
  - OpenSSL DLLs were missing: FindOpenSSL targets carry no DLL location, so `$<TARGET_RUNTIME_DLLS>` skipped them. They are now installed explicitly, and configure fails if they are not found.
  - Windows' own `icuuc.dll` was being redistributed (byte-identical to System32); it is now removed.
  - ~40 MB of software-GL and shader-compiler fallbacks were dropped. The install shrank from 75.5 MB to 42 MB.
- `installer/check_deps.py`: dumpbin-based check that every import of every shipped binary resolves on a clean PC. It caught both defects above on the old tree; it now passes (348 imports, 29 binaries).
- The staged app runs with PATH = System32 only: GUI opens and exits 0; tdm-cli passes the SHA-verified `--quick` benchmark.
- `installer/tesla-dm.iss.in` (Inno Setup 6.7.3): per-user install to `%LOCALAPPDATA%\Programs\TeslaDM`, Start menu entries, optional desktop icon; the uninstaller keeps user data on silent uninstall and asks otherwise. `scripts/package.ps1` = build → stage → dependency check → ISCC → `TeslaDM-0.1.0-setup.exe` (13.5 MB).
- Installer cycle verified:
  - silent install exit 0 (38 files, shortcuts, HKCU uninstall key)
  - the installed GUI and CLI ran with a minimal PATH (SHA OK)
  - silent uninstall exit 0 removed the folder, shortcuts and key; user data was kept

Found while running the full matrix, fixed, NOT YET RE-VERIFIED (the run was interrupted):
1. **Product defect:** `FileWriter::finalize` failed at once with ERROR_SHARING_VIOLATION when another handle without FILE_SHARE_DELETE (antivirus, indexer, previewer) had the part file open. A complete download became a "disk error".
   - Regression test `FileWriterSharing.FinalizeWaitsForAnotherReaderToLetGo` reproduces it on the old code ("being used by another process").
   - Fix: retry the rename with backoff (25 ms up to 400 ms steps, about 10.7 s in total) on sharing, access-denied or lock violations.
2. **Test defect:** `TransferJobTest` helper requests (server hashes) shared the job's connection pool (cap 8). A running job starved them until the download had finished, which also made `LiveCheckpointIsAValidCrashRecoveryPoint` fail in the Release matrix (111/112).
   - Fix: the helpers use their own pool.
   - A pacing workaround tried first was based on a wrong diagnosis and was removed.

Not done:
- LGPL-3.0/GPL-3.0 license texts for Qt: gnu.org was unreachable from this network (connect timeouts); `installer/licenses/` is empty and the notices give the official URLs.
- Full Release/Debug matrix after the two fixes above.
- The project's own license (owner's decision).

## Remaining
- Phase 9: re-verify (run `FileWriterSharing*`, `*LiveCheckpoint*` ×5, then `ctest -L engine` and `-L ui` in Release and Debug), add the LGPL/GPL texts, rebuild the installer

## Exact next steps
1. `.\scripts\build.ps1` then `build\msvc-release\tests\tdm_tests.exe --gtest_filter=FileWriterSharing*:*LiveCheckpoint* --gtest_repeat=5`.
2. `ctest --test-dir build/msvc-release -L engine` and `-L ui`; the same for `build/msvc-debug`.
3. Put `LGPL-3.0.txt` and `GPL-3.0.txt` (from gnu.org) into `installer/licenses/`.
4. `.\scripts\package.ps1` and repeat the silent install/run/uninstall check.

## Key decisions
- Engine is Qt-free; UI talks to `engine::DownloadEngine` only.
- Probe with a GET without Range; reuse its body as segment 0 (D1).
- Open-ended `Range: bytes=N-` + `If-Range` on every segment request; any validator, size or 200-vs-206 mismatch → discard and restart (D3). Verified: a mid-download version bump always ends in a complete new-version file (6/6 runs, restarts=1).
- Resume only when ETag or Last-Modified exists.
- Planner commit and disk enqueue happen under one lock, so progress snapshots never claim bytes that are not queued; ResumeState is captured only after flush.
- `Accept-Encoding: identity`; own UA `TeslaDM/<ver>`.
- Crash safety: the DB only ever holds checkpoints whose bytes are flushed. Bytes written after the last checkpoint are simply downloaded again on resume (identical data, guarded by If-Range). Verified by `LiveCheckpointIsAValidCrashRecoveryPoint` (resume from an older checkpoint → exact SHA-256).
- Engine observers run on the maintenance thread; never call `shutdown()` from an observer (it joins that thread). The UI never does: it only reacts to the bridge's queued signal.
- Bandwidth: limits are charged per received body byte (headers are not counted). A limit change applies on each connection's next read.

## Known issues / limitations
- Build env: `build.ps1` uses `$ErrorActionPreference=Continue` (PowerShell 5.1 turns native stderr into errors); `.cmd` files can't run directly here; aqtinstall 3.3.0 can't install Qt ≥ 6.11; `TeslaDM.exe` needs Qt DLLs on PATH until Phase 9.
- net: stale keep-alive retry not integration-tested; queued pool waiters have no timeout; redirect bodies not drained; no proxy, HTTP/2 or DNS cache; Content-Encoding not decoded.
- multipart:
  - min split is fixed (`TransferOptions::min_split`); adaptive sizing from throughput is still to do
  - finalize runs on a detached thread (the job keeps itself alive)
  - single-stream downloads cannot resume (restart from zero)
  - segments use the post-redirect URL, so expiring signed redirect URLs are not refreshed
- The probe server does TLS handshakes on its accept thread (test tool only).
- engine:
  - DB writes are synchronous under `db_mu_` (fast with WAL; no dedicated DB thread as the architecture originally said)
  - DB errors throw out of API calls; inside job callbacks they would terminate
  - snapshots are pushed for running downloads each tick, plus on every state change
  - no `remove(id)` API yet (rows stay in the DB)
  - a real process crash is not tested end to end (simulated with an older checkpoint instead)
- bench: client and server share one machine (loopback, Python server); not yet measured against real internet servers or IDM.
- ui:
  - no single-instance guard: a second `TeslaDM.exe` fails to open the same DB cleanly only if SQLite reports busy, otherwise both run against one DB (to fix in Phase 9 with a named mutex / local socket)
  - closing the window quits (no minimise-to-tray option yet)
  - no browser integration beyond the `TeslaDM.exe <url>` argument
  - strings use `tr()` but no translations ship
  - icons are Qt standard icons
- scheduler:
  - windows are checked every `snapshot_interval` (default 250 ms)
  - a lowered queue limit does not stop running downloads (they finish; no new starts)
  - per-queue speed limits are not implemented (global + per-download only)
  - very low limits (< 16 KiB/s × connections) are rounded up per read to 16 KiB, which is coarse but stays accurate on average

## Concurrent file writing: verification (2026-11-10)
Tests added (`tests/transfer_job_test.cpp`):
- `LedgerWriter` decorator (injected through `TransferOptions::writer_factory`). It records every write range when the job hands it over and counts any overlap with earlier writes. The ledger is shared across stop and resume.
- `SegmentWatch` checks every 20 ms progress snapshot: contiguous disjoint coverage, `begin ≤ pos ≤ end`, positions never move backwards, and per-segment completion times.
- Probe server: `rate_jitter=1` (each connection gets a random rate in [rate/4, rate]) and `/admin/sha256?start&end` (hash of a byte range).
- `Large1GiBEightUnevenSegmentsWriteExactlyOnce`: 0 overlaps, exact [0, 1 GiB) coverage, bytes_received == size, 8 segments, 8 different finish moments, SHA-256 equals the server's.
- `Large1GiBStopResumePreservesValidProgress`: stop at about 40%; every claimed range [begin, pos) of the part file is hashed against the server's range hash (8 segments, all partial); resume fetches only the rest (first + second == size, restarts 0, no overlap across runs), and the final SHA-256 matches.

Failure report received ("ledger / 1 GiB tests failing"): **not reproducible.** No failing output was supplied, and the terminal panel shows no test run. Evidence:
- `tdm_tests --gtest_filter=*Large1GiB* --gtest_repeat=3` (Release): 6/6 OK, full log `build/large_release.log`
- `ctest` Release 79/79 (`build/ctest_release.log`); `ctest` Debug 79/79 (`build/ctest_debug.log`); each 1 GiB test takes about 10–12 s, under the 120 s ctest timeout

Code review of the ordering (no defect found):
- `on_body` enqueues the write and commits the planner under `mu_`, so snapshots never claim unqueued bytes
- the stop snapshot is taken only when no connections remain and after `close()` (flush + FlushFileBuffers)
- a restart increments `gen_` before `discard()`; old-generation sinks are rejected; `discard()` clears the queue and joins the writer thread

Environment needs for the 1 GiB tests: ≥ 2 GiB free in `%TEMP%` (each test deletes its directory on success; a failing ASSERT leaves at most 1 GiB, which the next run's `fresh_dir` removes).

## Build / test
`.\scripts\build.ps1 [-Preset msvc-debug] [-NoTest]`.
Last result (2026-11-10, Phase 8), engine and UI run separately by ctest label:
| | engine (`ctest -L engine`, incl. `bench_quick`) | UI (`ctest -L ui`, offscreen) |
|---|---|---|
| Release | **112/112**, 99 s (`build/ctest_release_engine.log`) | **14/14**, 5.9 s (`build/ctest_release_ui.log`) |
| Debug | **112/112**, 97 s (`build/ctest_debug_engine.log`) | **14/14**, 6.3 s (`build/ctest_debug_ui.log`) |

Builds have 0 compiler warnings at /W4 (Phase 8 builds checked again). In Phase 7 the UI suite also passed 3× with `--gtest_repeat=3` (`build/ui_repeat.log`). The real `TeslaDM.exe` (Windows platform, not offscreen) opened its main window and exited cleanly with code 0 on WM_CLOSE.
- Integration tests need Python (`find_package(Python3)`); they start the probe server through CreateProcess on port 0.
- UI tests: `tdm_ui_tests` (own `main` with `QApplication`, `QT_QPA_PLATFORM=offscreen`, offscreen plugin loaded from `TDM_QT_PLUGIN_DIR`, QSettings isolated under "TeslaDM-tests"). The end-to-end tests use the real Engine + probe server: add → progress → completed (SHA-256 checked, painted offscreen), pause/resume through the toolbar actions, coalescing under a busy UI thread, shutdown mid-download then resume in a new session, and Add-dialog validation.
- Run one side only: `ctest --test-dir build/msvc-release -L engine` or `-L ui`.
- MSVC: avoid raw string literals containing `\"` inside gtest macros (C2017).
