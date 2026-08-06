#!/usr/bin/env python3
"""Compares two refs' instruction counts across the whole benchmark suite, on the Pi.

Exists because a regression was once reported as "isolation-clean" after comparing a fresh build
against a stale checkout -- tar preserves mtimes, so make happily skips rebuilding a changed file.
Every side here is clean-built from a git archive into its own scratch directory, so that failure
mode is unreachable rather than merely discouraged.

Instructions, not wall-clock: wall-clock on this Pi swings several percent from background load,
while instruction counts are stable to ~0.05% run to run.

Usage: python3 tools/bench.py [--base REF] [--head REF] [--runs N] [--threshold PCT]
                              [--only NAME,NAME] [--host USER@HOST] [--event EVENT]
"""
import argparse
import re
import subprocess
import sys

BENCHMARKS = [
    "nbody", "log_processing", "dict_bench", "small_dict_bench", "lookup_table_bench",
    "sieve", "mandelbrot", "binary_trees", "fib_bench", "struct_array_scan",
]


def ssh(host, cmd):
    """Argument list, never shell=True -- on Windows that routes through cmd.exe, where POSIX
       quoting silently mangles the remote command."""
    return subprocess.run(["ssh", host, cmd], capture_output=True, text=True)


def deploy(host, ref, path):
    """git archive REF -> remote path, then a genuinely clean build."""
    print("  building %s at %s ..." % (path, ref), flush=True)
    p = subprocess.run(["git", "archive", ref], capture_output=True)
    if p.returncode:
        sys.exit("git archive %s failed: %s" % (ref, p.stderr.decode()))
    remote = "rm -rf %s && mkdir -p %s && tar -x -C %s" % (path, path, path)
    r = subprocess.run(["ssh", host, remote], input=p.stdout, capture_output=True)
    if r.returncode:
        sys.exit("deploy failed: %s" % r.stderr.decode())
    r = ssh(host, "cd %s && make clean >/dev/null 2>&1 && make -j4 2>&1 | grep -c ': error'" % path)
    if r.stdout.strip() not in ("0", ""):
        sys.exit("build at %s reported errors -- aborting rather than measuring a broken tree" % ref)


def measure(host, path, name, runs, event):
    """Minimum of `runs` counts -- the least noise-inflated sample, not a mean."""
    best = None
    for _ in range(runs):
        r = ssh(host, "cd %s && perf stat -e %s ./binary/aer bench/%s.aer 2>&1 >/dev/null"
                % (path, event, name))
        m = re.search(r"^\s*([0-9,]+)\s+%s" % re.escape(event), r.stdout + r.stderr, re.M)
        if not m:
            return None
        v = int(m.group(1).replace(",", ""))
        best = v if best is None else min(best, v)
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default="HEAD~1")
    ap.add_argument("--head", default="HEAD")
    ap.add_argument("--runs", type=int, default=2)
    ap.add_argument("--threshold", type=float, default=0.3, help="flag deltas over this percent")
    ap.add_argument("--only", default="", help="comma-separated benchmark subset")
    ap.add_argument("--host", default="pi@192.168.18.13")
    # Changes that only move icache/BTB pressure (removing a dispatch-table entry, outlining a cold
    # opcode body) are instruction-neutral by construction and can only be judged on cycles.
    ap.add_argument("--event", default="instructions", help="perf event to count")
    args = ap.parse_args()

    names = [n.strip() for n in args.only.split(",") if n.strip()] or BENCHMARKS

    deploy(args.host, args.base, "~/bench-base")
    deploy(args.host, args.head, "~/bench-head")

    print("\n%-22s %14s %14s %9s" % ("benchmark", "base", "head", "delta"))
    print("-" * 62)
    regressions, improvements = [], []
    for name in names:
        b = measure(args.host, "~/bench-base", name, args.runs, args.event)
        h = measure(args.host, "~/bench-head", name, args.runs, args.event)
        if b is None or h is None:
            print("%-22s %14s %14s %9s" % (name, "?", "?", "FAILED"))
            continue
        pct = (h - b) * 100.0 / b
        flag = ""
        if pct > args.threshold:
            flag = "  <-- REGRESSION"
            regressions.append((name, pct))
        elif pct < -args.threshold:
            improvements.append((name, pct))
        print("%-22s %14d %14d %+8.2f%%%s" % (name, b, h, pct, flag))

    print("-" * 62)
    print("base=%s  head=%s  runs=%d (min)  threshold=%.2f%%  event=%s"
          % (args.base, args.head, args.runs, args.threshold, args.event))
    for name, pct in improvements:
        print("  improved: %-20s %+.2f%%" % (name, pct))
    if regressions:
        print("\n%d regression(s) over %.2f%%:" % (len(regressions), args.threshold))
        for name, pct in regressions:
            print("  %-20s %+.2f%%" % (name, pct))
        return 1
    print("\nno regression over %.2f%%" % args.threshold)
    return 0


if __name__ == "__main__":
    sys.exit(main())
