#!/usr/bin/env python3
"""Refuse read-modify-write operations on sub-word atomics.

On the ESP32-P4 an atomic RMW on an 8/16-bit (or bool) object is an LR/SC
loop over the whole 32-bit word. A plain store from the other core to a
neighbour sharing that word was observed to be lost (the SC wrote the stale
half back): the DMX control universe slot kept reverting to "unmapped" next to
a 16-bit exchange. Loads and stores of sub-word atomics are fine; exchange,
fetch_* and compare_exchange_* must use a 32-bit (or pointer-sized) atomic.

    tools/lint_atomics.py            # scans components/ and main/, exit 1 on a finding
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SUBWORD = r"(?:bool|char|u?int8_t|u?int16_t|int8_t|int16_t|uint8_t|uint16_t|signed char|unsigned char|short|unsigned short)"
DECL = re.compile(r"std::atomic<\s*" + SUBWORD + r"\s*>\s+(\w+)")
RMW = r"\.(?:exchange|fetch_add|fetch_sub|fetch_or|fetch_and|fetch_xor|compare_exchange_weak|compare_exchange_strong)\s*\("


def sources():
    for top in ("components", "main"):
        for d, _, files in os.walk(os.path.join(ROOT, top)):
            if "/test" in d or "managed_components" in d:
                continue
            for f in files:
                if f.endswith((".cpp", ".h", ".c", ".hpp")):
                    yield os.path.join(d, f)


def main():
    findings = []
    for path in sources():
        text = open(path, errors="ignore").read()
        names = set(DECL.findall(text))
        for name in names:
            for m in re.finditer(r"\b" + re.escape(name) + r"(?:\[[^\]]*\])?" + RMW, text):
                line = text.count("\n", 0, m.start()) + 1
                findings.append(f"{os.path.relpath(path, ROOT)}:{line}: RMW on sub-word atomic "
                                f"'{name}' — use a 32-bit atomic (see tools/lint_atomics.py)")
    for f in findings:
        print(f)
    print(f"lint_atomics: {len(findings)} finding(s)")
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
