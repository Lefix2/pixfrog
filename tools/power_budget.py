#!/usr/bin/env python3
"""Power budget of a 48 V run feeding LED bars through strip nodes
(docs/LONG_RUNS.md §6).

A run is one cable from the PSU (home run, `--home` m) to the first node, then
node to node over jumpers (`--jumper` m each). Every node is a constant-power
load: its bar through a buck of efficiency `--eff`, plus `--logic` W. The
ladder is solved exactly (fixed point on the node voltages), so the drop of
the home run and of every jumper is counted.

  power_budget.py --bars 8 --px 60 --w-px 0.32 --home 20     one run
  power_budget.py --px 60 --w-px 0.32 --table                limits table
  power_budget.py --markdown                                 the doc tables

A run passes when its current stays under `--imax`, its cable loss under
`--loss` and the last node above `--vmin`."""
import argparse
import math

# IEC 60228 class 5 (flexible) copper, max resistance at 20 °C: 13.3 Ω/km for
# 1.5 mm². Scaled by section; a cable at 60 °C reads ~16 % more.
RHO_FLEX = 13.3e-3 * 1.5  # Ω·mm²/m


def loop_ohms(length_m, section_mm2, hot=False):
    r = 2 * length_m * RHO_FLEX / section_mm2
    return r * 1.16 if hot else r


def solve(a):
    """Node voltages and run current for a's bars; None when the cable cannot
    deliver (the constant-power loads collapse the line)."""
    p_node = a.px * a.w_px / a.eff + a.logic
    segs = [loop_ohms(a.home, a.section, a.hot)] + \
           [loop_ohms(a.jumper, a.section, a.hot)] * (a.bars - 1)
    v = [a.volts] * a.bars
    # A bar on the bus straight (no buck) draws about its rated current
    # whatever it gets: constant current, not constant power.
    draw = (lambda x: p_node / a.volts) if getattr(a, "direct", False) else \
           (lambda x: p_node / x)
    for _ in range(500):
        # Current through segment k = every node from k on.
        cur = [draw(x) for x in v]
        down = [sum(cur[k:]) for k in range(a.bars)]
        nv, at = [], a.volts
        for k in range(a.bars):
            at -= down[k] * segs[k]
            nv.append(at)
        if min(nv) <= 0:  # the line collapses
            return None
        if max(abs(x - y) for x, y in zip(nv, v)) < 1e-6:
            v = nv
            break
        v = nv
    else:
        return None
    if min(v) < a.volts / 2:  # past the maximum power transfer point
        return None
    i = sum(draw(x) for x in v)
    p_in = a.volts * i
    p_load = sum(draw(x) * x for x in v)
    return dict(v_end=v[-1], current=i, p_supply=p_in, p_load=p_load,
                loss=(p_in - p_load) / p_in, p_bar=a.px * a.w_px, p_node=p_node)


def verdict(a, r):
    if r is None:
        return "cable cannot deliver"
    bad = []
    if r["current"] > a.imax:
        bad.append(f"current > {a.imax:g} A")
    if r["loss"] > a.loss:
        bad.append(f"loss > {a.loss:.0%}")
    if r["v_end"] < a.vmin:
        bad.append(f"last node < {a.vmin:g} V")
    return ", ".join(bad) if bad else "ok"


def with_(a, **kw):
    b = argparse.Namespace(**vars(a))
    for k, x in kw.items():
        setattr(b, k, x)
    return b


def max_bars(a, home):
    n = 0
    while n < 500 and verdict(a, solve(with_(a, bars=n + 1, home=home))) == "ok":
        n += 1
    return n


def max_home(a, bars):
    if verdict(a, solve(with_(a, bars=bars, home=0.0))) != "ok":
        return 0.0
    lo, hi = 0.0, 2000.0
    for _ in range(60):
        mid = (lo + hi) / 2
        ok = verdict(a, solve(with_(a, bars=bars, home=mid))) == "ok"
        lo, hi = (mid, hi) if ok else (lo, mid)
    return lo


def report(a):
    r = solve(a)
    print(f"bar {a.px} px × {a.w_px:g} W = {a.px * a.w_px:.1f} W; node input "
          f"{a.px * a.w_px / a.eff + a.logic:.1f} W (buck {a.eff:.0%} + {a.logic:g} W)")
    print(f"run: {a.bars} bars, home {a.home:g} m + {a.bars - 1} × {a.jumper:g} m, "
          f"{a.section:g} mm², PSU {a.volts:g} V")
    if r:
        print(f"current {r['current']:.2f} A, last node {r['v_end']:.1f} V, "
              f"cable loss {r['loss']:.1%}, PSU {r['p_supply']:.0f} W")
    print("verdict:", verdict(a, r))


