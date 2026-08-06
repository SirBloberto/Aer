#!/usr/bin/env python3
"""Checks that a runtime error reports the source line it actually happened on.

Nothing else in the suite covers this. test_errors_scope.aer asserts on error *behaviour* --
that a bad operation fails rather than silently producing a wrong value -- but an assert() based
test can never see the reported line, because the error aborts the run before the next assert.

That gap matters because vm->ip, which is what chunk_line_for_offset resolves against, is only
synced at sites that can raise. A missed sync does not crash or corrupt anything; it just makes a
future error blame the wrong line, which no other test would notice.

Each case in tests/error_lines/ marks the one line it expects to fault with a trailing
`#!error: <substring>`. The expected line number is the marker's own line, so it cannot drift out
of date when a case is edited -- there is no second copy of it to forget to update.

Usage: python3 tests/error_lines.py [--binary binary/aer]
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CASES = os.path.join(HERE, "error_lines")
MARKER = "#!error:"

# "<path>:<line>: Error: msg", or "<path>:<line>, in fn(): Error: msg" when it faulted inside a
# call. The path is matched loosely because it is whatever was passed on the command line.
REPORT = re.compile(r"^(?P<path>.+?):(?P<line>\d+)(?:, in [^:]*)?: Error: (?P<msg>.*)$", re.M)


def expectation(path):
    """(line number, expected message substring) from the single marked line."""
    found = []
    with open(path, encoding="utf-8") as fh:
        for n, text in enumerate(fh, 1):
            if MARKER in text:
                found.append((n, text.split(MARKER, 1)[1].strip()))
    if len(found) != 1:
        sys.exit("%s: expected exactly one %s marker, found %d" % (path, MARKER, len(found)))
    return found[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=os.path.join("binary", "aer"))
    args = ap.parse_args()

    binary = args.binary
    if not os.path.exists(binary) and os.path.exists(binary + ".exe"):
        binary += ".exe"
    if not os.path.exists(binary):
        sys.exit("aer binary not found at %s -- build it first (make all)" % args.binary)
    # Absolute and native-separator: Windows CreateProcess refuses a relative path spelled with
    # forward slashes, which is exactly what a make/MSYS2 invocation passes in.
    binary = os.path.normpath(os.path.abspath(binary))

    cases = sorted(f for f in os.listdir(CASES) if f.endswith(".aer"))
    if not cases:
        sys.exit("no cases in %s" % CASES)

    failures = 0
    for name in cases:
        path = os.path.join(CASES, name)
        want_line, want_msg = expectation(path)
        r = subprocess.run([binary, path], capture_output=True, text=True)
        out = r.stdout + r.stderr

        m = REPORT.search(out)
        if not m:
            print("FAIL %-34s no 'file:line: Error:' report; got: %s" % (name, out.strip()[:90]))
            failures += 1
            continue
        got_line, got_msg = int(m.group("line")), m.group("msg")
        if got_line != want_line:
            print("FAIL %-34s reported line %d, expected %d (%s)" % (name, got_line, want_line, got_msg))
            failures += 1
        elif want_msg.lower() not in got_msg.lower():
            print("FAIL %-34s line %d correct, but message %r lacks %r"
                  % (name, got_line, got_msg, want_msg))
            failures += 1
        else:
            print("ok   %-34s line %d: %s" % (name, got_line, got_msg))

    print("\n%d case(s), %d failure(s)" % (len(cases), failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
