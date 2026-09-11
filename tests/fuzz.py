#!/usr/bin/env python3
"""Mutation-based fuzzer for the aer binary — seeds from tests/*.aer, mutates
(byte flips, token insert/delete, boundary-value substitution), runs each
result under a timeout. Goal: crashes/hangs (memory safety), not wrong output.

Best run against an ASAN build (`make asan`/`make fuzz`) so non-crashing bugs
still get caught. A meaningful hang RATE is expected and not by itself a
finding: AER's loops are unrestricted, so mutating a comparison operator or a
range bound/step in any seed can trivially produce a genuinely-infinite loop by
AER's own semantics — the halting problem means no static check can rule this
out in general.

"Hang" is decided by an instruction budget (--max-instructions, enforced by the
interpreter itself), not by wall-clock. That makes it an exact property of the
program — "did not halt within N instructions" — and identical on every machine,
so a --seed run reproduces a hang set anywhere. A wall-clock limit could not: the
same mutant flipped between hang and pass depending on how loaded the machine
was, which made a fixed seed reproduce different results on different hardware.

--seconds survives as a backstop only, for a mutant that blocks in a syscall
(net.accept, reading stdin) and so never spends its budget. Those are reported
separately as BLOCKED, since unlike a hang they are machine-dependent.

Usage: python3 tests/fuzz.py [--iterations N] [--binary PATH] [--seconds S]
                             [--max-instructions N] [--seed N]
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
        # sorted() — glob order is filesystem-dependent, and an unsorted seed list made
        # --seed runs irreproducible across machines (same seed, different mutation targets).
        for path in sorted(glob.glob(pattern, recursive=True)):
            # tests/error_lines/ fixtures each abort on their first statement by design, so they
            # mutate into nothing useful — and admitting them would renumber every iteration of an
            # existing --seed run, since rng.choice() indexes into this list.
            if os.path.basename(os.path.dirname(path)) == "error_lines":
                continue
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


def mutate_nest(data, rng):
    # Single-token insertion needs hundreds of coincidences to reach a recursion limit, so the
    # depth caps in the parser and in value rendering went unfuzzed until this ran them directly.
    opener, closer = rng.choice([(b"(", b")"), (b"[", b"]"), (b"{", b"}")])
    depth = rng.choice([64, 256, 300, 1000])
    body = opener * depth + (closer * depth if rng.random() < 0.5 else b"")
    pos = rng.randrange(len(data) + 1)
    return data[:pos] + body + data[pos:]


# Appended, not inserted: rng.choice() indexes this list, so reordering it renumbers every
# iteration of an existing --seed run.
MUTATORS = [mutate_byte_flip, mutate_delete_span, mutate_insert_token, mutate_boundary_number,
            mutate_nest]


# aer's own exit code for --max-instructions being spent (main.c).
EXIT_BUDGET_EXHAUSTED = 3


def run_one(binary, data, timeout_s, max_instructions):
    import tempfile
    fd, path = tempfile.mkstemp(suffix=".aer")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        argv = [binary, "--max-instructions=%d" % max_instructions, path]
        try:
            proc = subprocess.run(argv, capture_output=True, timeout=timeout_s)
        except subprocess.TimeoutExpired:
            # The budget bounds computation, not blocking syscalls, so this still fires for a
            # mutant parked in net.accept or reading stdin. Wall-clock, hence machine-dependent --
            # which is why it is the backstop and not the primary signal.
            return "blocked", None
        # A negative returncode means the process was killed by a signal
        # (segfault, abort, ASAN detection) — a clean parse/runtime error
        # exits 1 via a normal return, never a signal.
        if proc.returncode is not None and proc.returncode < 0:
            return "crash", proc
        if proc.returncode == EXIT_BUDGET_EXHAUSTED:
            return "hang", proc
        if proc.returncode not in (0, 1):
            return "crash", proc
        return "ok", proc
    finally:
        os.unlink(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iterations", type=int, default=200)
    ap.add_argument("--binary", default=os.path.join(REPO_ROOT, "binary", "aer.exe" if os.name == "nt" else "aer"))
    ap.add_argument("--seconds", type=float, default=30.0,
                    help="wall-clock backstop for mutants that block on a syscall")
    ap.add_argument("--max-instructions", type=int, default=200_000_000,
                    help="a run exceeding this counts as a hang; deterministic, unlike wall-clock")
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
    blocked = 0
    start = time.time()
    for i in range(args.iterations):
        base = rng.choice(seeds)
        mutated = base
        for _ in range(rng.randint(1, 3)):
            mutated = rng.choice(MUTATORS)(mutated, rng)

        outcome, proc = run_one(args.binary, mutated, args.seconds, args.max_instructions)
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
        elif outcome == "blocked":
            blocked += 1
            blocked_path = os.path.join(CRASH_DIR, f"blocked_{i}.aer")
            with open(blocked_path, "wb") as f:
                f.write(mutated)
            print(f"[BLOCKED] iteration {i} exceeded {args.seconds}s of wall-clock without spending "
                  f"its instruction budget, saved to {blocked_path}")

    elapsed = time.time() - start
    print(f"\n{args.iterations} iterations in {elapsed:.1f}s — {crashes} crash(es), "
          f"{hangs} hang(s), {blocked} blocked")
    # blocked is deliberately not a failure: it is the wall-clock backstop, so it varies with
    # machine load and would make this gate flaky in exactly the way the budget was added to stop.
    if blocked:
        print(f"{blocked} run(s) hit the wall-clock backstop without spending their instruction "
              f"budget — inspect them, but they are machine-dependent and do not fail this run")
    if crashes or hangs:
        print(f"Failing inputs saved under {CRASH_DIR}")
        sys.exit(1)
    sys.exit(0)


if __name__ == "__main__":
    main()
