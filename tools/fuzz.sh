#!/usr/bin/env bash
# Builds the libFuzzer targets (tests/fuzz, clang + ASan/UBSan) and fuzzes each
# one for SECONDS (default 60) in parallel, starting from the committed seed
# corpus. New inputs go to build/fuzz-corpus/<target> (kept between runs);
# a crash leaves build/fuzz-artifacts/<target>/crash-* and fails the script.
#
#   tools/fuzz.sh [SECONDS] [target...]
#   build/tests-fuzz/fuzz/fuzz_web_api build/fuzz-artifacts/fuzz_web_api/crash-…  # replay
set -euo pipefail
cd "$(dirname "$0")/.."

secs=${1:-60}
shift || true
targets=("$@")
[ ${#targets[@]} -eq 0 ] && targets=(fuzz_parsers fuzz_receivers fuzz_console fuzz_web_api)

CC=clang CXX=clang++ cmake -S tests -B build/tests-fuzz -DPIXFROG_FUZZ=ON >/dev/null
cmake --build build/tests-fuzz --parallel --target "${targets[@]}" >/dev/null

pids=()
for t in "${targets[@]}"; do
    mkdir -p "build/fuzz-corpus/$t" "build/fuzz-artifacts/$t"
    "build/tests-fuzz/fuzz/$t" -max_total_time="$secs" -timeout=10 -rss_limit_mb=2048 \
        -artifact_prefix="build/fuzz-artifacts/$t/" \
        "build/fuzz-corpus/$t" "tests/fuzz/corpus/$t" >"build/fuzz-artifacts/$t.log" 2>&1 &
    pids+=($!)
done
fail=0
for i in "${!targets[@]}"; do
    t=${targets[$i]}
    if wait "${pids[$i]}"; then
        echo "$t: $(grep -E 'DONE' "build/fuzz-artifacts/$t.log" | tail -1)"
    else
        fail=1
        echo "$t: FAILED"
        grep -E 'ERROR|SUMMARY|runtime error' "build/fuzz-artifacts/$t.log" | head -5
        ls "build/fuzz-artifacts/$t/"
    fi
done
exit $fail
