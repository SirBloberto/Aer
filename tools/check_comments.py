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
# How far a file may drift above its recorded density before failing, so one genuinely needed
# sentence doesn't break the build while a paragraph habit still does.
DENSITY_SLACK = 2
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASELINE = os.path.join(ROOT, "tools", "comment_baseline.txt")
DENSITY_BASELINE = os.path.join(ROOT, "tools", "comment_density_baseline.txt")
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


def all_sources():
    for top in SCAN:
        for dirpath, _, names in os.walk(os.path.join(ROOT, top)):
            for name in sorted(names):
                if name.endswith((".c", ".h")):
                    path = os.path.join(dirpath, name)
                    yield os.path.relpath(path, ROOT).replace(os.sep, "/"), path


def density(path):
    """Comment lines as a percentage of the file's non-blank lines."""
    with open(path, encoding="utf-8", errors="replace") as fh:
        nonblank = sum(1 for line in fh if line.strip())
    comment = sum(length for _, length in blocks(path))
    return (comment * 100 // nonblank) if nonblank else 0


def read_density_baseline():
    recorded = {}
    if os.path.exists(DENSITY_BASELINE):
        with open(DENSITY_BASELINE, encoding="utf-8") as fh:
            for raw in fh:
                raw = raw.strip()
                if raw and not raw.startswith("#"):
                    rel, pct = raw.rsplit(" ", 1)
                    recorded[rel] = int(pct)
    return recorded


def scan_density():
    """Files whose comment density has grown past what the baseline recorded.

    A ratchet rather than a limit. MAX_BLOCK only kills essays, and the density that actually
    happened was death by a thousand three-line paragraphs, every one of which passes that check.
    An absolute threshold cannot work either: vm.h sits near half comment legitimately, because 149
    opcodes carry their operand shapes one line each."""
    recorded = read_density_baseline()
    grown = []
    for rel, path in all_sources():
        if rel in recorded and density(path) > recorded[rel] + DENSITY_SLACK:
            grown.append((rel, recorded[rel], density(path)))
    return grown


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


def dangling_enum_comments():
    """A comment block inside the Opcode enum that is followed by ANOTHER comment rather than by an
    opcode. That is what a deleted opcode leaves behind: the enum entry goes, the paragraph
    explaining it stays, and it then reads as documentation for whichever opcode follows. Three of
    these had accumulated in vm.h, one describing an opcode removed the same day."""
    path = os.path.join(ROOT, "source", "core", "vm.h")
    if not os.path.exists(path):
        return []
    with open(path, encoding="utf-8") as fh:
        text = fh.read()
    if "} Opcode;" not in text or "typedef enum {" not in text:
        return []
    head = text[: text.index("} Opcode;")]
    offset = text[: head.index("typedef enum {")].count("\n")
    lines = head[head.index("typedef enum {"):].splitlines()

    out = []
    i = 0
    while i < len(lines):
        if not lines[i].strip().startswith("/*"):
            i += 1
            continue
        start = i
        while i < len(lines) and "*/" not in lines[i]:
            i += 1
        i += 1
        j = i
        while j < len(lines) and not lines[j].strip():
            j += 1
        if j < len(lines) and lines[j].strip().startswith("/*"):
            out.append((offset + start + 1, lines[start].strip()[:70]))
    return out


def comment_blocks_ending_at(lines):
    """Maps a comment block's last line number to its (start, end)."""
    ends, start = {}, None
    for n, raw in enumerate(lines, 1):
        s = raw.strip()
        if start is None:
            if s.startswith("//") or (s.startswith("/*") and "*/" in s[2:]):
                ends[n] = (n, n)
            elif s.startswith("/*"):
                start = n
        elif "*/" in s:
            ends[n] = (start, n)
            start = None
    return ends


def dangling_label_comments():
    """The same failure as dangling_enum_comments, one file over: a dispatch label in vm.c preceded
    by two separate comment blocks. Deleting an opcode's handler leaves its paragraph behind, where
    it then reads as documentation for whichever label follows. Two of these were found by hand, one
    describing a struct cast sitting above lbl_dict_new."""
    path = os.path.join(ROOT, "source", "core", "vm.c")
    if not os.path.exists(path):
        return []
    with open(path, encoding="utf-8", errors="replace") as fh:
        lines = fh.read().split("\n")
    ends = comment_blocks_ending_at(lines)

    out = []
    for n, raw in enumerate(lines, 1):
        if not re.match(r"^lbl_[A-Za-z0-9_]+:", raw):
            continue
        cur, found = n - 1, []
        while cur >= 1:
            if not lines[cur - 1].strip():
                cur -= 1
            elif cur in ends:
                start, _ = ends[cur]
                found.append(start)
                cur = start - 1
            else:
                break
        if len(found) >= 2:
            out.append((found[-1], raw.strip()[:40], lines[found[-1] - 1].strip()[:60]))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--max", type=int, default=MAX_BLOCK)
    ap.add_argument("--update-baseline", action="store_true")
    ap.add_argument("--allow-raise", action="store_true", help="record densities that grew, not just ones that fell")
    args = ap.parse_args()

    found = scan(args.max)

    if args.update_baseline:
        with open(BASELINE, "w", encoding="utf-8") as fh:
            for rel, line, length in found:
                fh.write("%s:%d\n" % (rel, length))
        # Lower-only unless asked otherwise. Rewriting every entry to today's value also banks the
        # drift that stayed under DENSITY_SLACK, in files the cleanup never touched -- which loosens
        # the ratchet that running a cleanup was meant to tighten.
        recorded = read_density_baseline()
        raised = []
        with open(DENSITY_BASELINE, "w", encoding="utf-8") as fh:
            fh.write("# Comment lines as a percent of non-blank, per file. A ratchet: free to fall,\n")
            fh.write("# may not rise by more than %d without being re-recorded here.\n" % DENSITY_SLACK)
            for rel, path in all_sources():
                now = density(path)
                was = recorded.get(rel)
                if was is not None and now > was and not args.allow_raise:
                    raised.append((rel, was, now))
                    now = was
                fh.write("%s %d\n" % (rel, now))
        print("baseline: %d block(s) over %d lines, plus per-file density" % (len(found), args.max))
        for rel, was, now in raised:
            print("  kept %s at %d%% (now %d%%) -- pass --allow-raise to record the rise" % (rel, was, now))
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

    grown = scan_density()
    if grown:
        print("Comment density grew (see ARCHITECTURE.md, Contributing conventions):\n")
        for rel, was, now in grown:
            print("  %-42s %d%% -> %d%% of non-blank lines" % (rel, was, now))
        print("\nSay it in fewer lines, or if the file has genuinely earned it, run:")
        print("  python3 tools/check_comments.py --update-baseline")
        return 1

    stale = dangling_enum_comments()
    if stale:
        print("Comment blocks in the Opcode enum that document no opcode:\n")
        for line, text in stale:
            print("  source/core/vm.h:%d  %s" % (line, text))
        print("\nAn opcode was deleted and its paragraph stayed. Delete it, or attach it to the")
        print("opcode it actually describes.")
        return 1

    orphaned = dangling_label_comments()
    if orphaned:
        print("Dispatch labels in vm.c preceded by two separate comment blocks:\n")
        for line, label, text in orphaned:
            print("  source/core/vm.c:%d  %s  above %s" % (line, text, label))
        print("\nThe first block almost certainly documents a handler that no longer follows it.")
        print("Delete it, or merge it into the comment for the label it actually describes.")
        return 1

    print("comment check: clean (%d grandfathered block(s) remaining)" % len(found))
    return 0


if __name__ == "__main__":
    sys.exit(main())
