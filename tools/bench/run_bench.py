#!/usr/bin/env python3
"""Benchmark runner: Tesla Download Manager (tdm-cli) vs curl.exe.

Starts the local probe server, runs each scenario REPS times with both tools
against identical URLs, verifies every downloaded file's SHA-256 against the
server, and writes raw JSON plus a Markdown report. Every number in the report
is a measurement from this run; nothing is estimated.

  python run_bench.py --cli <path to tdm-cli.exe> [--reps 3] [--quick] [--out DIR]

--quick runs tiny scenarios (used as a ctest smoke test of the CLI).
"""
import argparse
import datetime
import hashlib
import json
import os
import platform
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = os.path.join(HERE, "..", "testserver", "probe_server.py")
MiB = 1 << 20


def start_server():
    p = subprocess.Popen([sys.executable, "-I", SERVER, "--port", "0"],
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    line = p.stdout.readline()
    port = int(line.rsplit(":", 1)[1])
    return p, f"http://127.0.0.1:{port}"


def server_sha(base, name, size):
    with urllib.request.urlopen(f"{base}/admin/sha256/{name}?size={size}") as r:
        return r.read().decode()


def file_sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def run_tdm(cli, urls, out_dir, connections, max_active):
    cmd = [cli, "get", "-o", out_dir, "-c", str(connections), "--max-active", str(max_active)] + urls
    t0 = time.perf_counter()
    res = subprocess.run(cmd, capture_output=True, text=True, timeout=3600)
    wall = time.perf_counter() - t0
    if res.returncode != 0:
        raise RuntimeError(f"tdm-cli failed ({res.returncode}): {res.stderr.strip()} {res.stdout[:300]}")
    data = json.loads(res.stdout)
    return {"seconds": data["seconds"], "process_seconds": wall, "total_bytes": data["total_bytes"],
            "bytes_done_seconds": data.get("bytes_done_seconds"),
            "max_connections": data["max_connections"],
            "files": [(f["path"], f["sha256"]) for f in data["files"]]}


def run_curl(urls, out_dir, parallel):
    files = [os.path.join(out_dir, f"curl_{i}.bin") for i in range(len(urls))]
    cmd = ["curl.exe", "-s", "-S", "--fail"]
    if parallel > 1 and len(urls) > 1:
        # --parallel-immediate: open connections right away instead of waiting
        # to multiplex over one (HTTP/2 only), the fair HTTP/1.1 baseline.
        cmd += ["--parallel", "--parallel-immediate", "--parallel-max", str(parallel)]
    for u, f in zip(urls, files):
        cmd += ["-o", f, u]
    t0 = time.perf_counter()
    res = subprocess.run(cmd, capture_output=True, text=True, timeout=3600)
    secs = time.perf_counter() - t0
    if res.returncode != 0:
        raise RuntimeError(f"curl failed ({res.returncode}): {res.stderr.strip()}")
    total = sum(os.path.getsize(f) for f in files)
    return {"seconds": secs, "process_seconds": secs, "total_bytes": total, "bytes_done_seconds": None,
            "max_connections": min(parallel, len(urls)),
            "files": [(f, None) for f in files]}


def verify(base, run, names_sizes):
    # names_sizes: list of (server file name, size) in URL order
    if len(run["files"]) != len(names_sizes):
        raise RuntimeError("file count mismatch")
    for (path, reported), (name, size) in zip(run["files"], names_sizes):
        expect = server_sha(base, name, size)
        on_disk = file_sha(path)  # always re-hash independently from disk
        if on_disk != expect:
            raise RuntimeError(f"SHA-256 mismatch for {name}: {on_disk} != {expect}")
        if reported is not None and reported != expect:  # tdm also reports the engine's own hash
            raise RuntimeError(f"engine-reported SHA-256 mismatch for {name}")


def scenarios(quick):
    if quick:
        return [
            {"id": "quick-capped", "desc": "4 MiB, 2 MB/s per connection", "files": [("q1.bin", 4 * MiB)],
             "query": "rate=2000000", "tdm_conns": [1, 4], "curl": True, "max_active": 1},
            {"id": "quick-multi", "desc": "4 x 1 MiB, 2 MB/s per connection", "files": [(f"qm{i}.bin", MiB) for i in range(4)],
             "query": "rate=2000000", "tdm_conns": [4], "curl": True, "max_active": 4},
        ]
    return [
        {"id": "capped-64MiB", "desc": "64 MiB, server caps each connection at 4 MB/s",
         "files": [("cap.bin", 64 * MiB)], "query": "rate=4000000", "tdm_conns": [1, 2, 4, 8, 16], "curl": True,
         "max_active": 1},
        {"id": "capped-latency-64MiB", "desc": "64 MiB, 4 MB/s per connection, 100 ms before every response",
         "files": [("lat.bin", 64 * MiB)], "query": "rate=4000000&latency_ms=100", "tdm_conns": [1, 8], "curl": True,
         "max_active": 1},
        {"id": "unthrottled-1GiB", "desc": "1 GiB, no server throttle (loopback; Python server is the ceiling)",
         "files": [("big.bin", 1024 * MiB)], "query": "", "tdm_conns": [1, 8], "curl": True, "max_active": 1},
        {"id": "many-small", "desc": "100 files x 1 MiB, 4 MB/s per connection, 4 at a time",
         "files": [(f"s{i:03d}.bin", MiB) for i in range(100)], "query": "rate=4000000", "tdm_conns": [8],
         "curl": True, "max_active": 4},
    ]


def hw_info():
    info = {"os": platform.platform(), "python": platform.python_version(), "logical_cpus": os.cpu_count()}
    try:
        ps = ("$c=Get-CimInstance Win32_Processor | Select-Object -First 1;"
              "$m=(Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory;"
              "$d=Get-PhysicalDisk | Select-Object -First 1 FriendlyName,MediaType;"
              "\"$($c.Name)|$($c.NumberOfCores)|$m|$($d.FriendlyName)|$($d.MediaType)\"")
        out = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True,
                             timeout=60).stdout.strip().split("|")
        info.update({"cpu": out[0].strip(), "cores": int(out[1]), "ram_gib": round(int(out[2]) / (1 << 30), 1),
                     "disk": f"{out[3]} ({out[4]})"})
    except Exception as e:  # informational only
        info["hw_error"] = str(e)
    try:
        info["curl"] = subprocess.run(["curl.exe", "--version"], capture_output=True, text=True).stdout.splitlines()[0]
    except Exception:
        pass
    return info


