"""Lists the longest function bodies per file, so a cleanup can be aimed rather than guessed.

Deliberately crude: a function is a line starting in column 0 that ends in `{`, and it ends at the
first line that is exactly `}`. That misreads nothing in this codebase, where every definition is
formatted by clang-format at column 0, and it needs no parser.
"""

import argparse
import glob
import os
import re

START = re.compile(r"^[A-Za-z_].*\)\s*\{\s*$")
SKIP = re.compile(r"^\s*(typedef|struct|union|enum)\b")


def functions(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        lines = f.read().splitlines()
    out, i = [], 0
    while i < len(lines):
        if START.match(lines[i]) and not SKIP.match(lines[i]):
            for j in range(i + 1, len(lines)):
                if lines[j] == "}":
                    name = re.sub(r"\s*\(.*", "", lines[i]).split()[-1].lstrip("*")
                    out.append((j - i + 1, name, i + 1))
                    i = j
                    break
        i += 1
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--min", type=int, default=60, help="only report functions at least this long")
    ap.add_argument("--top", type=int, default=0, help="only the N longest overall (0 = all)")
    args = ap.parse_args()

    found = []
    for path in sorted(glob.glob("source/**/*.c", recursive=True)):
        for length, name, line in functions(path):
            if length >= args.min:
                found.append((length, path.replace(os.sep, "/"), name, line))
    found.sort(reverse=True)
    if args.top:
        found = found[: args.top]
    total = 0
    for length, path, name, line in found:
        print("%5d  %s:%d  %s" % (length, path, line, name))
        total += length
    print("\n%d function(s) at or over %d lines, %d lines in total" % (len(found), args.min, total))


if __name__ == "__main__":
    main()
