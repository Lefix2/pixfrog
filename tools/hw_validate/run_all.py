#!/usr/bin/env python3
"""Run the full hardware regression suite, one validator at a time.

    ./run_all.py                 # everything except OTA
    ./run_all.py --with-ota      # everything (flashes the inactive slot)
    ./run_all.py artnet scenes   # a subset
    ./run_all.py --junit out/    # + one JUnit XML per validator (CI / self-hosted runner)

Each validator opens its own UART session (the board resets between them —
that is fine and even desirable: every validator starts from a booted,
synced state). Exit code is non-zero if any validator failed.
"""
import subprocess
import sys
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ORDER = ["artnet", "sacn", "failsafe", "scenes", "show", "control_mode", "output",
         "identify_gamma", "fseq",
         "display", "webops", "auth", "ota"]


def main():
    argv = sys.argv[1:]
    env = dict(os.environ)
    if "--junit" in argv:
        i = argv.index("--junit")
        env["JUNIT_DIR"] = os.path.abspath(argv[i + 1])
        del argv[i:i + 2]
    args = [a for a in argv if not a.startswith("--")]
    with_ota = "--with-ota" in argv
    selected = args or [n for n in ORDER if n != "ota" or with_ota]

    failed = []
    for name in selected:
        script = os.path.join(HERE, f"validate_{name}.py")
        if not os.path.exists(script):
            print(f"unknown validator: {name}")
            failed.append(name)
            continue
        r = subprocess.run([sys.executable, script], env=env)
        if r.returncode != 0:
            failed.append(name)
        print()

    print("SUITE:", "PASS" if not failed else "FAIL → " + ", ".join(failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
