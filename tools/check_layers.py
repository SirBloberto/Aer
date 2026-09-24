#!/usr/bin/env python3
"""Fails when a source file crosses a layer boundary.

The directories under source/ are layers, lowest first. A file may include headers from its own
layer or any layer below it, never above: that is what lets each layer be read, and changed, in
terms of the ones beneath it alone. include/aer.h is the published contract and may be included
anywhere, and so may opcodes.h: it is one enum with no dependencies, and the operators the runtime
implements are named by the same values. A header named *_internal.h belongs to its directory and
may only be included from it.

Two jobs each have one owner. Only runtime/ allocates from the heap, so an object's layout and the
invariants of a freshly built one live in one place; the VM asks the runtime to build what it
needs. Only compiler/ and bytecode/ write bytecode, so what a compiled program looks like is
decided where it is compiled.

Some upward includes are deliberate -- the collector's roots are VM frames, for one -- and each is
listed in tools/layers_baseline.txt with the reason. A new one fails; a listed one that is gone is
reported so the list only ever shrinks.

Usage: python3 tools/check_layers.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASELINE = os.path.join(ROOT, "tools", "layers_baseline.txt")

VOCABULARY = {"opcodes.h"}

LAYERS = ["utilities", "runtime", "bytecode", "compiler", "vm", "stdlib", "host", "repl", "cli", "tools"]

# (what the job is, the pattern that does it, the layers allowed to)
OWNERS = [
    ("allocates from the heap", re.compile(r"\bheap_alloc\("), {"runtime"}),
    ("writes bytecode", re.compile(r"\bchunk_emit\(|->code\[[^\]]*\]\s*=[^=]"), {"compiler", "bytecode"}),
]


def layer_of(rel):
    parts = rel.split("/")
    if parts[0] == "include":
        return "include"
    if len(parts) == 2:
        return "cli"
    return parts[1]


def sources():
    for top in ("source", "include"):
        for dirpath, _, names in os.walk(os.path.join(ROOT, top)):
            for name in sorted(names):
                if name.endswith((".c", ".h")):
                    path = os.path.join(dirpath, name)
                    yield os.path.relpath(path, ROOT).replace(os.sep, "/"), path


def without_comments(text):
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)


def main():
    files = dict(sources())
    home = {}
    for rel in files:
        home.setdefault(os.path.basename(rel), rel)

    found = set()
    for rel, path in files.items():
        mine = layer_of(rel)
        with open(path, encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        for inc in re.findall(r'^#include "([^"]+)"', text, re.M):
            target = home.get(inc)
            if target is None:
                continue
            theirs = layer_of(target)
            if inc.endswith("_internal.h") and os.path.dirname(target) != os.path.dirname(rel):
                found.add("%s -> %s" % (rel, inc))
            elif inc in VOCABULARY or theirs in ("include", mine):
                continue
            elif mine in LAYERS and theirs in LAYERS and LAYERS.index(theirs) > LAYERS.index(mine):
                found.add("%s -> %s" % (rel, inc))
        code = without_comments(text)
        for job, pattern, owners in OWNERS:
            if mine not in owners and pattern.search(code):
                found.add("%s %s" % (rel, job))

    allowed = set()
    if os.path.exists(BASELINE):
        with open(BASELINE, encoding="utf-8") as fh:
            for line in fh:
                line = line.split("#", 1)[0].strip()
                if line:
                    allowed.add(line)

    new = sorted(found - allowed)
    gone = sorted(allowed - found)
    if gone:
        print("No longer present -- delete from tools/layers_baseline.txt:\n")
        for edge in gone:
            print("  " + edge)
        print()
    if new:
        print("Layer boundaries crossed (%s, lowest first):\n" % " < ".join(LAYERS))
        for edge in new:
            print("  " + edge)
        print("\nMove the code to the layer that owns it, or pass what it needs down from above.")
        return 1
    print("layer check: clean (%d deliberate exception(s) in tools/layers_baseline.txt)" % len(allowed))
    return 0


if __name__ == "__main__":
    sys.exit(main())
