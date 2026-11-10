#!/usr/bin/env python3
"""Local HTTP test/probe server for the download manager project.

Black-box observation tool: serves deterministic synthetic files and logs
every request (method, headers, client port = connection id, bytes sent,
timings) as JSON lines. It never touches the client program itself.

Behaviours, selected by query string on /f/<name>:
  size=<bytes>        file size (default 64 MiB)
  norange=1           ignore Range, always 200 full body, no Accept-Ranges
  rate=<bytes/s>      per-connection throttle (makes segmentation visible)
  rate_jitter=1       each request gets a random rate in [rate/4, rate]
  latency_ms=<ms>     wait this long before answering (simulated RTT)
  ctype=<type>        Content-Type to send (default application/octet-stream)
  nodisp=1            omit Content-Disposition (name then comes from the URL)
  chunked=1           Transfer-Encoding: chunked, no Content-Length
  busy=<n>            first n GETs for this name get 503 + Retry-After
  retry_after=<s>     value used with busy (default 2)
  validators=0        omit ETag / Last-Modified
Other endpoints:
  /redirect/<n>/f/... 302 chain of length n ending at /f/...
  /admin/bump/<name>  change the "version" of a file (new ETag, new bytes)
  /admin/log          return the request log collected so far
  /admin/sha256/<name>?size=N[&start=a&end=b]   hex SHA-256 of the file or of [a, b)
  /admin/shutdown     stop the server (used by the C++ integration tests)
Options: --port 0 picks a free port (printed on the first stdout line);
--tls-cert/--tls-key serve HTTPS instead of HTTP.

Only binds to 127.0.0.1. Stdlib only.
"""
import argparse
import email.utils
import hashlib
import json
import random
import re
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

BLOCK = 1 << 16
_lock = threading.Lock()
_versions = {}      # name -> int
_busy_count = {}    # name -> GETs seen (for busy=)
_log = []
_log_file = None
_server = None
_start = time.time()


def version_of(name):
    with _lock:
        return _versions.setdefault(name, 1)


def block_for(name, ver):
    seed = hashlib.sha256(f"{name}:{ver}".encode()).digest()
    out = bytearray()
    ctr = 0
    while len(out) < BLOCK:
        out += hashlib.sha256(seed + ctr.to_bytes(8, "little")).digest()
        ctr += 1
    return bytes(out[:BLOCK])


_block_cache = {}


def content(name, ver, start, end):
    """Yield bytes [start, end] inclusive of the synthetic file."""
    key = (name, ver)
    blk = _block_cache.get(key)
    if blk is None:
        blk = _block_cache[key] = block_for(name, ver)
    pos = start
    while pos <= end:
        off = pos % BLOCK
        n = min(BLOCK - off, end - pos + 1)
        yield blk[off:off + n]
        pos += n