def fmt_rate(bps):
    return f"{bps / 1e6:.1f} MB/s"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", required=True)
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--out", default=os.path.join(HERE, "..", "..", "docs", "benchmarks"))
    a = ap.parse_args()

    srv, base = start_server()
    work = tempfile.mkdtemp(prefix="tdm_bench_")
    results = []
    try:
        for sc in scenarios(a.quick):
            urls = [f"{base}/f/{n}?size={s}" + (f"&{sc['query']}" if sc["query"] else "") for n, s in sc["files"]]
            variants = [("tdm", c) for c in sc["tdm_conns"]] + ([("curl", 1)] if sc["curl"] else [])
            for tool, conns in variants:
                runs = []
                for rep in range(a.reps):
                    d = os.path.join(work, f"{sc['id']}_{tool}{conns}_{rep}")
                    os.makedirs(d)
                    if tool == "tdm":
                        r = run_tdm(os.path.abspath(a.cli), urls, d, conns, sc["max_active"])
                    else:
                        r = run_curl(urls, d, sc["max_active"])
                    verify(base, r, sc["files"])
                    runs.append({"seconds": r["seconds"], "process_seconds": r["process_seconds"],
                                 "bytes_done_seconds": r["bytes_done_seconds"],
                                 "bytes": r["total_bytes"], "max_connections": r["max_connections"]})
                    shutil.rmtree(d, ignore_errors=True)
                    print(f"{sc['id']:24} {tool:4} c={conns:<2} rep{rep}: {r['seconds']:.2f}s "
                          f"{fmt_rate(r['total_bytes'] / r['seconds'])} sha OK", flush=True)
                secs = [x["seconds"] for x in runs]
                bd = [x["bytes_done_seconds"] for x in runs if x["bytes_done_seconds"] is not None]
                results.append({"scenario": sc["id"], "desc": sc["desc"], "tool": tool, "connections": conns,
                                "max_active": sc["max_active"], "bytes": runs[0]["bytes"], "runs": runs,
                                "median_s": statistics.median(secs), "min_s": min(secs), "max_s": max(secs),
                                "median_bytes_done_s": statistics.median(bd) if bd else None,
                                "observed_connections": max(x["max_connections"] for x in runs)})
    finally:
        srv.terminate()
        shutil.rmtree(work, ignore_errors=True)

    if a.quick:
        print("quick benchmark OK:", len(results), "variants, all SHA-256 verified")
        return 0

    os.makedirs(a.out, exist_ok=True)
    stamp = datetime.date.today().isoformat()
    meta = {"date": stamp, "reps": a.reps, "hardware": hw_info(),
            "setup": "client and Python probe server on the same machine over loopback (127.0.0.1)"}
    with open(os.path.join(a.out, f"{stamp}.json"), "w", encoding="utf-8") as f:
        json.dump({"meta": meta, "results": results}, f, indent=1)

    lines = [f"# Benchmarks {stamp}", "",
             "Generated by `tools/bench/run_bench.py`. Every row is measured; every download's SHA-256 was "
             "verified against the server (and, for Tesla DM, re-hashed independently from disk).", "",
             "## Setup", ""]
    for k, v in meta["hardware"].items():
        lines.append(f"- {k}: {v}")
    lines += [f"- {meta['setup']}", f"- repetitions per variant: {a.reps} (median shown; min/max in brackets)", "",
              "Tesla DM times run from adding the URLs until every download is Completed, so they include the "
              "engine's own finalize step (rename + SHA-256 of the file). curl times are the whole process run.", ""]
    by_sc = {}
    for r in results:
        by_sc.setdefault(r["scenario"], []).append(r)
    for sc_id, rows in by_sc.items():
        lines += [f"## {sc_id}", "", rows[0]["desc"], "",
                  "| Tool | Connections (observed) | Median time | Throughput (median) | Min-max time | "
                  "Last byte at (median) | Speed-up vs curl |",
                  "|---|---|---|---|---|---|---|"]
        curl = next((r for r in rows if r["tool"] == "curl"), None)
        for r in rows:
            label = "Tesla DM" if r["tool"] == "tdm" else "curl.exe"
            conns = (f"{r['connections']} per file ({r['observed_connections']})" if r["tool"] == "tdm"
                     else "1 per file")
            if r["max_active"] > 1:
                conns += f", {r['max_active']} files at a time"
            speed = r["bytes"] / r["median_s"]
            up = f"{curl['median_s'] / r['median_s']:.2f}x" if curl else ""
            last = f"{r['median_bytes_done_s']:.2f} s" if r["median_bytes_done_s"] is not None else "-"
            lines.append(f"| {label} | {conns} | {r['median_s']:.2f} s | {fmt_rate(speed)} | "
                         f"{r['min_s']:.2f}-{r['max_s']:.2f} s | {last} | {up} |")
        lines.append("")
    with open(os.path.join(a.out, f"{stamp}.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print("wrote", os.path.join(a.out, f"{stamp}.md"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