def table(a, homes=(5, 10, 20, 30, 50), bar_counts=(1, 2, 4, 8, 16)):
    print(f"Max bars per run (home run, then {a.jumper:g} m jumpers):")
    print("home m | " + " | ".join(f"{h:g}" for h in homes))
    print("bars   | " + " | ".join(str(max_bars(a, h)) for h in homes))
    print("\nMax home run (m) for a number of bars:")
    print("bars | " + " | ".join(str(n) for n in bar_counts))
    print("m    | " + " | ".join(f"{max_home(a, n):.0f}" for n in bar_counts))


def markdown(base):
    """The tables of docs/LONG_RUNS.md §6, from the default assumptions."""
    bars = [("A", "1 m · 60 px RGB, 12 V class", 60, 0.24),
            ("B", "1 m · 60 px RGBW, 24 V class", 60, 0.32),
            ("C", "1 m · 144 px RGB", 144, 0.24)]
    homes = (5, 10, 20, 30, 50)
    print("| Bar | Bar W | Node in W | " +
          " | ".join(f"{h} m" for h in homes) + " |")
    print("|---|---:|---:|" + "---:|" * len(homes))
    for tag, label, px, w in bars:
        a = with_(base, px=px, w_px=w)
        cells = [str(max_bars(a, h)) for h in homes]
        print(f"| {tag} — {label} | {px * w:.1f} | {px * w / a.eff + a.logic:.1f} | " +
              " | ".join(cells) + " |")
    print()
    counts = (1, 2, 4, 8, 16)
    print("| Bar | " + " | ".join(f"{n} bars" for n in counts) + " |")
    print("|---|" + "---:|" * len(counts))
    for tag, label, px, w in bars:
        a = with_(base, px=px, w_px=w)
        cells = []
        for n in counts:
            m = max_home(a, n)
            cells.append(f"{m:.0f} m" if m >= 1 else "—")
        print(f"| {tag} | " + " | ".join(cells) + " |")
    print()
    # Same bar B, same cable, other bus voltages (no buck below 48 V: the bar
    # takes the bus straight, so the whole drop reaches the LEDs).
    print("| Bus | Current | Last bar sees | Cable loss |")
    print("|---|---:|---:|---:|")
    for volts, direct in ((12, True), (24, True), (52, False)):
        a = with_(base, px=60, w_px=0.32, bars=8, home=20.0, volts=float(volts),
                  direct=direct)
        if direct:
            a.eff, a.logic = 1.0, 0.0
        r = solve(a)
        if r is None:
            print(f"| {volts} V direct | — | drop over {volts} V: impossible | — |")
            continue
        seen = f"{r['v_end']:.1f} V ({(r['v_end'] - volts) / volts:+.0%})" if direct \
            else f"{r['v_end']:.1f} V → buck → 24 V"
        label = f"{volts} V direct" if direct else "48 V (52) + buck"
        print(f"| {label} | {r['current']:.1f} A | {seen} | {r['loss']:.0%} |")


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--bars", type=int, default=8)
    p.add_argument("--px", type=int, default=60, help="pixels per bar")
    p.add_argument("--w-px", type=float, default=0.32,
                   help="W per pixel at full white (RGB ~0.24, RGBW ~0.32)")
    p.add_argument("--home", type=float, default=20.0, help="PSU to first node, m")
    p.add_argument("--jumper", type=float, default=1.0, help="node to node, m")
    p.add_argument("--section", type=float, default=1.5, help="mm² per conductor")
    p.add_argument("--volts", type=float, default=52.0, help="PSU output (48 V trimmed up)")
    p.add_argument("--eff", type=float, default=0.95, help="node buck efficiency")
    p.add_argument("--logic", type=float, default=0.5, help="node MCU + link, W")
    p.add_argument("--imax", type=float, default=10.0, help="per-run current limit, A")
    p.add_argument("--loss", type=float, default=0.10, help="max cable loss")
    p.add_argument("--vmin", type=float, default=36.0, help="min node input, V")
    p.add_argument("--hot", action="store_true", help="cable at 60 °C (+16 %%)")
    p.add_argument("--table", action="store_true")
    p.add_argument("--markdown", action="store_true")
    a = p.parse_args()
    if a.markdown:
        markdown(a)
    elif a.table:
        table(a)
    else:
        report(a)


if __name__ == "__main__":
    main()