def record(entry):
    entry["t"] = round(time.time() - _start, 4)
    with _lock:
        _log.append(entry)
        if _log_file:
            _log_file.write(json.dumps(entry) + "\n")
            _log_file.flush()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "ProbeServer/1.0"

    def log_message(self, fmt, *args):  # quiet default stderr log
        pass

    def _base_entry(self):
        return {
            "conn": self.client_address[1],
            "method": self.command,
            "path": self.path,
            "http": self.request_version,
            "headers": {k: v for k, v in self.headers.items()},
        }

    def do_HEAD(self):
        self._handle(head=True)

    def do_GET(self):
        self._handle(head=False)

    def _send_simple(self, code, body=b"", headers=None, entry=None):
        self.send_response(code)
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)
        if entry is not None:
            entry.update(status=code, sent=len(body))
            record(entry)

    def _handle(self, head):
        entry = self._base_entry()
        url = urlsplit(self.path)
        q = {k: v[-1] for k, v in parse_qs(url.query).items()}
        path = url.path

        if path == "/admin/log":
            with _lock:
                body = json.dumps(_log).encode()
            return self._send_simple(200, body, {"Content-Type": "application/json"})
        if path == "/admin/shutdown":
            self._send_simple(200, b"bye\n")
            threading.Thread(target=_server.shutdown, daemon=True).start()
            return
        m = re.fullmatch(r"/admin/sha256/([\w.\-]+)", path)
        if m:
            name = m.group(1)
            size = int(q.get("size", 64 << 20))
            lo = int(q.get("start", 0))            # optional sub-range [start, end)
            hi = min(int(q.get("end", size)), size)
            h = hashlib.sha256()
            for piece in content(name, version_of(name), lo, hi - 1):
                h.update(piece)
            return self._send_simple(200, h.hexdigest().encode())
        m = re.fullmatch(r"/admin/bump/([\w.\-]+)", path)
        if m:
            with _lock:
                _versions[m.group(1)] = _versions.get(m.group(1), 1) + 1
                v = _versions[m.group(1)]
            return self._send_simple(200, f"version={v}\n".encode(), entry=entry)
        m = re.fullmatch(r"/redirect/(\d+)(/.*)", path)
        if m:
            n = int(m.group(1))
            rest = m.group(2) + (("?" + url.query) if url.query else "")
            loc = rest if n <= 1 else f"/redirect/{n-1}{rest}"
            return self._send_simple(302, b"", {"Location": loc}, entry)
        m = re.fullmatch(r"/f/([\w.\-]+)", path)
        if not m:
            return self._send_simple(404, b"not found\n", entry=entry)

        name = m.group(1)
        size = int(q.get("size", 64 << 20))
        ver = version_of(name)
        validators = q.get("validators", "1") != "0"
        norange = q.get("norange") == "1"
        chunked = q.get("chunked") == "1"
        rate = int(q.get("rate", 0))
        latency = int(q.get("latency_ms", 0))
        if latency:
            time.sleep(latency / 1000.0)  # simulated round trip before the response starts
        if rate and q.get("rate_jitter") == "1":
            rate = int(rate * random.uniform(0.25, 1.0))  # uneven connections finish at different times
        entry["rate"] = rate

        if not head and "busy" in q:
            with _lock:
                seen = _busy_count[name] = _busy_count.get(name, 0) + 1
            if seen <= int(q["busy"]):
                return self._send_simple(503, b"busy\n",
                                         {"Retry-After": q.get("retry_after", "2")}, entry)

        etag = '"%s"' % hashlib.sha256(f"{name}:{ver}:{size}".encode()).hexdigest()[:16]
        lastmod = email.utils.formatdate(1_700_000_000 + ver * 3600, usegmt=True)

        start, end, status = 0, size - 1, 200
        rng = self.headers.get("Range")
        ifr = self.headers.get("If-Range")
        if rng and not norange and not chunked:
            honour = True
            if ifr is not None and ifr not in (etag, lastmod):
                honour = False  # validator mismatch -> full 200 body
            rm = re.fullmatch(r"bytes=(\d*)-(\d*)", rng.strip())
            if honour and rm:
                a, b = rm.group(1), rm.group(2)
                if a == "":
                    start, end = max(0, size - int(b)), size - 1
                else:
                    start = int(a)
                    end = min(int(b), size - 1) if b else size - 1
                if start >= size or start > end:
                    return self._send_simple(416, b"", {"Content-Range": f"bytes */{size}"}, entry)
                status = 206

        self.send_response(status)
        self.send_header("Content-Type", q.get("ctype", "application/octet-stream"))
        if q.get("nodisp") != "1":
            self.send_header("Content-Disposition", f'attachment; filename="{name}"')
        if validators:
            self.send_header("ETag", etag)
            self.send_header("Last-Modified", lastmod)
        if not norange and not chunked:
            self.send_header("Accept-Ranges", "bytes")
        if status == 206:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        if chunked:
            self.send_header("Transfer-Encoding", "chunked")
        else:
            self.send_header("Content-Length", str(end - start + 1))
        self.end_headers()
        entry.update(status=status, range=[start, end], version=ver)

        sent = 0
        t0 = time.time()
        try:
            if not head:
                for piece in content(name, ver, start, end):
                    # throttle in 16 KiB steps when rate is set
                    for i in range(0, len(piece), 16384):
                        part = piece[i:i + 16384]
                        if chunked:
                            self.wfile.write(b"%x\r\n" % len(part) + part + b"\r\n")
                        else:
                            self.wfile.write(part)
                        sent += len(part)
                        if rate:
                            ahead = sent / rate - (time.time() - t0)
                            if ahead > 0:
                                time.sleep(ahead)
                if chunked:
                    self.wfile.write(b"0\r\n\r\n")
        except (ConnectionResetError, ConnectionAbortedError, BrokenPipeError):
            entry["aborted"] = True
            self.close_connection = True
        entry.update(sent=sent, secs=round(time.time() - t0, 3))
        record(entry)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--log", default=None, help="append JSON lines here")
    ap.add_argument("--tls-cert", default=None)
    ap.add_argument("--tls-key", default=None)
    a = ap.parse_args()
    global _log_file, _server
    if a.log:
        _log_file = open(a.log, "a", encoding="utf-8")
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    srv.daemon_threads = True
    scheme = "http"
    if a.tls_cert:
        import ssl
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(a.tls_cert, a.tls_key)
        srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
        scheme = "https"
    _server = srv
    print(f"probe server on {scheme}://127.0.0.1:{srv.server_address[1]}", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    sys.exit(main())
