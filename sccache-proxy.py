#!/usr/bin/env python3
"""
Local S3 cache proxy for sccache.

Read-through, write-back proxy that:
- On GET: serves from local cache; on miss, fetches from upstream S3 and caches locally
- On PUT: stores to local cache only (for local fork builds)
- On HEAD: checks local cache, then upstream
- Health check endpoint at /

sccache uses OpenDAL which, with a custom SCCACHE_ENDPOINT, uses path-style S3 URLs:
  GET /bucket/prefix/a/b/c/hash
  PUT /bucket/prefix/a/b/c/hash

Usage:
  python3 sccache-proxy.py [--port 8083] [--cache-dir ./cache/sccache] [--max-size 80]

Configure sccache to use this proxy:
  SCCACHE_ENDPOINT=http://localhost:8083
  SCCACHE_BUCKET=clickhouse-builds
  SCCACHE_S3_KEY_PREFIX=ccache/sccache
  SCCACHE_REGION=us-east-1
  AWS_ACCESS_KEY_ID=local
  AWS_SECRET_ACCESS_KEY=local
"""

import argparse
import hashlib
import http.server
import json
import logging
import os
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("sccache-proxy")

UPSTREAM_HOST = "s3.us-east-1.amazonaws.com"
STATS_LOCK = threading.Lock()
STATS = {
    "local_hits": 0,
    "upstream_hits": 0,
    "upstream_dedup": 0,
    "misses": 0,
    "puts": 0,
    "errors": 0,
    "bytes_served": 0,
    "bytes_fetched": 0,
    "bytes_stored": 0,
}


class InflightTracker:
    """Deduplicates concurrent upstream fetches for the same key.

    When multiple threads request the same missing key, only the first
    thread fetches from upstream. Others wait on a threading.Event and
    reuse the result.
    """

    def __init__(self):
        self._lock = threading.Lock()
        # key -> (Event, result_slot)
        # result_slot is a single-element list: [bytes | None]
        self._inflight: dict[str, tuple[threading.Event, list]] = {}

    def acquire(self, key: str) -> tuple[bool, threading.Event, list]:
        """Try to become the fetcher for `key`.

        Returns (is_fetcher, event, result_slot).
        - If is_fetcher is True, caller must fetch and then call release().
        - If is_fetcher is False, caller should wait on event and read result_slot[0].
        """
        with self._lock:
            if key in self._inflight:
                event, slot = self._inflight[key]
                return False, event, slot
            event = threading.Event()
            slot = [None]  # mutable slot for the result
            self._inflight[key] = (event, slot)
            return True, event, slot

    def release(self, key: str, data: bytes | None) -> None:
        """Store the fetch result and wake all waiters."""
        with self._lock:
            if key in self._inflight:
                event, slot = self._inflight[key]
                slot[0] = data
                event.set()
                del self._inflight[key]


class CacheManager:
    """Manages the local cache directory with optional size-based eviction."""

    def __init__(self, cache_dir: Path, max_size_gb: float):
        self.cache_dir = cache_dir
        self.max_size_bytes = int(max_size_gb * 1024**3)
        self.cache_dir.mkdir(parents=True, exist_ok=True)
        self._eviction_lock = threading.Lock()
        log.info("Cache dir: %s, max size: %.1f GB", self.cache_dir, max_size_gb)

    def local_path(self, key: str) -> Path:
        """Convert S3 key to local filesystem path."""
        # key is like: clickhouse-builds/ccache/sccache/a/b/c/hash
        return self.cache_dir / key

    def get(self, key: str) -> bytes | None:
        """Read from local cache. Returns None on miss."""
        path = self.local_path(key)
        if path.is_file():
            try:
                data = path.read_bytes()
                # Touch atime for LRU eviction
                os.utime(path, (time.time(), path.stat().st_mtime))
                return data
            except OSError:
                return None
        return None

    def put(self, key: str, data: bytes) -> None:
        """Write to local cache atomically."""
        path = self.local_path(key)
        path.parent.mkdir(parents=True, exist_ok=True)
        # Atomic write: write to temp file, then rename
        fd, tmp = tempfile.mkstemp(dir=path.parent, prefix=".tmp_")
        try:
            os.write(fd, data)
            os.close(fd)
            os.rename(tmp, path)
        except OSError:
            os.close(fd)
            try:
                os.unlink(tmp)
            except OSError:
                pass
            raise

    def exists(self, key: str) -> tuple[bool, int]:
        """Check if key exists locally. Returns (exists, size)."""
        path = self.local_path(key)
        if path.is_file():
            try:
                st = path.stat()
                return True, st.st_size
            except OSError:
                pass
        return False, 0

    def evict_if_needed(self) -> None:
        """LRU eviction if cache exceeds max size. Run in background."""
        if not self._eviction_lock.acquire(blocking=False):
            return  # Another thread is already evicting
        try:
            self._do_evict()
        finally:
            self._eviction_lock.release()

    def _do_evict(self) -> None:
        total = 0
        files = []
        for dirpath, _, filenames in os.walk(self.cache_dir):
            for fn in filenames:
                if fn.startswith(".tmp_"):
                    continue
                fp = os.path.join(dirpath, fn)
                try:
                    st = os.stat(fp)
                    files.append((fp, st.st_atime, st.st_size))
                    total += st.st_size
                except OSError:
                    continue

        if total <= self.max_size_bytes:
            return

        log.info(
            "Cache eviction: %.1f GB > %.1f GB limit, evicting...",
            total / 1024**3,
            self.max_size_bytes / 1024**3,
        )
        # Sort by atime ascending (oldest accessed first)
        files.sort(key=lambda x: x[1])
        target = int(self.max_size_bytes * 0.9)  # Evict down to 90%
        for fp, _, size in files:
            if total <= target:
                break
            try:
                os.unlink(fp)
                total -= size
            except OSError:
                continue
        log.info("Cache eviction done: %.1f GB remaining", total / 1024**3)


