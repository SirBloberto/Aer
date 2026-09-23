#!/usr/bin/env python3
"""Ranks every opcode by how many times the corpus actually dispatches it.

check_opcode_coverage.py answers "is this opcode ever emitted", which is the tripwire for adding one
with no test. This answers the other question: does anything ever RUN it. The two differ by a lot.
The compound-raw field family reached 26 opcodes of which 4 carried traffic and 22 carried 1,493
dispatches between them -- all 26 were "covered".

The distinction that matters is not the number, it is what the opcode is for:

  A fusion or specialization opcode exists only to be faster than a generic sequence it can always
  fall back to. Negligible traffic makes it pure cost -- a handler, a dispatch-table slot, a parser
  emit site, a disassembler row, an untested path, and branch-target-buffer pressure on the opcodes
  that do carry the program.

  A semantic opcode is a language feature. OP_SLICE_GET dispatching 2,210 times means the benchmarks
  rarely slice, not that slicing should be removed.

So this reports; it does not fail a build. Run it before adding an opcode, to see what the one you
are about to duplicate is actually worth, and after, to see whether the new one earns its slot.

Needs a profile build: make profile

Usage: python3 tools/opcode_traffic.py [--binary binary/aer-profile] [--threshold 100000]
"""
import argparse
import glob
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Needs a listening peer, real stdin, or a wall clock -- a flaky row would make the ranking useless.
SKIP = {"test_net.aer", "test_stdin.aer", "test_actor.aer", "test_scheduler.aer",
        "test_actor_specialize.aer"}


def corpus():
    out = []
    for pattern in ("bench/*.aer", "tests/*.aer", "examples/*.aer"):
        for p in sorted(glob.glob(os.path.join(ROOT, pattern))):
            if os.path.basename(p) not in SKIP:
                out.append(p)
    return out


def declared_opcodes():
    """Only the ones the dispatch table can actually reach.

    Several opcodes are never an instruction's opcode byte: OP_NOT and OP_NEGATE ride in OP_UNARY's
    B field, and OP_AND/OP_OR/OP_PIPE are parser-internal tokens that compile to jumps. Ranking those
    alongside real opcodes would report them at zero dispatches forever and invite someone to "cut" a
    table slot they do not occupy. Their opcodes.def rows name the handler `none`.
    """
    src = open(os.path.join(ROOT, "source", "core", "opcodes.def"), encoding="utf-8").read()
    rows = re.findall(r"^OPCODE\(([A-Z0-9_]+), ([a-z0-9_]+),", src, re.M)
    return ["OP_" + name for name, handler in rows if handler != "none"]


def dispatches(binary, paths):
    totals = {}
    with tempfile.TemporaryDirectory() as tmp:
        dump = os.path.join(tmp, "dump.txt")
        for p in paths:
            try:
                subprocess.run([binary, "--max-instructions=8000000", "--debug-path=" + dump, p],
                               capture_output=True, timeout=300, cwd=ROOT)
            except subprocess.TimeoutExpired:
                continue
            if not os.path.exists(dump):
                continue
            text = open(dump, encoding="utf-8", errors="replace").read()
            m = re.search(r"--- per-opcode summary ---\n(.*?)(?:\n\n|\n---)", text, re.S)
            if not m:
                continue
            for line in m.group(1).splitlines():
                f = line.split()
                if len(f) == 2 and f[0].startswith("OP_"):
                    totals[f[0]] = totals.get(f[0], 0) + int(f[1])
    return totals


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=os.path.join("binary", "aer-profile"))
    ap.add_argument("--threshold", type=int, default=100000,
                    help="below this, an opcode is listed as carrying negligible traffic")
    args = ap.parse_args()

    binary = args.binary
    if not os.path.exists(binary) and os.path.exists(binary + ".exe"):
        binary += ".exe"
    if not os.path.exists(binary):
        sys.exit("no profile binary at %s -- run: make profile" % binary)
    # Absolute, because the runs below set cwd=ROOT and Windows resolves a relative program path
    # against the parent's directory, not the child's.
    binary = os.path.abspath(binary)

    ops = declared_opcodes()
    totals = dispatches(binary, corpus())
    grand = sum(totals.values())
    if grand == 0:
        sys.exit("no dispatch counts recorded -- is %s built with -DAER_PROFILE?" % binary)

    print("%d opcodes, %s dispatches across %d programs\n" % (len(ops), "{:,}".format(grand),
                                                              len(corpus())))
    ranked = sorted(ops, key=lambda o: -totals.get(o, 0))
    cum = 0
    print("carrying the program:")
    for o in ranked:
        n = totals.get(o, 0)
        if n < args.threshold:
            break
        cum += n
        print("  %-52s %14s  %6.2f%%  cum %6.2f%%" % (o, "{:,}".format(n), 100.0 * n / grand,
                                                      100.0 * cum / grand))

    low = [o for o in ranked if totals.get(o, 0) < args.threshold]
    print("\nbelow %s dispatches -- a fusion or specialization opcode here is pure cost, a language"
          "\nfeature here just means the corpus rarely uses it:" % "{:,}".format(args.threshold))
    for o in low:
        print("  %-52s %14s" % (o, "{:,}".format(totals.get(o, 0))))
    rest = grand - cum
    print("\n%d of %d opcodes carry the program; the other %d carry %s dispatches between them." %
          (len(ops) - len(low), len(ops), len(low), "{:,}".format(rest)))


if __name__ == "__main__":
    main()
