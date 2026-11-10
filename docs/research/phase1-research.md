# Tesla Download Manager: Phase 1 research

Date: 2026-11-10. Status: Phase 1 is done. Phase 2 (architecture) is blocked until the toolchain is installed (see the end of this doc).

Tesla Download Manager (TDM) is an independent Windows download manager written in C++20, Qt 6, Asio, SQLite and JSON. It is *inspired by* the behaviour users know from IDM, but it is not a copy. This document records:

1. what we took from the reference list `awesome-reverse-engineer-anything` and why;
2. what we observed, and how we observed it, staying within the rules below;
3. the design decisions that follow from it.

## Ground rules (clean-room boundary)

- **No disassembly or decompilation of IDM. No reading its binaries, its resources or its registry state. No contact with its licensing.** We look only at what a server on the other end of the wire can see, plus public documentation.
- No IDM code, assets, strings or UI artwork go into TDM.
- The reference repo is read-only reference material. Nothing was built inside it.
- Every claim below carries one of three labels, following the evidence discipline in the reference list: **Proven** (measured here, with an evidence file), **Inferred** (a reasonable reading of proven data) or **Public** (from public HTTP standards or docs, not observed).

## What we used from the reference repo, and why

The reference list (`D:\Umar\awesome-reverse-engineer-anything`, README plus `docs/what-is-rea.md`, `docs/is-it-rea.md`, `docs/by-app.md` and `docs/rea-vs-craft-anything.md`) catalogues AI-assisted reverse-engineering cases and tools. Most of it is about binaries (Ghidra, IDA, Hopper, JADX and the REA MCP server). We are deliberately **not** using that layer. The parts that do fit a network-level study of a desktop app:

