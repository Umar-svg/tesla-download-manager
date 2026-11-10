# Tesla Download Manager

An independent, high-performance download manager for Windows. It uses C++20, Qt 6, Asio, SQLite and JSON.

Its own architecture and code: no third-party download-manager code, assets or licensing workarounds.

## Status

- Phase 1, research: done. See [docs/research/phase1-research.md](docs/research/phase1-research.md).
- Phase 2, architecture: next. It waits on the toolchain (MSVC, CMake, Qt 6 and vcpkg).

## Layout

- `docs/research/`: research notes and dated, hash-pinned evidence
- `tools/testserver/probe_server.py`: local HTTP test server. It supports Range, validators, redirects, Retry-After, throttling and no-Range or chunked modes, and logs every request.

Run the test server:

    python tools/testserver/probe_server.py --port 8765 --log probe.jsonl
