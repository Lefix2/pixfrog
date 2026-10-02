#!/usr/bin/env python3
"""Capture the demo box's API answers for the GitHub Pages demo.

    tools/demo/snapshot.py            # rewrite tools/demo/snapshot.json
    tools/demo/snapshot.py --check    # fail if the API grew fields the snapshot lacks

Runs build/tests/harness/pixfrog_api_host --demo (the screenshots' show-sized
setup) and records what the SPA reads: config, status, diag, logs, FSEQ files
and playlist, the control fixture. mock.js starts from this state, so the demo
speaks the firmware's own JSON shapes; --check (run by tests/web) flags a
snapshot that went stale after an API change.
"""
import json
import os
import socket
import subprocess
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
HOST = os.environ.get("PIXFROG_API_HOST",
                      os.path.join(REPO, "build", "tests", "harness", "pixfrog_api_host"))
OUT = os.path.join(HERE, "snapshot.json")
GETS = {
    "config": "/api/config",
    "status": "/api/status",
    "diag": "/api/diag",
    "logs": "/api/logs",
    "fseq_files": "/api/fseq/files",
    "playlist": "/api/fseq/playlist",
    "fixture": "/api/control/fixture",
}


def capture():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    p = subprocess.Popen([HOST, "--port", str(port), "--demo"], stdout=subprocess.DEVNULL)
    url = f"http://127.0.0.1:{port}"
    try:
        for _ in range(100):
            try:
                urllib.request.urlopen(url + "/api/status", timeout=0.3)
                break
            except OSError:
                time.sleep(0.05)
        time.sleep(1.5)  # live counters and the push task have run a while
        out = {}
        for key, path in GETS.items():
            with urllib.request.urlopen(url + path, timeout=5) as r:
                body = r.read().decode()
            out[key] = body if key == "logs" else json.loads(body)
        return out
    finally:
        p.terminate()
        p.wait()


def keys(v, prefix=""):
    """Every key path in a JSON value (lists: their first element)."""
    if isinstance(v, dict):
        for k, x in v.items():
            yield prefix + k
            yield from keys(x, prefix + k + ".")
    elif isinstance(v, list) and v:
        yield from keys(v[0], prefix + "[].")


def main():
    snap = capture()
    if "--check" in sys.argv:
        old = json.load(open(OUT))
        missing = sorted(set(keys(snap)) - set(keys(old)))
        if missing:
            print("demo snapshot is stale, run tools/demo/snapshot.py; new fields:", missing)
            return 1
        print("demo snapshot covers every API field")
        return 0
    json.dump(snap, open(OUT, "w"), indent=1, sort_keys=True)
    print("wrote", os.path.relpath(OUT, REPO))
    return 0


if __name__ == "__main__":
    sys.exit(main())
