#!/usr/bin/env python3
"""Runs bench/*.aer against the same workload written in Lua and Python, and reports the ratios.

Every bench/NAME.aer that has a bench/NAME.lua and/or bench/NAME.py beside it is a hand-written
port of the SAME workload, not a generated one -- so a ratio here is a language comparison, not a
harness artifact. Interpreters are found on PATH; any that is missing is reported as such and
skipped rather than failing the run, so this works on a machine with only some of them installed.

Wall-clock, not instructions: the point is how AER compares to another language on the same
machine, and `perf` cannot count a different interpreter's work in comparable units. Each timing
is the MEAN of --runs executions, printed with the peak-to-peak spread beside it -- wall-clock is
the shakiest thing measured anywhere in this repo, and a ratio means nothing without knowing how
much the machine moved under it. Peak memory comes from /usr/bin/time, so it is reported when the
run is remote or on a POSIX box and shown as '-' otherwise.

Usage:
  python3 tools/compare_languages.py                       # every benchmark, every interpreter found
  python3 tools/compare_languages.py --runs 5
  python3 tools/compare_languages.py --only nbody,sieve
  python3 tools/compare_languages.py --host pi@192.168.18.13 --remote-dir '~/Aer'
"""
import argparse
import os
import shutil
import subprocess
import sys
import time

# label -> (candidate executable names, argv prefix, source suffix). Several names per runtime
# because the same interpreter is packaged under different ones -- python3 vs python vs py on
# Windows, lua5.4/lua5.3 on Debian. The first that resolves wins. LuaJIT runs with the JIT off
# because AER has no JIT: -joff compares interpreter against interpreter, the honest matchup.
RUNTIMES = [
    ("lua", ["lua", "lua5.4", "lua5.3", "lua54", "lua53"], [], ".lua"),
    ("luajit -joff", ["luajit"], ["-joff"], ".lua"),
    ("luau", ["luau"], [], ".lua"),
    ("python3", ["python3", "python", "py"], [], ".py"),
]


def find(names, host):
    """First of `names` that exists, or None. Resolves remotely when --host is given."""
    for exe in names:
        if host:
            r = subprocess.run(["ssh", host, "command -v %s" % exe], capture_output=True, text=True)
            if r.returncode == 0:
                return exe
        elif shutil.which(exe):
            return exe
    return None


def _peak_rss_windows(proc):
    """Peak working set of an exited child, in KB. Windows keeps the counters readable through the
       process handle after exit, which subprocess holds until the object is collected."""
    import ctypes
    from ctypes import wintypes

    class PMC(ctypes.Structure):
        _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                    ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                    ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                    ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t)]

    counters = PMC()
    counters.cb = ctypes.sizeof(PMC)
    try:
        if ctypes.WinDLL("psapi").GetProcessMemoryInfo(int(proc._handle), ctypes.byref(counters),
                                                       counters.cb):
            return counters.PeakWorkingSetSize // 1024
    except Exception:
        pass
    return 0