| From the list | What it is | How TDM uses it |
| --- | --- | --- |
| **Clean-room / black-box stance**: Craft Anything's default "clean room" mode (reads only what a visitor can see) and WordCraft's AGENTS.md ban on disassembling Word (`docs/rea-vs-craft-anything.md`, README "wider wave") | A rule about which evidence is allowed | It is our boundary: we observe IDM only as a remote server would see it. That keeps us clear of IDM's licence terms and of the clean-room criticism in the README's "Legal and controversy" section. |
| **Wire-capture as the source of truth**: feature-heist rebuilt an npm search client from a **HAR capture**; OpenGHub was reverse-engineered from **captures** | Learn a client's protocol behaviour from its traffic, not its code | We built our own capture point, a local logging HTTP server (`tools/testserver/probe_server.py`). It needs no Wireshark or TLS interception, and it only sees traffic sent to us. |
| **Evidence discipline**: the wispr-flow teardown marks each claim *proven / inferred / hidden*; rea-casebook requires conclusions to cite real evidence ids; feature-heist **hash-pins** stored evidence | Every finding can be checked | Labels in this doc, plus a dated evidence file with its SHA-256 recorded below. |
| **Differential testing against the original**: feature-heist (each rebuild checked against the original), DX-Ball (3,205 test cases), zx-reaper (each stage tested against the last) | Acceptance = behaviour matches on the same inputs | Phase 8 benchmarks run TDM and the reference behaviour against the *same* probe server scenarios and compare request patterns, completion time and SHA-256. We publish only real numbers (the README's "date every number" rule). |
| **Targeted questions beat open exploration**: the rea-lab result (482 runs; REA gave no gain on open-ended work, ~2× cost) and leanrea (a plain-CLI skill matched REA at lower cost) | Method choice | We did not install REA, Ghidra or IDA. We asked narrow questions ("how does a segmented downloader split ranges?") that a few probe runs can answer. |
| **Sandboxing untrusted analysis**: rea-safe-agents, agent-sandbox | Isolation | The probe server binds to 127.0.0.1 only, writes into the project folder, and the downloaded test file went to a scratch folder. |

Not used: REA itself, Ghidra, IDA, Hopper, JADX, Frida and x64dbg. All of them look inside the target, which this project rules out.

## Probe setup

- `tools/testserver/probe_server.py` (stdlib Python, 127.0.0.1:8765). It serves deterministic synthetic files: the bytes depend on name and version, so the SHA-256 can be reproduced. It supports Range/206, If-Range, ETag/Last-Modified, 416, a no-Range mode, chunked transfer, redirect chains, 503 + Retry-After and per-connection throttling. It also has a "bump version" endpoint, which simulates a file changing on the server mid-download. Every request is logged as a JSON line with its connection id (client port).
- Self-test with curl (Proven, 2026-11-10): HEAD headers correct; `Range: bytes=100-199` → 206 with `Content-Range: bytes 100-199/1000000`; a wrong `If-Range` → full 200; 3-hop redirect followed; the first GET gets 503 `Retry-After: 3` and the second gets 200; a version bump changes the SHA-256; two Range halves reassemble to the full-file SHA-256.

## Observation: IDM against the probe server

One run on 2026-11-10. IDM, already installed on this machine, was handed one URL through its documented command line (`/d URL /p dir /f name /n`). File: 32 MiB (33,554,432 bytes), throttled to 1.5 MB/s per connection so the segmentation shows. Evidence file: `docs/research/evidence/idm-probe-2026-11-10.json` (9 requests), SHA-256 `9756919f87d47fc6d74b127938febc40f471cc2e1e0595b680e3d20c9691c660`.

| t (s) | conn | request | result |
| --- | --- | --- | --- |
| 24.15 | 53420 | GET, **no Range** | 200; client closed after ~240 KB |
| 26.80–26.87 | 8 new conns | GET `Range: bytes=X-…` | 206 ×8 |

Ranges requested (sorted): `233600-EOF`, `4447624-16905827`, `8600337-16905827`, `12753050-16905827`, `16905764-EOF`, `21151112-25230160`, `25230097-EOF`, `29401087-EOF`.

Findings:

1. **Proven:** no HEAD request. The first request is a plain GET without Range. The client reads the response headers (size, `Accept-Ranges`), keeps the first bytes, then closes that connection once segments take over.
2. **Proven:** after a ~2.6 s pause, 8 Range connections open within 70 ms. Total 9 TCP connections for one file.
3. **Proven:** ranges are mostly **open-ended or reach to the end of a parent range** (`X-EOF`, `X-16905827`) and they overlap. Connections whose range overlapped the next segment were closed by the client once they reached it (`aborted` in the log). **Inferred:** a dynamic "split the largest remaining range in half" scheme. A new connection takes the back half of the biggest unfinished piece, and the front connection stops at the new boundary. No fixed segment table is negotiated in advance.
4. **Proven:** start offsets are not aligned to any power of two (e.g. 233600, 4447624). **Inferred:** split points are computed from live progress at split time.
5. **Proven:** bytes received on the probe GET are reused. The first Range segment starts at 233,600, which is inside the 245,760 bytes already sent on that connection.
6. **Proven:** headers sent are `Accept: */*`, `Accept-Encoding: identity` (no compression, so offsets stay byte-exact) and a browser-like `User-Agent`. No `If-Range` or `If-Match`.
7. **Proven:** the final file's SHA-256 (`acd6661a…99d4eb`) equals the server's full-body SHA-256.
8. **Proven:** with 8 × 1.5 MB/s, the 32 MiB transfer took ~2.8 s after the pause, which is consistent with full parallel use of the per-connection cap.

Not observed yet, left for Phase 8 against TDM itself: behaviour on no-Range servers, chunked responses, 503/Retry-After, a mid-download version change and redirects. We stopped after one IDM run, and further IDM runs are not planned unless the user asks for them.

## Design decisions for TDM that follow

| # | Decision | Basis |
| --- | --- | --- |
| D1 | Start with a **GET without Range** (not HEAD), read the headers, keep the body as segment 0, and only then open extra connections. Fall back to HEAD only if GET is refused. | Finding 1 and 5. It saves one round trip, and some servers lie on HEAD. |
| D2 | **Dynamic bisection scheduler**: each connection owns `[pos, end)`. When a slot is free and the largest remaining piece is above a minimum (default 1 MiB, adaptive to throughput × ~2 s), split it at the midpoint of what remains. The new connection requests `bytes=mid-end`, and the old one's `end` shrinks to `mid`. Request open-ended ranges and **stop reading** at the boundary rather than re-requesting. | Findings 3–4. This gives natural work-stealing: fast connections take over slow ones' work without a fixed table. |
| D3 | **Safer than observed: send `If-Range: <ETag>` (or Last-Modified) on every Range request**, and check that every 206 `Content-Range` total and validator matches the first response. On any mismatch (200 instead of 206, a new ETag or a different size), discard all segments and restart from zero. Never mix bytes of two versions. | Finding 6 (no If-Range observed) plus the spec's "never mix segments from different versions" rule, and RFC 9110 §13.1.5. |
| D4 | `Accept-Encoding: identity` on all transfer connections. | Finding 6. Byte offsets must refer to the stored representation. |
| D5 | Default 8 connections per file, user-tunable 1–32, with a per-host cap and connection reuse (keep-alive pool) across segments and files. | Finding 2. |
| D6 | No `Accept-Ranges` / 200 to a Range request / chunked → **single-stream mode**. Resume is impossible then unless validators exist and a later probe shows Range support. | Public (RFC 9110). To be verified with the probe server. |
| D7 | 503/429 with `Retry-After` (seconds or HTTP-date): back off for that long per host, reduce concurrency for the host, then retry. Otherwise use exponential backoff with jitter. | Spec. The probe server already emulates this. |
| D8 | Streaming writes: each connection writes into a preallocated sparse file at its offset through a bounded async write queue. Persist segment progress to SQLite every ≤1 s or every 4 MiB. | Spec plus resume needs. |
| D9 | After completion, compute SHA-256 (streaming) and compare it with the expected hash if one was given. Always store it. | Finding 7 method. Spec. |
| D10 | Do **not** copy IDM's identity: TDM sends its own `User-Agent: TeslaDM/<ver>`, with an optional per-site override. | Clean-room rule. |

## Toolchain check (2026-11-10)

| Needed | Found |
| --- | --- |
| C++ compiler (MSVC 2022 or LLVM/clang) | **Missing**: no `cl`, `clang++`, `g++` or Visual Studio installation |
| CMake ≥ 3.25 and Ninja | **Missing** |
| Qt 6 (msvc2022_64) | **Missing**: no `C:\Qt` |
| vcpkg (for Asio, SQLite3, nlohmann-json) | **Missing** |
| Git | `C:\Program Files\Git\cmd\git.exe` (not on PATH) |
| Python 3 | 3.15.2, used for the test server |
| curl, tar | Windows built-ins |
| Installer tool (Inno Setup or NSIS) | Missing; needed only in Phase 9 |
| Disk | C: 162 GB free, D: 551 GB free |

### Installed later on 2026-11-10 (verified)

| Component | Version / location |
| --- | --- |
| VS 2022 Build Tools, VC tools workload | MSVC 14.44 (`_MSC_VER` 1944), `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools` |
| CMake | 4.4.4, `C:\Program Files\CMake\bin` |
| Ninja | 1.13.2 (winget), plus the copy bundled with the Build Tools |
| vcpkg | `C:\vcpkg` (full history, so manifest baselines work), tool 2026-10-29 |
| Qt | 6.10.2 msvc2022_64 at `C:\Qt\6.10.2\msvc2022_64`, installed with aqtinstall 3.3.0. aqt can't read the per-arch repo layout used by Qt 6.11+. aqtinstall runs from the venv `C:\Qt\.aqt-venv` on Python 3.14, because 3.15 has no wheels for its dependencies. |

Smoke test: Qt 6.10.2, Asio 1.38.2, SQLite 3.54.1 and nlohmann-json 3.12.0 configured, built with MSVC + Ninja, and ran. The test showed that Asio defaults to `_WIN32_WINNT=0x0601`, so the project must define `_WIN32_WINNT=0x0A00`.

To build from PowerShell, first load the MSVC environment: run `vcvars64.bat && set` through `cmd /d /c` and copy the variables in. Running `.cmd` files directly from the scratch folder failed in this environment.
