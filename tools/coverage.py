#!/usr/bin/env python3
"""Whole-firmware coverage report: host suites + emulator scenarios.

    tools/coverage.py              # build, run, report → build/coverage/
    tools/coverage.py --no-build   # report from existing .gcda files

Outputs (build/coverage/):
    coverage.lcov   uploaded to Codecov by CI
    html/index.html browsable report
    summary.txt     per-component table, also printed

gcov only knows the files a test compiled, which would report the pure
libraries (~99 %) as the project figure. Every firmware .cpp that no test
compiles is therefore appended to the lcov file with all its code lines at 0,
so the number reflects the whole code base and rises as the harness grows.
"""
import argparse
import collections
import os
import re
import shutil
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(REPO, "build", "coverage")
BUILD_TESTS = os.path.join(OUT, "tests")
BUILD_EMU = os.path.join(OUT, "emulator")

# Generated tables, vendored code and host-only scaffolding are not firmware.
EXCLUDE = re.compile(
    r"/test/|/tests/|third_party|font_data\.cpp|font_oled\.cpp|splash_anim\.cpp|"
    r"splash_oled\.cpp|^tools/")


def run(cmd, **kw):
    print("+", " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True, **kw)


def build_and_run():
    shutil.rmtree(OUT, ignore_errors=True)
    run(["cmake", "-S", "tests", "-B", BUILD_TESTS, "-DPIXFROG_COVERAGE=ON"], cwd=REPO,
        stdout=subprocess.DEVNULL)
    run(["cmake", "--build", BUILD_TESTS, "--parallel"], cwd=REPO, stdout=subprocess.DEVNULL)
    run(["ctest", "--test-dir", BUILD_TESTS, "--output-on-failure"], cwd=REPO)
    # The emulator compiles the real menu/canvas code: its scenarios count too.
    run(["cmake", "-S", "tools/emulator", "-B", BUILD_EMU, "-DCMAKE_BUILD_TYPE=Debug",
         "-DCMAKE_CXX_FLAGS=--coverage -O0", "-DCMAKE_EXE_LINKER_FLAGS=--coverage"], cwd=REPO,
        stdout=subprocess.DEVNULL)
    run(["cmake", "--build", BUILD_EMU, "--parallel"], cwd=REPO, stdout=subprocess.DEVNULL)
    env = dict(os.environ, SDL_VIDEODRIVER=os.environ.get("SDL_VIDEODRIVER", "dummy"))
    emu = os.path.join(BUILD_EMU, "pixfrog_emu")
    for script in sorted(os.listdir(os.path.join(REPO, "tools", "emulator"))):
        if script == "smoke.sh" or (script.startswith("scenario_") and script.endswith(".sh")):
            run(["bash", os.path.join("tools", "emulator", script), emu], cwd=REPO, env=env)
    run([sys.executable, "tools/emulator/crawl.py", emu, "--no-golden"], cwd=REPO, env=env)


def code_lines(path):
    """Line numbers holding code (not blank, not a comment)."""
    lines, in_block = [], False
    with open(os.path.join(REPO, path), errors="ignore") as f:
        for no, raw in enumerate(f, 1):
            s = raw.strip()
            if in_block:
                if "*/" in s:
                    in_block = False
                continue
            if not s or s.startswith("//") or s in ("{", "}", "};"):
                continue
            if s.startswith("/*"):
                in_block = "*/" not in s
                continue
            lines.append(no)
    return lines


def firmware_sources():
    out = subprocess.run(["git", "ls-files", "components/*.cpp", "main/*.cpp"], cwd=REPO,
                         capture_output=True, text=True, check=True).stdout.split()
    return [p for p in out if not EXCLUDE.search(p)]


def report():
    os.makedirs(os.path.join(OUT, "html"), exist_ok=True)
    lcov = os.path.join(OUT, "coverage.lcov")
    common = ["-r", REPO, "--filter", r"components/", "--filter", r"main/",
              "--exclude", r".*/test/.*", "--exclude", r".*third_party.*",
              "--exclude", r".*(font_data|font_oled|splash_anim|splash_oled)\.cpp",
              "--gcov-ignore-parse-errors=negative_hits.warn_once_per_file",
              BUILD_TESTS, BUILD_EMU]
    run([sys.executable, "-m", "gcovr", *common, "--lcov", lcov,
         "--html-details", os.path.join(OUT, "html", "index.html")], cwd=REPO)

    covered = set()
    with open(lcov) as f:
        for line in f:
            if line.startswith("SF:"):
                covered.add(os.path.relpath(os.path.join(REPO, line[3:].strip()), REPO))

    per = collections.defaultdict(lambda: [0, 0])  # component → [hit, total]
    with open(lcov) as f:
        cur = None
        for line in f:
            if line.startswith("SF:"):
                cur = os.path.relpath(os.path.join(REPO, line[3:].strip()), REPO)
            elif line.startswith("DA:") and cur:
                _, hits = line[3:].split(",")[:2]
                comp = cur.split("/")[1] if cur.startswith("components/") else "main"
                per[comp][1] += 1
                per[comp][0] += int(hits) > 0

    with open(lcov, "a") as f:
        for path in firmware_sources():
            if path in covered:
                continue
            lines = code_lines(path)
            if not lines:
                continue
            f.write("TN:\nSF:%s\n" % path)
            for no in lines:
                f.write("DA:%d,0\n" % no)
            f.write("LH:0\nLF:%d\nend_of_record\n" % len(lines))
            comp = path.split("/")[1] if path.startswith("components/") else "main"
            per[comp][1] += len(lines)

    rows = ["%-18s %7s %7s %6s" % ("component", "hit", "lines", "cover")]
    hit = tot = 0
    for comp, (h, t) in sorted(per.items(), key=lambda kv: kv[1][0] / max(kv[1][1], 1)):
        rows.append("%-18s %7d %7d %5.1f%%" % (comp, h, t, 100.0 * h / max(t, 1)))
        hit += h
        tot += t
    rows.append("%-18s %7d %7d %5.1f%%" % ("TOTAL", hit, tot, 100.0 * hit / max(tot, 1)))
    text = "\n".join(rows)
    print(text)
    with open(os.path.join(OUT, "summary.txt"), "w") as f:
        f.write(text + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--no-build", action="store_true", help="report from existing .gcda files")
    args = ap.parse_args()
    if not args.no_build:
        build_and_run()
    report()


if __name__ == "__main__":
    main()
