#!/usr/bin/env python3
"""Fails when an opcode is never emitted by any benchmark or test.

An untested opcode here is not a theoretical gap. The raw and shape-specialized families read and
write a field's storage at a compile-time-constant offset with no tag ever materialized, so a
defect in one produces a silently wrong number rather than a crash -- nothing else in the suite
would notice. Writing coverage for the previously-untested int-field family is what turned up the
loop-condition register bug.

Coverage means "emitted into a chunk somewhere", not "hit at runtime": an opcode inside a branch a
test never takes still counts. That is deliberate -- the check is a tripwire for adding an opcode
with no test at all, which is the failure that actually happened, and a stricter definition would
need per-opcode hit counts that vary with input data.

The debug-tools build's disassembler is the source of truth for what a program emitted, so this
needs `make debug-tools` first.

Usage: python3 tools/check_opcode_coverage.py [--binary binary/aer-debug] [--update-baseline]
"""
import argparse
import glob
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASELINE = os.path.join(ROOT, "tools", "opcode_coverage_baseline.txt")

# Never reachable from a script, so they can never appear in a disassembly:
#   OP_AND/OP_OR/OP_PIPE  parser tags -- &&/||/|> compile to jumps and desugared calls, and none
#                         of the three is ever emitted or dispatched.
#   OP_PRINT_REPL         shell mode only; a file-driven run never emits it.
NEVER_EMITTED = {"OP_AND", "OP_OR", "OP_PIPE", "OP_PRINT_REPL"}


def declared_opcodes():
    """Every name in the Opcode enum, in vm.h, excluding the count marker."""
    src = open(os.path.join(ROOT, "source", "core", "vm.h"), encoding="utf-8").read()
    m = re.search(r'typedef enum \{(.*?)\} Opcode;', src, re.S)
    if not m:
        sys.exit("could not find the Opcode enum in source/core/vm.h")
    body = re.sub(r'/\*.*?\*/', '', m.group(1), flags=re.S)
    names = re.findall(r'\b(OP_[A-Z0-9_]+)\b', body)
    return {n for n in names if n != "OP_OPCODE_COUNT_MARKER"}


def emitted_opcodes(binary, programs):
    seen = set()
    for path in programs:
        with tempfile.TemporaryDirectory() as tmp:
            dump = os.path.join(tmp, "dump.txt")
            subprocess.run([binary, "--debug-path=" + dump, path],
                           capture_output=True, timeout=600)
            if not os.path.exists(dump):
                continue
            text = open(dump, encoding="utf-8", errors="replace").read()
        seen.update(re.findall(r'\b(OP_[A-Z0-9_]+)\b', text))
    return seen


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=os.path.join("binary", "aer-debug"))
    ap.add_argument("--update-baseline", action="store_true")
    args = ap.parse_args()

    binary = args.binary
    if not os.path.exists(binary) and os.path.exists(binary + ".exe"):
        binary += ".exe"
    if not os.path.exists(binary):
        sys.exit("%s not found -- run `make debug-tools` first" % args.binary)
    # Absolute and native-separator: Windows CreateProcess refuses a relative path spelled with
    # forward slashes, which is what a make/MSYS2 invocation passes in.
    binary = os.path.normpath(os.path.abspath(binary))

    programs = sorted(glob.glob(os.path.join(ROOT, "bench", "*.aer")))
    programs += sorted(glob.glob(os.path.join(ROOT, "tests", "test_*.aer")))

    declared = declared_opcodes()
    seen = emitted_opcodes(binary, programs)
    uncovered = sorted(declared - seen - NEVER_EMITTED)

    if args.update_baseline:
        with open(BASELINE, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("\n".join(uncovered) + ("\n" if uncovered else ""))
        print("baseline updated: %d uncovered opcode(s)" % len(uncovered))
        return 0

    allowed = set()
    if os.path.exists(BASELINE):
        allowed = {l.strip() for l in open(BASELINE, encoding="utf-8") if l.strip()}

    covered = len(declared) - len(NEVER_EMITTED) - len(uncovered)
    total = len(declared) - len(NEVER_EMITTED)
    new = [op for op in uncovered if op not in allowed]
    stale = sorted(allowed - set(uncovered))

    print("opcode coverage: %d/%d emitted by bench/ + tests/ (%d grandfathered)"
          % (covered, total, len(allowed)))

    if stale:
        print("\n%d baselined opcode(s) are now covered -- run with --update-baseline:" % len(stale))
        for op in stale:
            print("  " + op)
        return 1
    if new:
        print("\n%d opcode(s) no test or benchmark emits:" % len(new))
        for op in new:
            print("  " + op)
        print("\nAdd a case that emits it. A raw or specialized opcode with no coverage fails\n"
              "silently -- wrong numbers, not a crash. If it is genuinely unreachable, delete it\n"
              "(CLAUDE.md, Dead code) rather than baselining it.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
