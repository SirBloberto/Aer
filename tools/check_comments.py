#!/usr/bin/env python3
"""Fails the build on comment blocks longer than MAX_BLOCK lines.

The convention (ARCHITECTURE.md, "Contributing conventions") has always been: default to no
comment, single line when one is needed, multi-line only for real hidden complexity. Stating it
there was not enough -- it depends on whoever is editing having read and remembered it, and it
drifted back to 20% comment density and 44-line blocks. This check is the same rule with teeth.

It deliberately cannot judge whether a 3-line comment is any good. It only kills the essays, which
is the failure mode that actually happened. Prefer turning a warning comment into a _Static_assert
wherever the constraint is checkable -- an assertion cannot be skimmed past.

Usage: python3 tools/check_comments.py [--max N] [--update-baseline]
"""
import argparse
import os
import re
import sys

MAX_BLOCK = 6
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASELINE = os.path.join(ROOT, "tools", "comment_baseline.txt")
SCAN = ("source", "include")


def blocks(path):
    """Yields (start_line, length) for every comment block in one file."""
    with open(path, encoding="utf-8", errors="replace") as fh:
        lines = fh.read().split("\n")
    out, start, count, inside = [], 0, 0, False
    for n, raw in enumerate(lines, 1):
        s = raw.strip()
        if inside:
            count += 1
            if "*/" in s:
                out.append((start, count))
                inside = False
        elif s.startswith("/*"):
            start, count = n, 1
            if "*/" in s[2:]:
                out.append((start, 1))
            else:
                inside = True
        elif s.startswith("//"):
            out.append((n, 1))
    if inside:
        out.append((start, count))
    return out


def scan(max_block):
    found = []
    for top in SCAN:
        for dirpath, _, names in os.walk(os.path.join(ROOT, top)):
            for name in names:
                if not name.endswith((".c", ".h")):
                    continue
                path = os.path.join(dirpath, name)
                rel = os.path.relpath(path, ROOT).replace(os.sep, "/")
                for line, length in blocks(path):
                    if length > max_block:
                        found.append((rel, line, length))
    return sorted(found)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--max", type=int, default=MAX_BLOCK)
    ap.add_argument("--update-baseline", action="store_true")
    args = ap.parse_args()

    found = scan(args.max)

    if args.update_baseline:
        with open(BASELINE, "w", encoding="utf-8") as fh:
            for rel, line, length in found:
                fh.write("%s:%d\n" % (rel, length))
        print("baseline: %d block(s) over %d lines" % (len(found), args.max))
        return 0

    # Grandfathers the blocks that already existed, keyed by file+length rather than line number so
    # unrelated edits above them don't spuriously fail. New offenders still fail; every entry
    # cleaned up should be removed with --update-baseline so the count only ever goes down.
    allowed = {}
    if os.path.exists(BASELINE):
        with open(BASELINE, encoding="utf-8") as fh:
            for raw in fh:
                raw = raw.strip()
                if raw:
                    rel, length = raw.rsplit(":", 1)
                    allowed[(rel, int(length))] = allowed.get((rel, int(length)), 0) + 1

    bad = []
    for rel, line, length in found:
        key = (rel, length)
        if allowed.get(key):
            allowed[key] -= 1
        else:
            bad.append((rel, line, length))

    if bad:
        print("Comment blocks over %d lines (see ARCHITECTURE.md, Contributing conventions):\n" % args.max)
        for rel, line, length in bad:
            print("  %s:%d  %d lines" % (rel, line, length))
        print("\nSay it in one line, delete it, or make it a _Static_assert. If it genuinely needs")
        print("the space, run: python3 tools/check_comments.py --update-baseline")
        return 1

    print("comment check: clean (%d grandfathered block(s) remaining)" % len(found))
    return 0


if __name__ == "__main__":
    sys.exit(main())
