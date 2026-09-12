#!/usr/bin/env python3
"""Hashes the bytecode every .aer file in the corpus compiles to, so a refactor of the emit path can
prove it changed nothing.

`--debug-path` dumps the disassembly after a run, which means the dump also covers bodies compiled
lazily at runtime (shape and numeric specializations) -- exactly the codegen a compile-only dump
would miss. The trailing memory summary is stripped: it reports live cells, which a GC timing
difference can legitimately move without any instruction changing.

Usage:
  python3 tools/bytecode_identity.py --binary binary/aer --out before.json
  python3 tools/bytecode_identity.py --binary binary/aer --out after.json --compare before.json
"""
import argparse
import glob
import hashlib
import json
import os
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Needs a listening peer / real stdin / a wall clock; the compiled bytecode is still covered by the
# rest of the corpus, and a flaky row would make the gate useless.
SKIP = {"test_net.aer", "test_stdin.aer", "test_actor.aer", "test_scheduler.aer",
        "test_actor_specialize.aer"}


def corpus():
    out = []
    for pattern in ("tests/*.aer", "bench/*.aer", "examples/*.aer"):
        for p in sorted(glob.glob(os.path.join(REPO, pattern))):
            if os.path.basename(p) not in SKIP:
                out.append(p)
    return out


def digest(binary, path, tmpdir):
    dump = os.path.join(tmpdir, "dump.txt")
    try:
        subprocess.run([binary, "--max-instructions=4000000", "--debug-path=" + dump, path],
                       capture_output=True, timeout=180, cwd=REPO)
    except subprocess.TimeoutExpired:
        return "timeout"
    if not os.path.exists(dump):
        return "no-dump"
    with open(dump, "rb") as f:
        text = f.read()
    # Drop the memory summary: live-cell counts are not instructions.
    cut = text.find(b"--- memory ---")
    if cut != -1:
        text = text[:cut]
    return hashlib.sha256(text).hexdigest()[:16]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=os.path.join("binary", "aer"))
    ap.add_argument("--out", required=True)
    ap.add_argument("--compare")
    args = ap.parse_args()

    binary = args.binary
    if not os.path.exists(binary) and os.path.exists(binary + ".exe"):
        binary += ".exe"
    if not os.path.exists(binary):
        sys.exit("aer binary not found at %s -- build it first" % args.binary)
    binary = os.path.normpath(os.path.abspath(binary))

    files = corpus()
    result = {}
    with tempfile.TemporaryDirectory() as tmpdir:
        for p in files:
            rel = os.path.relpath(p, REPO).replace(os.sep, "/")
            result[rel] = digest(binary, p, tmpdir)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(result, f, indent=1, sort_keys=True)

    usable = sum(1 for v in result.values() if len(v) == 16)
    print("hashed %d file(s); %d produced a dump" % (len(result), usable))

    if not args.compare:
        return 0
    with open(args.compare, encoding="utf-8") as f:
        base = json.load(f)
    diffs = [k for k in sorted(set(base) | set(result)) if base.get(k) != result.get(k)]
    if not diffs:
        print("BYTECODE IDENTICAL across %d file(s)" % len(result))
        return 0
    print("\n%d file(s) compile differently:" % len(diffs))
    for k in diffs:
        print("  %-44s %s -> %s" % (k, base.get(k, "<absent>"), result.get(k, "<absent>")))
    return 1


if __name__ == "__main__":
    sys.exit(main())
