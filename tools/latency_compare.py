#!/usr/bin/env python3
"""Compare the app's reported latency with real tick-to-trade latency.

Runs build/market_maker on each fixture N times (no --trace) and, for every run,
reads what the app prints and writes:

  current metric   the "average latency" line the binary prints, taken as is
                   (nothing is recomputed here).
  tick-to-trade    for EVERY order sent: send.wall_ns - entry of the delivery that
                   caused it, from capture.csv alone. Callbacks that sent nothing
                   have no send record and contribute no sample.

Orders from all runs of a fixture are pooled into one distribution; no run is
dropped or averaged away, so slow runs stay in the tail.

Only reads app outputs; it never modifies app/ or sim/.

usage: tools/latency_compare.py [--runs N] [--binary PATH] [--fixtures a,b,...]
                                [--fixtures-dir DIR] [--csv FILE] [--gap-ms MIN:MAX]

--gap-ms sleeps a random MIN..MAX milliseconds before every run, so the CPU is
not kept hot by back-to-back runs and runs do not line up with periodic OS work.
"""
import argparse
import csv
import math
import random
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path


def percentile(sorted_vals, p):
    """Nearest-rank percentile (honest for small samples)."""
    if not sorted_vals:
        return float("nan")
    rank = max(1, math.ceil(p / 100.0 * len(sorted_vals)))
    return sorted_vals[rank - 1]


def read_capture(path):
    deliveries = {}  # index -> (kind, sim_time, entry, seq)
    sends = []       # (delivery_index, wall_ns), in file order
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            if row["record"] == "delivery":
                deliveries[int(row["delivery"])] = (
                    row["kind"],
                    int(row["sim_time_ns"]),
                    int(row["wall_ns"]),
                    int(row["seq"]) if row["seq"] else None,
                )
            else:
                sends.append((int(row["delivery"]), int(row["wall_ns"])))
    return deliveries, sends


def run_once(binary, scn):
    # Exactly: ./build/market_maker sim/fixtures/<name>.scn   (outputs go to the
    # default build/out/<name>/, overwritten on every run). Run from the repo root.
    proc = subprocess.run(
        [str(binary), str(scn)],
        capture_output=True, text=True, check=True,
    )
    m = re.search(r"messages: (\d+)\s+average latency: ([\d.e+-]+) ns", proc.stdout)
    if not m:
        sys.exit(f"could not parse report for {scn}:\n{proc.stdout}")
    return int(m.group(1)), float(m.group(2))


def summarize(vals):
    s = sorted(vals)
    if not s:
        return dict(n=0, mean=float("nan"), median=float("nan"),
                    p99=float("nan"), max=float("nan"))
    return dict(n=len(s), mean=statistics.fmean(s), median=statistics.median(s),
                p99=percentile(s, 99), max=s[-1])


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--runs", type=int, default=30)
    ap.add_argument("--binary", default="build/market_maker")
    ap.add_argument("--fixtures-dir", default="sim/fixtures")
    ap.add_argument("--fixtures", default="",
                    help="comma-separated names (default: every .scn)")
    ap.add_argument("--csv", default="", help="also write the table as CSV")
    ap.add_argument("--gap-ms", default="",
                    help="random sleep before each run, as MIN:MAX milliseconds")
    args = ap.parse_args()

    gap = None
    if args.gap_ms:
        lo, hi = (float(x) for x in args.gap_ms.split(":"))
        gap = (lo, hi)
    fdir = Path(args.fixtures_dir)
    names = ([n for n in args.fixtures.split(",") if n] or
             sorted(p.stem for p in fdir.glob("*.scn")))

    rows = []
    if True:
        for name in names:
            printed = []
            pooled, first, second = [], [], []
            n_deliv = n_timer = n_sending = n_orders = 0
            for i in range(args.runs):
                out = Path("build/out") / name
                if gap:
                    time.sleep(random.uniform(*gap) / 1000.0)
                _, printed_avg = run_once(args.binary, fdir / f"{name}.scn")
                deliveries, sends = read_capture(out / "capture.csv")
                printed.append(printed_avg)

                pos = {}
                for d_idx, t in sends:
                    lat = t - deliveries[d_idx][2]
                    p = pos.get(d_idx, 0)
                    pos[d_idx] = p + 1
                    pooled.append(lat)
                    (first if p == 0 else second).append(lat)
                if i == 0:  # counts are identical every run (deterministic input)
                    n_deliv = len(deliveries)
                    n_timer = sum(1 for d in deliveries.values() if d[0] == "timer")
                    n_sending = len(pos)
                    n_orders = len(sends)

            rows.append(dict(
                fixture=name, runs=args.runs,
                deliveries=n_deliv, timers=n_timer, sending=n_sending, orders=n_orders,
                cur_mean=statistics.fmean(printed),
                cur_min=min(printed), cur_max=max(printed),
                all=summarize(pooled), first=summarize(first), second=summarize(second),
            ))

    def f(x):
        return "-" if x != x else f"{x:,.0f}"

    print(f"\nGap before each run: {args.gap_ms or 'none'} ms.")
    print(f"Runs per fixture: {args.runs}. Times in ns. Tap-based = send - delivery entry, "
          f"every order, all runs pooled.\n")
    print("Table A1: what the current report averages over\n")
    print("| fixture | deliveries | timers | sending | orders | current mean (avg of runs) "
          "| current min..max across runs |")
    print("|---|---|---|---|---|---|---|")
    for r in rows:
        print(f"| {r['fixture']} | {r['deliveries']} | {r['timers']} | {r['sending']} | "
              f"{r['orders']} | {f(r['cur_mean'])} | {f(r['cur_min'])}..{f(r['cur_max'])} |")

    print("\nTable A2: tick-to-trade from the tap (pooled orders)\n")
    print("| fixture | orders pooled | mean | median | p99 | max |")
    print("|---|---|---|---|---|---|")
    for r in rows:
        a = r["all"]
        print(f"| {r['fixture']} | {a['n']} | {f(a['mean'])} | {f(a['median'])} | "
              f"{f(a['p99'])} | {f(a['max'])} |")

    print("\nTable A3: first vs second order in the same callback (pooled)\n")
    print("| fixture | 1st: n | median | p99 | 2nd: n | median | p99 |")
    print("|---|---|---|---|---|---|---|")
    for r in rows:
        a, b = r["first"], r["second"]
        print(f"| {r['fixture']} | {a['n']} | {f(a['median'])} | {f(a['p99'])} | "
              f"{b['n']} | {f(b['median'])} | {f(b['p99'])} |")

    if args.csv:
        with open(args.csv, "w", newline="") as fh:
            w = csv.writer(fh)
            w.writerow(["fixture", "runs", "deliveries", "timers", "sending", "orders",
                        "cur_mean", "cur_min", "cur_max",
                        "n", "mean", "median", "p99", "max",
                        "first_n", "first_median", "first_p99",
                        "second_n", "second_median", "second_p99"])
            for r in rows:
                a, b, c = r["all"], r["first"], r["second"]
                w.writerow([r["fixture"], r["runs"], r["deliveries"], r["timers"],
                            r["sending"], r["orders"], r["cur_mean"], r["cur_min"],
                            r["cur_max"], a["n"], a["mean"], a["median"], a["p99"],
                            a["max"], b["n"], b["median"], b["p99"],
                            c["n"], c["median"], c["p99"]])


if __name__ == "__main__":
    main()
