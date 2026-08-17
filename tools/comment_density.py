"""Comment lines per file, so a comment pass can be aimed rather than guessed.

Counts a block comment's continuation lines, which is the thing that actually accumulates: a
six-line block above a four-line function is what the convention is trying to prevent, and a naive
"lines starting with /*" count scores it as one.
"""

import argparse
import glob
import io
import os


def count(path):
    n = c = 0
    in_block = False
    for line in io.open(path, encoding="utf-8", errors="replace"):
        s = line.strip()
        if not s:
            continue
        n += 1
        if in_block:
            c += 1
            if "*/" in s:
                in_block = False
            continue
        if s.startswith("//") or (s.startswith("/*") and "*/" in s):
            c += 1
        elif s.startswith("/*"):
            c += 1
            in_block = True
    return c, n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--top", type=int, default=10)
    args = ap.parse_args()

    rows, total_c, total_n = [], 0, 0
    for pattern in ("source/**/*.c", "source/**/*.h"):
        for path in glob.glob(pattern, recursive=True):
            c, n = count(path)
            rows.append((c, n, path.replace(os.sep, "/")))
            total_c += c
            total_n += n
    rows.sort(reverse=True)
    print("%-40s %6s %6s %5s" % ("file", "cmt", "lines", "pct"))
    for c, n, path in rows[: args.top]:
        print("%-40s %6d %6d %4d%%" % (path, c, n, 100 * c // max(n, 1)))
    print("\nTOTAL %d comment lines of %d non-blank (%d%%)" % (total_c, total_n, 100 * total_c // total_n))


if __name__ == "__main__":
    main()
