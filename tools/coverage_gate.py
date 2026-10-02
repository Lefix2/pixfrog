#!/usr/bin/env python3
"""Fail when a branch lowers the project coverage.

    tools/coverage_gate.py --head-sha <sha>        # CI: both figures from Codecov
    tools/coverage_gate.py --lcov build/coverage/coverage.lcov   # ci-local

The base is the Codecov report of the merge-base with origin/main. If that
commit has no report, for example a docs-only commit that skipped the
coverage job, the gate walks back its first parents. The head is either
the Codecov report of the uploaded commit, polled until Codecov has
processed it, or the local lcov counted the way Codecov counts it:
- a line is a hit, a partial (some branch never taken) or a miss;
- lines holding only a brace are ignored;
- coverage = hits / lines.
The local figure matches Codecov to about 0.05 point, since the compiler
and toolchain differ from CI. CI is the reference.

The allowed drop is COVERAGE_MAX_DROP points (default 0.1): it absorbs
rounding, while a lost test (#126: -0.46) fails. Codecov's own statuses are
informational, and this job feeds the required "CI gate". A PR labelled
coverage-drop-ok skips the check (a deliberate, explained drop).
"""
import argparse
import collections
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
API = "https://api.codecov.io/api/v2/github/Lefix2/repos/pixfrog"
MAX_DROP = float(os.environ.get("COVERAGE_MAX_DROP", "0.1"))
BRACE = re.compile(r"^\s*[{}]\s*(//.*)?$")
IGNORED = ("tests/", "tools/")  # codecov.yml `ignore`


def git(*args):
    return subprocess.run(["git", *args], cwd=REPO, capture_output=True, text=True,
                          check=True).stdout.strip()


def codecov_totals(sha):
    """Codecov's totals for `sha`, or None while it has no processed report."""
    try:
        with urllib.request.urlopen(f"{API}/commits/{sha}/", timeout=20) as r:
            c = json.load(r)
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return None
        raise
    t = c.get("totals") or {}
    return t if c.get("state") == "complete" and t.get("lines") else None


def base_coverage(head):
    # From the PR head, not CI's merge commit (whose merge-base is main's tip).
    base = git("merge-base", "origin/main", head)
    for sha in git("rev-list", "--first-parent", "-n", "30", base).split():
        t = codecov_totals(sha)
        if t:
            return sha, float(t["coverage"])
    return base, None


def head_from_codecov(sha, wait_s=480):
    deadline = time.time() + wait_s
    while True:
        t = codecov_totals(sha)
        if t:
            return float(t["coverage"])
        if time.time() > deadline:
            sys.exit(f"coverage gate: Codecov has no processed report for {sha[:8]} "
                     f"after {wait_s} s")
        time.sleep(15)


def head_from_lcov(path):
    lines = collections.defaultdict(dict)  # file → line → [hits, branches, taken]
    cur = None
    with open(path) as f:
        for raw in f:
            l = raw.strip()
            if l.startswith("SF:"):
                cur = os.path.relpath(l[3:], REPO)
            elif l.startswith("DA:"):
                n, h = l[3:].split(",")[:2]
                e = lines[cur].setdefault(int(n), [0, 0, 0])
                e[0] = max(e[0], int(h))
            elif l.startswith("BRDA:"):
                n, _, _, taken = l[5:].split(",")
                e = lines[cur].setdefault(int(n), [0, 0, 0])
                e[1] += 1
                e[2] += taken not in ("-", "0")
    hits = total = 0
    for f, by_line in lines.items():
        if f.startswith(IGNORED) or "/test/" in f or "third_party" in f:
            continue
        src = open(os.path.join(REPO, f)).read().split("\n")
        for n, (h, branches, taken) in by_line.items():
            if n <= len(src) and BRACE.match(src[n - 1]):
                continue
            total += 1
            hits += h > 0 and taken == branches
    return 100.0 * hits / total


def main():
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--head-sha", help="the commit CI uploaded to Codecov")
    g.add_argument("--lcov", help="a local coverage.lcov (approximate, ci-local)")
    a = ap.parse_args()

    try:
        base_sha, base = base_coverage(a.head_sha or "HEAD")
    except (urllib.error.URLError, TimeoutError) as e:
        if a.head_sha:  # CI: the upload needs Codecov too, fail rather than pass blind
            sys.exit(f"coverage gate: Codecov unreachable ({e})")
        print(f"coverage gate: skipped, Codecov unreachable ({e})")
        return 0
    if base is None:
        print(f"coverage gate: no Codecov report near {base_sha[:8]}, nothing to compare")
        return 0
    head = head_from_codecov(a.head_sha) if a.head_sha else head_from_lcov(a.lcov)
    drop = base - head
    where = "Codecov" if a.head_sha else "local lcov (±0.05)"
    print(f"coverage gate: base {base_sha[:8]} {base:.2f} % → head {head:.2f} % ({where}), "
          f"{-drop:+.2f} pt, max drop {MAX_DROP:.2f}")
    if drop > MAX_DROP:
        print("coverage gate: FAIL — coverage went down; add tests for the code this "
              "branch touches, or explain the drop in the PR and label it "
              "coverage-drop-ok")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
