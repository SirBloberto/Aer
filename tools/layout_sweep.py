#!/usr/bin/env python3
"""Compares two refs across several code layouts, so the branch-layout lottery stops confounding it.

Cycle counts on this interpreter move several percent purely on how vm_run_slice's ~153 dispatch
sites happen to alias in the branch-target buffer (ARCHITECTURE 5.16yc). That is a property of the
build, not of the change: any edit shifts every label's address, and the resulting misprediction
count swings 2-4x in either direction with no relation to what the edit does. Comparing one build
against one build folds that straight into the answer.

This runs each side at several deliberately different code offsets (-DAER_LAYOUT_PAD) and reports
the MEDIAN delta plus the spread across layouts. A result whose spread dwarfs its median is the
lottery, not the change. Same idea as Stabilizer's randomized layout: the bias cannot be removed,
but it can be prevented from lining up with the thing being measured.

Instructions are still the primary gate (tools/bench.py) -- they are near-immune to layout. Reach
for this when a change is expected to move cycles and you need to know whether it really did.

Usage:
  python3 tools/layout_sweep.py --base HEAD~1 --head HEAD
  python3 tools/layout_sweep.py --base HEAD~1 --head HEAD --event cycles --pads 0,64,192,320
  python3 tools/layout_sweep.py --base HEAD --head HEAD --only nbody   # measure the lottery itself
"""
import argparse
import re
import statistics
import subprocess
import sys

BENCHMARKS = ["nbody", "sieve", "mandelbrot", "fib_bench", "dict_bench", "binary_trees",
              "struct_array_scan", "log_processing"]


def ssh(host, cmd):
    return subprocess.run(["ssh", host, cmd], capture_output=True, text=True)


def deploy(host, ref, path):
    p = subprocess.run(["git", "archive", ref], capture_output=True)
    if p.returncode:
        sys.exit("git archive %s failed: %s" % (ref, p.stderr.decode()))
    r = subprocess.run(["ssh", host, "rm -rf %s && mkdir -p %s && tar -x -C %s" % (path, path, path)],
                       input=p.stdout, capture_output=True)
    if r.returncode:
        sys.exit("deploy failed: %s" % r.stderr.decode())


def build(host, path, pad):
    r = ssh(host, "cd %s && make clean >/dev/null 2>&1 && "
                  "make -j4 ARCH_FLAGS=-DAER_LAYOUT_PAD=%d 2>&1 | grep -c ': error'" % (path, pad))
    if r.stdout.strip() not in ("0", ""):
        sys.exit("build at pad=%d reported errors" % pad)


def sample(host, path, name, event, runs):
    """Minimum of `runs` counts -- the least noise-inflated sample for THIS layout."""
    best = None
    for _ in range(runs):
        r = ssh(host, "cd %s && perf stat -e %s ./binary/aer bench/%s.aer 2>&1 >/dev/null"
                % (path, event, name))
        m = re.search(r"^\s*([0-9,]+)\s+%s" % re.escape(event), r.stdout + r.stderr, re.M)
        if m:
            v = int(m.group(1).replace(",", ""))
            best = v if best is None else min(best, v)
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default="HEAD~1")
    ap.add_argument("--head", default="HEAD")
    ap.add_argument("--pads", default="0,64,192,320,448",
                    help="code offsets to build each side at; more is better and slower")
    ap.add_argument("--runs", type=int, default=3, help="samples per layout (min is kept)")
    ap.add_argument("--only", default="")
    ap.add_argument("--host", default="pi@192.168.18.13")
    ap.add_argument("--event", default="cycles")
    args = ap.parse_args()

    pads = [int(p) for p in args.pads.split(",") if p.strip()]
    names = [n.strip() for n in args.only.split(",") if n.strip()] or BENCHMARKS

    print("deploying...", flush=True)
    deploy(args.host, args.base, "~/layout-base")
    deploy(args.host, args.head, "~/layout-head")

    per_bench = {n: [] for n in names}
    raw = {n: {"base": [], "head": []} for n in names}
    for pad in pads:
        print("  building both sides at pad=%d ..." % pad, flush=True)
        build(args.host, "~/layout-base", pad)
        build(args.host, "~/layout-head", pad)
        for n in names:
            b = sample(args.host, "~/layout-base", n, args.event, args.runs)
            h = sample(args.host, "~/layout-head", n, args.event, args.runs)
            if b and h:
                per_bench[n].append((h - b) * 100.0 / b)
                raw[n]["base"].append(b)
                raw[n]["head"].append(h)

    print("\n%-20s %9s %9s %9s %11s" % ("benchmark", "median", "min", "max", "lottery"))
    print("-" * 62)
    for n in names:
        d = per_bench[n]
        if not d:
            print("%-20s %9s" % (n, "FAILED"))
            continue
        # How much the BASE alone moves across layouts -- the noise floor this comparison sits on.
        bs = raw[n]["base"]
        lottery = (max(bs) - min(bs)) * 100.0 / min(bs) if len(bs) > 1 else 0.0
        verdict = "" if abs(statistics.median(d)) > lottery else "   <-- within the lottery"
        print("%-20s %+8.2f%% %+8.2f%% %+8.2f%% %10.2f%%%s"
              % (n, statistics.median(d), min(d), max(d), lottery, verdict))
    print("-" * 62)
    print("base=%s head=%s  %d layouts x %d runs, event=%s"
          % (args.base, args.head, len(pads), args.runs, args.event))
    print("'lottery' is how far the BASE alone moves across layouts. A median inside that band is\n"
          "not evidence about the change -- it is evidence about which build got lucky.")


if __name__ == "__main__":
    main()
