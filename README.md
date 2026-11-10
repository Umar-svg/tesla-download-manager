# Tesla Download Manager

An independent, high-performance download manager for Windows, built with C++20, Qt 6, Asio, OpenSSL and SQLite. It uses its own architecture and code: no third-party download-manager code, assets or licensing workarounds.

## Features
- Segmented downloads with dynamic splitting across up to 32 connections. Each segment is validated with `If-Range` and Content-Range, so bytes from two versions of a file are never mixed.
- Crash-safe resume: progress is checkpointed only after the data is flushed to disk; SHA-256 is verified on completion.
- Retries with backoff that honour `Retry-After`; fallback to a single stream for servers without Range support.
- Global and per-download speed limits, queues with per-queue limits, and daily schedules.
- Qt UI (`TeslaDM.exe`) plus a Qt-free command-line tool (`tdm-cli`).

## Build (Windows)
Requires Visual Studio 2022 Build Tools, CMake ≥ 3.25, Ninja, vcpkg (`C:\vcpkg`) and Qt 6 (`C:\Qt\6.10.2\msvc2022_64`).

    .\scripts\build.ps1                    # Release build + all tests
    .\scripts\build.ps1 -Preset msvc-debug
    .\scripts\package.ps1                  # installer: build\msvc-release\installer\TeslaDM-<ver>-setup.exe (needs Inno Setup 6)

Tests: `ctest --test-dir build/msvc-release -L engine` (core, needs Python for the local test server) and `-L ui` (offscreen Qt).

## Docs
- `PROJECT_STATUS.md`: current state, decisions, verified results
- `docs/architecture.md`: design
- `docs/research/phase1-research.md`: black-box research
- `docs/benchmarks/`: measured results vs curl
