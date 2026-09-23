#!/usr/bin/env python3
"""Checks that a runtime error reports the source line it actually happened on.

Nothing else in the suite covers this. test_errors_scope.aer asserts on error *behaviour* --
that a bad operation fails rather than silently producing a wrong value -- but an assert() based
test can never see the reported line, because the error aborts the run before the next assert.

That gap matters because vm->ip, which is what chunk_line_for_offset resolves against, is only
synced at sites that can raise. A missed sync does not crash or corrupt anything; it just makes a
future error blame the wrong line, which no other test would notice.

Each case in tests/error_lines/ marks every line it expects to fault with a trailing
`#!error: <substring>`. The expected line number is the marker's own line, so it cannot drift out
of date when a case is edited -- there is no second copy of it to forget to update. The first
marker is checked in detail; the rest only count, so an error cascading from the first fails.

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
# Optional, at most once per case: the function the report must name. A specialized body runs at
# its own code offset, so getting this wrong renames a hot function to "?" and nothing else notices.
IN_MARKER = "#!in:"
# Optional, at most once per case, on the call itself: the line the first "called from line N" must name.
CALLED_MARKER = "#!called-from"
CALLER = re.compile(r"called from line (\d+)")

# "<path>:<line>: Error: msg", or "<path>:<line>, in fn(): Error: msg" when it faulted inside a
# call. The path is matched loosely because it is whatever was passed on the command line.
REPORT = re.compile(r"^(?P<path>.+?):(?P<line>\d+)(?:, in (?P<fn>[^:]*))?: Error: (?P<msg>.*)$", re.M)

# Parse-time errors (error_at) instead echo the source line and point a caret at the column, so
# the line number and the message land three lines apart.
PARSE_REPORT = re.compile(r"^(?P<path>.+?):(?P<line>\d+) \| .*\n.*\nError: (?P<msg>.*)$", re.M)


def expectation(path):
    """([(line number, expected message substring)], expected function name or None, caller line or None)."""
    found = []
    want_fn = None
    want_caller = None
    with open(path, encoding="utf-8") as fh:
        for n, text in enumerate(fh, 1):
            if IN_MARKER in text:
                want_fn = text.split(IN_MARKER, 1)[1].strip()
            elif CALLED_MARKER in text:
                want_caller = n
            elif MARKER in text:
                found.append((n, text.split(MARKER, 1)[1].strip()))
    if not found:
        sys.exit("%s: no %s marker" % (path, MARKER))
    return found, want_fn, want_caller


def caller_line(out):
    mc = CALLER.search(out)
    return int(mc.group(1)) if mc else None


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
        markers, want_fn, want_caller = expectation(path)
        want_line, want_msg = markers[0]
        r = subprocess.run([binary, path], capture_output=True, text=True)
        out = r.stdout + r.stderr

        m = REPORT.search(out) or PARSE_REPORT.search(out)
        if not m:
            print("FAIL %-34s no 'file:line: Error:' report; got: %s" % (name, out.strip()[:90]))
            failures += 1
            continue
        got_line, got_msg = int(m.group("line")), m.group("msg")
        reported = len(REPORT.findall(out)) + len(PARSE_REPORT.findall(out))
        if got_line != want_line:
            print("FAIL %-34s reported line %d, expected %d (%s)" % (name, got_line, want_line, got_msg))
            failures += 1
        elif want_msg.lower() not in got_msg.lower():
            print("FAIL %-34s line %d correct, but message %r lacks %r"
                  % (name, got_line, got_msg, want_msg))
            failures += 1
        elif want_fn is not None and m.groupdict().get("fn") != want_fn:
            print("FAIL %-34s reported function %r, expected %r"
                  % (name, m.groupdict().get("fn"), want_fn))
            failures += 1
        elif want_caller is not None and caller_line(out) != want_caller:
            print("FAIL %-34s called from line %s, expected %d" % (name, caller_line(out), want_caller))
            failures += 1
        elif reported != len(markers):
            print("FAIL %-34s reported %d error(s), expected %d" % (name, reported, len(markers)))
            failures += 1
        else:
            print("ok   %-34s line %d: %s" % (name, got_line, got_msg))

    print("\n%d case(s), %d failure(s)" % (len(cases), failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
