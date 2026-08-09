#!/usr/bin/env python3
"""Runs bench/*.aer against the same workload written in Lua and Python, and reports the ratios.

Every bench/NAME.aer that has a bench/NAME.lua and/or bench/NAME.py beside it is a hand-written
port of the SAME workload, not a generated one -- so a ratio here is a language comparison, not a
harness artifact. Interpreters are found on PATH; any that is missing is reported as such and
skipped rather than failing the run, so this works on a machine with only some of them installed.

Wall-clock, not instructions: the point is how AER compares to another language on the same
machine, and `perf` cannot count a different interpreter's work in comparable units. Each timing
is the MINIMUM of --runs executions, which is the least noise-inflated sample rather than a mean.

Usage:
  python3 tools/compare_languages.py                       # every benchmark, every interpreter found
  python3 tools/compare_languages.py --runs 5
  python3 tools/compare_languages.py --only nbody,sieve
  python3 tools/compare_languages.py --host pi@192.168.18.13 --remote-dir '~/Aer'
"""
import argparse
import os
import shutil
import subprocess
import sys
import time

# label -> (candidate executable names, argv prefix, source suffix). Several names per runtime
# because the same interpreter is packaged under different ones -- python3 vs python vs py on
# Windows, lua5.4/lua5.3 on Debian. The first that resolves wins. LuaJIT runs with the JIT off
# because AER has no JIT: -joff compares interpreter against interpreter, the honest matchup.
RUNTIMES = [
    ("lua", ["lua", "lua5.4", "lua5.3", "lua54", "lua53"], [], ".lua"),
    ("luajit -joff", ["luajit"], ["-joff"], ".lua"),
    ("luau", ["luau"], [], ".lua"),
    ("python3", ["python3", "python", "py"], [], ".py"),
]


def find(names, host):
    """First of `names` that exists, or None. Resolves remotely when --host is given."""
    for exe in names:
        if host:
            r = subprocess.run(["ssh", host, "command -v %s" % exe], capture_output=True, text=True)
            if r.returncode == 0:
                return exe
        elif shutil.which(exe):
            return exe
    return None


def time_once(argv, host, remote_dir):
    """Wall-clock seconds for one run, or None if it failed."""
    if host:
        remote = "cd %s && %s" % (remote_dir, " ".join(argv))
        argv = ["ssh", host, remote]
    start = time.perf_counter()
    p = subprocess.run(argv, capture_output=True, text=True)
    elapsed = time.perf_counter() - start
    return elapsed if p.returncode == 0 else None


def best_of(argv, runs, host, remote_dir):
    best = None
    for _ in range(runs):
        t = time_once(argv, host, remote_dir)
        if t is None:
            return None
        best = t if best is None else min(best, t)
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", type=int, default=3, help="timings per benchmark; the minimum is kept")
    ap.add_argument("--only", default="", help="comma-separated benchmark subset")
    ap.add_argument("--aer", default="binary/aer", help="path to the aer executable")
    ap.add_argument("--host", default="", help="run everything over ssh on this host instead")
    ap.add_argument("--remote-dir", default="~/Aer", help="repo directory on --host")
    args = ap.parse_args()

    aer = args.aer
    if not args.host:
        if os.name == "nt" and not aer.endswith(".exe") and os.path.exists(aer + ".exe"):
            aer += ".exe"
        if not os.path.exists(aer):
            sys.exit("aer executable not found at %s -- build it first (make all)" % aer)

    names = [n.strip() for n in args.only.split(",") if n.strip()]
    if not names:
        if args.host:
            r = subprocess.run(["ssh", args.host, "ls %s/bench/*.aer" % args.remote_dir],
                               capture_output=True, text=True)
            names = sorted(os.path.basename(l)[:-4] for l in r.stdout.split() if l.endswith(".aer"))
        else:
            names = sorted(f[:-4] for f in os.listdir("bench") if f.endswith(".aer"))

    available, missing = [], []
    for label, exe_names, prefix, suffix in RUNTIMES:
        found = find(exe_names, args.host)
        if found:
            available.append((label, found, prefix, suffix))
        else:
            missing.append(label)
    if missing:
        print("not installed, skipped: %s" % ", ".join(missing))
    if not available:
        sys.exit("no comparison interpreter found on PATH -- install at least one of: %s"
                 % ", ".join(r[0] for r in RUNTIMES))

    cols = [label for label, _, _, _ in available]
    print("\n%-22s %9s %s" % ("benchmark", "aer", " ".join("%18s" % c for c in cols)))
    print("-" * (32 + 19 * len(cols)))

    ratio_totals = {c: [] for c in cols}
    for name in names:
        # Forward slashes deliberately: valid on Windows too, and --host sends these
        # straight to a POSIX shell where a backslash from os.path.join would not resolve.
        aer_src = "bench/" + name + ".aer"
        t_aer = best_of([aer, aer_src], args.runs, args.host, args.remote_dir)
        if t_aer is None:
            print("%-22s %9s  (aer run failed)" % (name, "?"))
            continue
        cells = []
        for label, exe, prefix, suffix in available:
            src = "bench/" + name + suffix
            exists = (subprocess.run(["ssh", args.host, "test -f %s/%s" % (args.remote_dir, src)]).returncode == 0
                      if args.host else os.path.exists(src))
            if not exists:
                cells.append("%18s" % "-")
                continue
            t = best_of([exe] + prefix + [src], args.runs, args.host, args.remote_dir)
            if t is None:
                cells.append("%18s" % "failed")
                continue
            ratio = t / t_aer
            ratio_totals[label].append(ratio)
            cells.append("%18s" % ("%.3fs  %.2fx" % (t, ratio)))
        print("%-22s %8.3fs %s" % (name, t_aer, " ".join(cells)))

    print("-" * (32 + 19 * len(cols)))
    print("ratio > 1.00x means AER is faster on that row. runs=%d (min), wall-clock." % args.runs)
    for label in cols:
        rs = ratio_totals[label]
        if not rs:
            continue
        wins = sum(1 for r in rs if r > 1.0)
        # Geometric mean: ratios compose multiplicatively, so an arithmetic mean would let one
        # lopsided row dominate the summary.
        geo = 1.0
        for r in rs:
            geo *= r
        geo **= 1.0 / len(rs)
        print("  vs %-14s %.2fx geomean over %d benchmarks, AER faster on %d" % (label, geo, len(rs), wins))


if __name__ == "__main__":
    main()