def time_once(argv, host, remote_dir):
    """(wall-clock seconds, peak RSS in KB) for one run, or None if it failed. RSS is 0 where it
    cannot be measured: /usr/bin/time supplies it remotely, GetProcessMemoryInfo locally on
    Windows, and a local POSIX run reports time only (getrusage would give a running maximum across
    every child, not this one's).

    Under --host the clock runs ON the remote machine, not here. Timing the ssh call instead would
    fold connection setup into every measurement -- and since that constant lands on both sides of
    a ratio, it would quietly drag every result toward 1.00x. The timing stays date-based rather
    than /usr/bin/time's own %e, which rounds to 10ms and would flatten the quicker benchmarks.
    """
    if host:
        remote = ("cd %s && start=$(date +%%s%%N); "
                  "/usr/bin/time -f '%%M' -o /tmp/aerbench.mem %s >/dev/null 2>&1; rc=$?; "
                  "end=$(date +%%s%%N); "
                  "echo $rc $(( (end-start)/1000000 )) $(cat /tmp/aerbench.mem 2>/dev/null || echo 0)"
                  % (remote_dir, " ".join(argv)))
        p = subprocess.run(["ssh", host, remote], capture_output=True, text=True)
        parts = p.stdout.split()
        if p.returncode != 0 or len(parts) < 3 or parts[0] != "0":
            return None
        return int(parts[1]) / 1000.0, int(parts[2])
    start = time.perf_counter()
    proc = subprocess.Popen(argv, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    proc.wait()
    elapsed = time.perf_counter() - start
    if proc.returncode != 0:
        return None
    return elapsed, (_peak_rss_windows(proc) if os.name == "nt" else 0)


def measure(argv, runs, host, remote_dir):
    """(mean seconds, peak-to-peak spread as a percentage, mean peak RSS in KB).

    The mean rather than the best sample: the minimum hides how noisy a machine was, and on
    wall-clock that matters -- it is the shakiest thing measured here. The spread beside it is what
    tells a reader whether a 1.05x is a result or a shrug.
    """
    ts, ms = [], []
    for _ in range(runs):
        r = time_once(argv, host, remote_dir)
        if r is None:
            return None
        ts.append(r[0])
        ms.append(r[1])
    avg = sum(ts) / len(ts)
    return avg, (max(ts) - min(ts)) * 100.0 / avg if avg else 0.0, sum(ms) / float(len(ms))


def mem(kb):
    """Peak RSS, or '-' where the platform could not report it."""
    if not kb:
        return "-"
    return "%.0fM" % (kb / 1024.0) if kb >= 1024 else "%.0fK" % kb


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", type=int, default=3, help="timings per benchmark; the mean is reported")
    ap.add_argument("--only", default="", help="comma-separated benchmark subset")
    ap.add_argument("--aer", default="binary/aer", help="path to the aer executable")
    ap.add_argument("--host", default="", help="run everything over ssh on this host instead")
    ap.add_argument("--remote-dir", default="~/Aer", help="repo directory on --host")
    args = ap.parse_args()

    aer = args.aer
    if not args.host:
        if os.name == "nt" and not aer.endswith(".exe") and os.path.exists(aer + ".exe"):
            aer += ".exe"
        if not os.path.exists(aer):
            sys.exit("aer executable not found at %s -- build it first (make all)" % aer)

    names = [n.strip() for n in args.only.split(",") if n.strip()]
    if not names:
        if args.host:
            r = subprocess.run(["ssh", args.host, "ls %s/bench/*.aer" % args.remote_dir],
                               capture_output=True, text=True)
            names = sorted(os.path.basename(l)[:-4] for l in r.stdout.split() if l.endswith(".aer"))
        else:
            names = sorted(f[:-4] for f in os.listdir("bench") if f.endswith(".aer"))

    available, missing = [], []
    for label, exe_names, prefix, suffix in RUNTIMES:
        found = find(exe_names, args.host)
        if found:
            available.append((label, found, prefix, suffix))
        else:
            missing.append(label)
    if missing:
        print("not installed, skipped: %s" % ", ".join(missing))
    if not available:
        sys.exit("no comparison interpreter found on PATH -- install at least one of: %s"
                 % ", ".join(r[0] for r in RUNTIMES))

    cols = [label for label, _, _, _ in available]
    width = 50 + 16 * len(cols)
    print("\n%-20s %8s %6s %7s %s"
          % ("benchmark", "aer", "±", "mem", " ".join("%15s" % c for c in cols)))
    print("-" * width)

    ratio_totals = {c: [] for c in cols}
    mem_totals = {c: [] for c in cols}
    for name in names:
        # Forward slashes deliberately: valid on Windows too, and --host sends these
        # straight to a POSIX shell where a backslash from os.path.join would not resolve.
        aer_src = "bench/" + name + ".aer"
        a = measure([aer, aer_src], args.runs, args.host, args.remote_dir)
        if a is None:
            print("%-26s %8s  (aer run failed)" % (name, "?"))
            continue
        t_aer, sp_aer, m_aer = a
        cells = []
        for label, exe, prefix, suffix in available:
            src = "bench/" + name + suffix
            exists = (subprocess.run(["ssh", args.host, "test -f %s/%s" % (args.remote_dir, src)]).returncode == 0
                      if args.host else os.path.exists(src))
            if not exists:
                cells.append("%15s" % "-")
                continue
            r = measure([exe] + prefix + [src], args.runs, args.host, args.remote_dir)
            if r is None:
                cells.append("%15s" % "failed")
                continue
            ratio = r[0] / t_aer
            ratio_totals[label].append(ratio)
            if m_aer and r[2]:
                mem_totals[label].append(r[2] / m_aer)
            cells.append("%15s" % ("%.2fx %s" % (ratio, mem(r[2]))))
        # A wall-clock row that moved this much between runs cannot support a ratio; say so rather
        # than let it be read as a result. Short benchmarks on a busy desktop are the usual cause.
        noisy = "  <-- noisy, raise --runs" if sp_aer > 5.0 else ""
        print("%-26s %7.2fs %5.1f%% %7s %s%s"
              % (name, t_aer, sp_aer, mem(m_aer), " ".join(cells), noisy))

    print("-" * width)
    print("ratio > 1.00x means AER is faster on that row; the value beside it is that runtime's")
    print("peak memory. runs=%d (mean), '±' is peak-to-peak spread, wall-clock." % args.runs)
    for label in cols:
        rs = ratio_totals[label]
        if not rs:
            continue
        wins = sum(1 for r in rs if r > 1.0)
        # Geometric mean: ratios compose multiplicatively, so an arithmetic mean would let one
        # lopsided row dominate the summary.
        geo = 1.0
        for r in rs:
            geo *= r
        geo **= 1.0 / len(rs)
        ms = mem_totals[label]
        mem_note = ""
        if ms:
            gm = 1.0
            for r in ms:
                gm *= r
            mem_note = ", %.2fx memory" % (gm ** (1.0 / len(ms)))
        print("  vs %-14s %.2fx geomean over %d benchmarks, AER faster on %d%s"
              % (label, geo, len(rs), wins, mem_note))


if __name__ == "__main__":
    main()
