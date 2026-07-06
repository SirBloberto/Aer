#!/usr/bin/env python3
"""Summarizes gcov's .gcov output into a per-file/overall line-coverage percentage — a plain-text substitute for lcov, run after `make coverage`.

Usage: python3 tests/coverage_summary.py <dir containing .gcov files>
"""
import glob
import os
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def aer_source_names():
    """Basenames under source/ — excludes system-header .gcov files (stdio.h, ...) gcov also emits, which would otherwise skew the percentage."""
    names = set()
    for pattern in ("source/**/*.c", "source/**/*.h"):
        for path in glob.glob(os.path.join(REPO_ROOT, pattern), recursive=True):
            names.add(os.path.basename(path))
    return names


def summarize(path):
    """Returns (covered, executable) line counts for one .gcov file."""
    covered = 0
    executable = 0
    with open(path, "r", errors="replace") as f:
        for line in f:
            parts = line.split(":", 2)
            if len(parts) < 2:
                continue
            marker = parts[0].strip()
            if marker == "-":
                continue   # not executable (blank/comment/declaration)
            executable += 1
            if marker != "#####" and marker != "=====":
                covered += 1
    return covered, executable


def main():
    if len(sys.argv) != 2:
        raise SystemExit(f"Usage: {sys.argv[0]} <dir containing .gcov files>")
    directory = sys.argv[1]
    gcov_files = sorted(glob.glob(os.path.join(directory, "*.gcov")))
    if not gcov_files:
        raise SystemExit(f"No .gcov files found under {directory}")
    source_names = aer_source_names()

    total_covered = 0
    total_executable = 0
    print(f"{'File':<40} {'Covered/Total':>15} {'%':>7}")
    print("-" * 64)
    for path in gcov_files:
        name = os.path.basename(path).replace(".gcov", "")
        if name not in source_names:   # skip system-header .gcov noise
            continue
        covered, executable = summarize(path)
        if executable == 0:
            continue
        total_covered += covered
        total_executable += executable
        pct = 100.0 * covered / executable
        print(f"{name:<40} {covered:>6}/{executable:<8} {pct:>6.1f}%")

    print("-" * 64)
    if total_executable > 0:
        overall = 100.0 * total_covered / total_executable
        print(f"{'TOTAL':<40} {total_covered:>6}/{total_executable:<8} {overall:>6.1f}%")
    else:
        print("No executable lines found — coverage build may not have run any scripts")


if __name__ == "__main__":
    main()
