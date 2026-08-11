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

# compile_bound is deliberately unlike the rest: it is dominated by PARSING, not by the loop that
# follows. Every other benchmark measures steady-state execution of already-compiled code, so a
# change that taxes the compiler (reserving a register program-wide, say) was invisible here.
BENCHMARKS = [
    "nbody", "log_processing", "dict_bench", "small_dict_bench", "lookup_table_bench",
    "sieve", "mandelbrot", "binary_trees", "fib_bench", "struct_array_scan", "compile_bound",
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


def sample(host, path, name, event):
    """One run's (event count, peak RSS in KB). /usr/bin/time wraps perf rather than the other way
       round, so the RSS reported is the interpreter's own high-water mark, not perf's."""
    r = ssh(host, "cd %s && /usr/bin/time -f 'MAXRSS %%M' perf stat -e %s ./binary/aer bench/%s.aer"
                  " 2>&1 >/dev/null" % (path, event, name))
    out = r.stdout + r.stderr
    m = re.search(r"^\s*([0-9,]+)\s+%s" % re.escape(event), out, re.M)
    rss = re.search(r"^MAXRSS (\d+)", out, re.M)
    if not m:
        return None
    return int(m.group(1).replace(",", "")), int(rss.group(1)) if rss else 0


def measure_pair(host, name, runs, event):
    """Every run's samples for both sides, base and head INTERLEAVED rather than in two blocks.
    Running one side to completion first hands any drift over the measurement window (frequency
    ramp, page-cache warming, thermal) entirely to whichever side went first. That is invisible on
    instructions, which are deterministic, but on cycles it silently favoured head: a
    base-against-itself control reported six improvements and zero regressions, up to 5.15%, on
    identical code."""
    b, h = [], []
    for _ in range(runs):
        for path, into in (("~/bench-base", b), ("~/bench-head", h)):
            s = sample(host, path, name, event)
            if s is None:
                return None, None
            into.append(s)
    return b, h


def mean(xs):
    return sum(xs) / float(len(xs))


def spread(xs):
    """Peak-to-peak as a percentage of the mean -- what a reader needs to know before believing a
       delta, and honest about a 2-run sample in a way a standard deviation would not be."""
    m = mean(xs)
    return (max(xs) - min(xs)) * 100.0 / m if m else 0.0


def human(n):
    """3 significant digits with a magnitude suffix: raw 11-digit counts are unreadable side by
       side, and no decision here has ever turned on the last six of them."""
    for limit, suffix in ((1e9, "G"), (1e6, "M"), (1e3, "K")):
        if abs(n) >= limit:
            return "%.2f%s" % (n / limit, suffix)
    return "%.0f" % n


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

    print("\n%-20s %8s %8s %9s %7s %8s %8s" %
          ("benchmark", "base", "head", "delta", "spread", "mem", "mem Δ"))
    print("-" * 76)
    regressions, improvements = [], []
    for name in names:
        b, h = measure_pair(args.host, name, args.runs, args.event)
        if b is None or h is None:
            print("%-20s %8s %8s %9s" % (name, "?", "?", "FAILED"))
            continue
        bc, hc = [s[0] for s in b], [s[0] for s in h]
        bm, hm = [s[1] for s in b], [s[1] for s in h]
        pct = (mean(hc) - mean(bc)) * 100.0 / mean(bc)
        mem_pct = (mean(hm) - mean(bm)) * 100.0 / mean(bm) if mean(bm) else 0.0
        # the noisier side is the one that decides whether a delta means anything
        sp = max(spread(bc), spread(hc))
        flag = ""
        if pct > args.threshold:
            flag = "  <-- REGRESSION"
            regressions.append((name, pct))
        elif pct < -args.threshold:
            improvements.append((name, pct))
        print("%-20s %8s %8s %+8.2f%% %6.2f%% %8s %+7.1f%%%s" %
              (name, human(mean(bc)), human(mean(hc)), pct, sp,
               human(mean(hm) * 1024), mem_pct, flag))

    print("-" * 76)
    print("base=%s  head=%s  runs=%d (mean)  threshold=%.2f%%  event=%s"
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
