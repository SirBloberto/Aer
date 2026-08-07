#!/usr/bin/env python3
"""Runs matched AER and Lua programs and fails when their output differs.

The assert()-based suites can only check what someone already thought to check. This checks
agreement with a second, independent implementation of the same semantics, which catches the case
nobody predicted -- AER's `%` truncating while its `//` floored, so `(a // b) * b + (a % b) != a`
for negative operands, was found by the first fixture on its first run.

Fixtures live in tests/differential/ as <name>.aer and <name>.lua and must print byte-identical
output. Fractional results are scaled to integers inside the fixtures on purpose: float formatting
differs between the two languages for reasons that are not semantic, and comparing it would bury
real divergence in noise.

Where the two languages genuinely differ by design (0-based vs 1-based indexing, `null` vs `nil`),
the fixtures are written to compute the same values rather than to look the same -- the claim being
tested is that the semantics agree, not that the source does.

Usage: python3 tools/check_differential.py [--binary binary/aer] [--lua lua]
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CASES = os.path.join(ROOT, "tests", "differential")


def run(argv):
    p = subprocess.run(argv, capture_output=True, text=True, timeout=300)
    return (p.stdout + p.stderr).replace("\r\n", "\n").rstrip("\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=os.path.join("binary", "aer"))
    ap.add_argument("--lua", default="lua")
    args = ap.parse_args()

    binary = args.binary
    if not os.path.exists(binary) and os.path.exists(binary + ".exe"):
        binary += ".exe"
    if not os.path.exists(binary):
        sys.exit("aer binary not found at %s -- build it first (make)" % args.binary)
    binary = os.path.normpath(os.path.abspath(binary))

    if not shutil.which(args.lua):
        # Not every dev machine has Lua, and this must not become a reason to skip the whole
        # local test run. CI has it, so the gate is still enforced somewhere it counts.
        print("differential: '%s' not found on PATH -- skipping" % args.lua)
        return 0

    cases = sorted(glob.glob(os.path.join(CASES, "*.aer")))
    if not cases:
        sys.exit("no fixtures in %s" % CASES)

    failures = 0
    for aer_path in cases:
        name = os.path.splitext(os.path.basename(aer_path))[0]
        lua_path = os.path.join(CASES, name + ".lua")
        if not os.path.exists(lua_path):
            print("FAIL %-20s no matching %s.lua" % (name, name))
            failures += 1
            continue

        got, want = run([binary, aer_path]), run([args.lua, lua_path])
        if got == want:
            print("ok   %-20s %d line(s) agree" % (name, len(want.split("\n"))))
            continue

        failures += 1
        g, w = got.split("\n"), want.split("\n")
        print("FAIL %-20s output differs" % name)
        if len(g) != len(w):
            print("       aer printed %d line(s), lua printed %d" % (len(g), len(w)))
        for i in range(max(len(g), len(w))):
            a = g[i] if i < len(g) else "<no line>"
            b = w[i] if i < len(w) else "<no line>"
            if a != b:
                print("       line %d: aer=%r lua=%r" % (i + 1, a, b))

    print("\n%d fixture(s), %d failure(s)" % (len(cases), failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
