#!/usr/bin/env python3
"""Mutation-based fuzzer for the aer binary — seeds from tests/*.aer, mutates
(byte flips, token insert/delete, boundary-value substitution), runs each
result under a timeout. Goal: crashes/hangs (memory safety), not wrong output.

Best run against an ASAN build (`make asan`/`make fuzz`) so non-crashing bugs
still get caught. Known non-actionable "hang": a mutated numeric literal can
make a recursive seed fixture (e.g. test_perf_fusion.aer's is_even/is_odd)
loop forever via tail recursion — a property of that fixture's own logic,
not an AER bug. Check whether a saved tests/fuzz_crashes/ repro is minimal
and seed-independent before treating a "hang" as a real finding.

Usage: python3 tests/fuzz.py [--iterations N] [--binary PATH] [--seconds S] [--seed N]
"""
import argparse
import glob
import os
import random
import subprocess
import sys
import time

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CRASH_DIR = os.path.join(REPO_ROOT, "tests", "fuzz_crashes")

# Deliberately modest magnitudes — a range-loop bound (`for i in 0..N:`) is a
# common mutation target, and a huge substituted N (e.g. 1e16) produces a
# technically-finite but practically-endless loop that looks like a hang
# without being a memory-safety bug. True integer-boundary correctness
# (47-bit inline range, 64-bit overflow) has its own dedicated test
# (tests/test_memory_gc.aer) — this list only needs to be "unusual", not
# "as extreme as AerVal allows".
BOUNDARY_NUMBERS = [b"0", b"-1", b"1", b"100000", b"-100000", b"3.14e30", b"-0.0"]


def load_seeds():
    patterns = [
        os.path.join(REPO_ROOT, "tests", "*.aer"),
        os.path.join(REPO_ROOT, "tests", "**", "*.aer"),
    ]
    seeds = []
    for pattern in patterns:
        for path in glob.glob(pattern, recursive=True):
            with open(path, "rb") as f:
                data = f.read()
            if data:
                seeds.append(data)
    if not seeds:
        raise SystemExit("No .aer seed files found under tests/ — nothing to mutate")
    return seeds


def mutate_byte_flip(data, rng):
    data = bytearray(data)
    for _ in range(rng.randint(1, 4)):
        i = rng.randrange(len(data))
        data[i] ^= 1 << rng.randint(0, 7)
    return bytes(data)


def mutate_delete_span(data, rng):
    if len(data) < 2:
        return data
    start = rng.randrange(len(data))
    span = rng.randint(1, min(20, len(data) - start))
    return data[:start] + data[start + span:]


def mutate_insert_token(data, rng):
    tokens = [b"(", b")", b"[", b"]", b"{", b"}", b":", b",", b'"', b"\\", b"\n", b"\t", b"import ", b"function "]
    pos = rng.randrange(len(data) + 1)
    token = rng.choice(tokens)
    return data[:pos] + token + data[pos:]


def mutate_boundary_number(data, rng):
    text = data
    digit_positions = [i for i, b in enumerate(text) if 48 <= b <= 57]
    if not digit_positions:
        return data
    i = rng.choice(digit_positions)
    start = i
    while start > 0 and 48 <= text[start - 1] <= 57:
        start -= 1
    end = i
    while end < len(text) - 1 and 48 <= text[end + 1] <= 57:
        end += 1
    return text[:start] + rng.choice(BOUNDARY_NUMBERS) + text[end + 1:]


MUTATORS = [mutate_byte_flip, mutate_delete_span, mutate_insert_token, mutate_boundary_number]


def run_one(binary, data, timeout_s):
    import tempfile
    fd, path = tempfile.mkstemp(suffix=".aer")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        try:
            proc = subprocess.run([binary, path], capture_output=True, timeout=timeout_s)
        except subprocess.TimeoutExpired:
            return "hang", None
        # A negative returncode means the process was killed by a signal
        # (segfault, abort, ASAN detection) — a clean parse/runtime error
        # exits 1 via a normal return, never a signal.
        if proc.returncode is not None and proc.returncode < 0:
            return "crash", proc
        if proc.returncode not in (0, 1):
            return "crash", proc
        return "ok", proc
    finally:
        os.unlink(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iterations", type=int, default=200)
    ap.add_argument("--binary", default=os.path.join(REPO_ROOT, "binary", "aer.exe" if os.name == "nt" else "aer"))
    ap.add_argument("--seconds", type=float, default=8.0, help="per-run timeout")
    ap.add_argument("--seed", type=int, default=None, help="RNG seed, for reproducing a run")
    args = ap.parse_args()

    # Resolved to an absolute path — a bare relative path (e.g. "binary/aer.exe")
    # isn't reliably found by Windows' CreateProcess the way POSIX exec resolves
    # a cwd-relative path.
    args.binary = os.path.abspath(args.binary)
    if not os.path.isfile(args.binary):
        raise SystemExit(f"Binary not found: {args.binary} — build it first (make / make asan)")

    rng = random.Random(args.seed)
    seeds = load_seeds()
    os.makedirs(CRASH_DIR, exist_ok=True)

    crashes = 0
    hangs = 0
    start = time.time()
    for i in range(args.iterations):
        base = rng.choice(seeds)
        mutated = base
        for _ in range(rng.randint(1, 3)):
            mutated = rng.choice(MUTATORS)(mutated, rng)

        outcome, proc = run_one(args.binary, mutated, args.seconds)
        if outcome == "crash":
            crashes += 1
            crash_path = os.path.join(CRASH_DIR, f"crash_{i}.aer")
            with open(crash_path, "wb") as f:
                f.write(mutated)
            stderr = proc.stderr.decode("utf-8", "replace") if proc and proc.stderr else ""
            print(f"[CRASH] iteration {i}, saved to {crash_path}, returncode={proc.returncode if proc else '?'}")
            if stderr.strip():
                print(stderr.strip()[:2000])
        elif outcome == "hang":
            hangs += 1
            hang_path = os.path.join(CRASH_DIR, f"hang_{i}.aer")
            with open(hang_path, "wb") as f:
                f.write(mutated)
            print(f"[HANG] iteration {i}, saved to {hang_path}")

    elapsed = time.time() - start
    print(f"\n{args.iterations} iterations in {elapsed:.1f}s — {crashes} crash(es), {hangs} hang(s)")
    if crashes or hangs:
        print(f"Failing inputs saved under {CRASH_DIR}")
        sys.exit(1)
    sys.exit(0)


if __name__ == "__main__":
    main()