def fetch_upstream(key: str) -> bytes | None:
    """Fetch an object from upstream S3. Returns None on 404."""
    # Path-style URL for upstream
    url = f"https://{UPSTREAM_HOST}/{key}"
    req = urllib.request.Request(url, method="GET")
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            return resp.read()
    except urllib.error.HTTPError as e:
        if e.code in (404, 403):
            return None
        log.warning("Upstream error for %s: %s", key, e)
        return None
    except Exception as e:
        log.warning("Upstream fetch failed for %s: %s", key, e)
        return None


def head_upstream(key: str) -> tuple[bool, int]:
    """Check if object exists upstream. Returns (exists, size)."""
    url = f"https://{UPSTREAM_HOST}/{key}"
    req = urllib.request.Request(url, method="HEAD")
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            size = int(resp.headers.get("Content-Length", 0))
            return True, size
    except urllib.error.HTTPError:
        return False, 0
    except Exception:
        return False, 0


class S3ProxyHandler(http.server.BaseHTTPRequestHandler):
    """HTTP handler that implements a minimal S3-compatible API."""

    cache: CacheManager  # Set by the factory
    inflight: InflightTracker  # Set by the factory

    def log_message(self, format, *args):
        # Use our logger instead of stderr
        log.debug(format, *args)

    def _extract_key(self) -> str:
        """Extract the S3 key from the URL path.

        OpenDAL with custom endpoint uses path-style:
          /<bucket>/<key>
        The full path includes the bucket name.
        We strip the leading / and use the rest as the cache key.
        """
        return self.path.lstrip("/")

    def _send_blob(self, data: bytes) -> None:
        """Send a 200 response with binary data."""
        self.send_response(200)
        self.send_header("Content-Type", "binary/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("ETag", f'"{hashlib.md5(data).hexdigest()}"')
        self.end_headers()
        self.wfile.write(data)

    def _send_not_found(self) -> None:
        """Send a 404 NoSuchKey response."""
        self.send_response(404)
        self.send_header("Content-Type", "application/xml")
        body = b'<?xml version="1.0"?><Error><Code>NoSuchKey</Code></Error>'
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        key = self._extract_key()

        if not key or key == "/" or key == "stats":
            # Health check / stats
            with STATS_LOCK:
                stats = dict(STATS)
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            body = json.dumps(stats, indent=2).encode()
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        # Try local cache first
        data = self.cache.get(key)
        if data is not None:
            with STATS_LOCK:
                STATS["local_hits"] += 1
                STATS["bytes_served"] += len(data)
            self._send_blob(data)
            return

        # Cache miss — need to fetch from upstream.
        # Use inflight tracker to deduplicate concurrent fetches for the same key.
        is_fetcher, event, slot = self.inflight.acquire(key)

        if is_fetcher:
            # We are the first thread to request this key — do the actual fetch
            data = fetch_upstream(key)
            if data is not None:
                try:
                    self.cache.put(key, data)
                except OSError as e:
                    log.warning("Failed to cache %s locally: %s", key, e)
            # Wake all waiters with the result
            self.inflight.release(key, data)

            if data is not None:
                with STATS_LOCK:
                    STATS["upstream_hits"] += 1
                    STATS["bytes_fetched"] += len(data)
                    STATS["bytes_served"] += len(data)
                self._send_blob(data)
                # Background eviction check
                threading.Thread(target=self.cache.evict_if_needed, daemon=True).start()
            else:
                with STATS_LOCK:
                    STATS["misses"] += 1
                self._send_not_found()
        else:
            # Another thread is already fetching this key — wait for it
            event.wait(timeout=60)
            data = slot[0]

            if data is not None:
                with STATS_LOCK:
                    STATS["upstream_dedup"] += 1
                    STATS["bytes_served"] += len(data)
                self._send_blob(data)
            else:
                with STATS_LOCK:
                    STATS["misses"] += 1
                self._send_not_found()

    def do_PUT(self):
        key = self._extract_key()
        content_length = int(self.headers.get("Content-Length", 0))

        if content_length == 0:
            self.send_response(200)
            self.end_headers()
            return

        data = self.rfile.read(content_length)

        try:
            self.cache.put(key, data)
            with STATS_LOCK:
                STATS["puts"] += 1
                STATS["bytes_stored"] += len(data)
            log.debug("PUT %s (%d bytes)", key, len(data))
        except OSError as e:
            log.error("PUT failed for %s: %s", key, e)
            with STATS_LOCK:
                STATS["errors"] += 1
            self.send_response(500)
            self.end_headers()
            return

        self.send_response(200)
        self.send_header("ETag", f'"{hashlib.md5(data).hexdigest()}"')
        self.end_headers()

        # Background eviction check
        threading.Thread(target=self.cache.evict_if_needed, daemon=True).start()

    def do_HEAD(self):
        key = self._extract_key()

        exists, size = self.cache.exists(key)
        if exists:
            self.send_response(200)
            self.send_header("Content-Length", str(size))
            self.send_header("Content-Type", "binary/octet-stream")
            self.end_headers()
            return

        exists, size = head_upstream(key)
        if exists:
            self.send_response(200)
            self.send_header("Content-Length", str(size))
            self.send_header("Content-Type", "binary/octet-stream")
            self.end_headers()
            return

        self.send_response(404)
        self.end_headers()

    def do_DELETE(self):
        # sccache doesn't delete, but respond OK anyway
        self.send_response(204)
        self.end_headers()


class ThreadedHTTPServer(http.server.ThreadingHTTPServer):
    """Threaded HTTP server with SO_REUSEADDR."""

    allow_reuse_address = True
    daemon_threads = True


def main():
    parser = argparse.ArgumentParser(description="Local S3 cache proxy for sccache")
    parser.add_argument(
        "--port", type=int, default=8083, help="Port to listen on (default: 8083)"
    )
    parser.add_argument(
        "--cache-dir",
        type=str,
        default="./cache/sccache",
        help="Local cache directory (default: ./cache/sccache)",
    )
    parser.add_argument(
        "--max-size",
        type=float,
        default=80.0,
        help="Max cache size in GB (default: 80)",
    )
    parser.add_argument(
        "--verbose", "-v", action="store_true", help="Enable debug logging"
    )
    args = parser.parse_args()

    if args.verbose:
        log.setLevel(logging.DEBUG)

    cache = CacheManager(cache_dir=Path(args.cache_dir), max_size_gb=args.max_size)
    inflight = InflightTracker()

    # Create handler class with cache and inflight references
    handler = type(
        "Handler",
        (S3ProxyHandler,),
        {"cache": cache, "inflight": inflight},
    )

    server = ThreadedHTTPServer(("0.0.0.0", args.port), handler)
    log.info("sccache S3 proxy listening on 0.0.0.0:%d", args.port)
    log.info("Upstream: %s", UPSTREAM_HOST)
    log.info("Cache: %s (max %.1f GB)", args.cache_dir, args.max_size)
    log.info("Stats: curl http://localhost:%d/", args.port)

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        log.info("Shutting down...")
        server.shutdown()
        with STATS_LOCK:
            log.info("Final stats: %s", json.dumps(STATS, indent=2))


if __name__ == "__main__":
    main()
