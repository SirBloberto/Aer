#!/usr/bin/env bash
# Benchmark AER against Python and Lua across seven workloads.
# Usage: ./benchmark.sh
# Requirements: python3 (required), lua (optional)
#
# Runs under plain POSIX bash (Linux/macOS) or Git Bash/MSYS2 on Windows —
# no bc, no GNU-only flags, and both `aer`/`aer.exe` and `python3`/`python`
# are resolved automatically so the same script works on either platform.

set -e

if [ -x "./binary/aer.exe" ]; then
    AER="./binary/aer.exe"
else
    AER="./binary/aer"
fi

PYTHON="$(command -v python3 2>/dev/null || command -v python 2>/dev/null || true)"
if [ -z "$PYTHON" ]; then
    echo "python3 (or python) not found on PATH — required to run this benchmark." >&2
    exit 1
fi

TMP="${TMPDIR:-/tmp}/aer_bench"
mkdir -p "$TMP"

RUNS=3          # number of timed runs per language per test
LUA_AVAILABLE=0
if command -v lua &>/dev/null || command -v lua5.4 &>/dev/null || command -v lua5.3 &>/dev/null; then
    LUA_AVAILABLE=1
    LUA="${LUA:-$(command -v lua5.4 2>/dev/null || command -v lua5.3 2>/dev/null || command -v lua)}"
fi

# ── helpers ────────────────────────────────────────────────────────────────────

divider() { printf '%0.s─' {1..60}; echo; }

# time_run <label> <cmd...>  — prints median wall time over $RUNS runs
# Uses awk (not bc — not installed by default on Windows/MSYS2) for the
# Xm Y.ZZZs -> seconds conversion and median arithmetic.
time_run() {
    local label="$1"; shift
    local times=()
    for _ in $(seq 1 $RUNS); do
        local t
        t=$({ time "$@" > /dev/null 2>&1; } 2>&1 | grep real | awk '{print $2}')
        # convert Xm Y.ZZZs -> seconds
        local min sec
        min=$(echo "$t" | sed 's/m.*//')
        sec=$(echo "$t" | sed 's/.*m//' | sed 's/s//')
        local total
        total=$(awk -v m="$min" -v s="$sec" 'BEGIN { printf "%.6f", (m * 60) + s }')
        times+=("$total")
    done
    # sort and pick median
    local median
    median=$(printf '%s\n' "${times[@]}" | sort -n | awk -v n="$RUNS" 'NR==int(n/2)+1')
    printf "  %-12s %ss\n" "$label" "$median"
}

# ── Workload 1: recursive Fibonacci(32) ───────────────────────────────────────

divider
echo "Workload 1 — recursive Fibonacci(32)"
divider

cat > "$TMP/fib.aer" << 'AER'
function fib(n):
    if n <= 1:
        return n
    return fib(n - 1) + fib(n - 2)
print(fib(32))
AER

cat > "$TMP/fib.py" << 'PY'
def fib(n):
    if n <= 1:
        return n
    return fib(n-1) + fib(n-2)
print(fib(32))
PY

cat > "$TMP/fib.lua" << 'LUA'
local function fib(n)
    if n <= 1 then return n end
    return fib(n-1) + fib(n-2)
end
print(fib(32))
LUA

time_run "AER"    "$AER" "$TMP/fib.aer"
time_run "Python" "$PYTHON" "$TMP/fib.py"
if [ $LUA_AVAILABLE -eq 1 ]; then
    time_run "Lua" "$LUA" "$TMP/fib.lua"
fi

# ── Workload 2: counting loop (1 million iterations) ──────────────────────────

divider
echo "Workload 2 — counting loop (1 000 000 iterations)"
divider

cat > "$TMP/loop.aer" << 'AER'
total = 0
for i in 0..1000000:
    total += i
print(total)
AER

cat > "$TMP/loop.py" << 'PY'
total = 0
for i in range(1_000_000):
    total += i
print(total)
PY

cat > "$TMP/loop.lua" << 'LUA'
local total = 0
for i = 0, 999999 do
    total = total + i
end
print(total)
LUA

time_run "AER"    "$AER" "$TMP/loop.aer"
time_run "Python" "$PYTHON" "$TMP/loop.py"
if [ $LUA_AVAILABLE -eq 1 ]; then
    time_run "Lua" "$LUA" "$TMP/loop.lua"
fi

# ── Workload 3: dict insert + lookup (10 000 entries) ─────────────────────────

divider
echo "Workload 3 — dict insert + lookup (10 000 entries)"
divider

cat > "$TMP/dict.aer" << 'AER'
d = {}
i = 0
for i < 10000:
    d[i as string] = i * i
    i += 1
total = 0
i = 0
for i < 10000:
    total += d[i as string]
    i += 1
print(total)
AER

cat > "$TMP/dict.py" << 'PY'
d = {}
for i in range(10_000):
    d[str(i)] = i * i
total = sum(d[str(i)] for i in range(10_000))
print(total)
PY

cat > "$TMP/dict.lua" << 'LUA'
local d = {}
for i = 0, 9999 do
    d[tostring(i)] = i * i
end
local total = 0
for i = 0, 9999 do
    total = total + d[tostring(i)]
end
print(total)
LUA

time_run "AER"    "$AER" "$TMP/dict.aer"
time_run "Python" "$PYTHON" "$TMP/dict.py"
if [ $LUA_AVAILABLE -eq 1 ]; then
    time_run "Lua" "$LUA" "$TMP/dict.lua"
fi

# ── Workload 4: higher-order functions (apply map-like pattern) ───────────────

divider
echo "Workload 4 — higher-order functions (1000 calls)"
divider

cat > "$TMP/ho.aer" << 'AER'
function double(n):
    return n * 2

function apply(f, arr):
    result = []
    i = 0
    for i < length(arr):
        append(result, f(arr[i]))
        i += 1
    return result

data = []
i = 0
for i < 1000:
    append(data, i)
    i += 1

out = apply(double, data)
print(length(out))
AER

cat > "$TMP/ho.py" << 'PY'
def double(n):
    return n * 2

data = list(range(1000))
out = list(map(double, data))
print(len(out))
PY

cat > "$TMP/ho.lua" << 'LUA'
local function double(n) return n * 2 end
local data = {}
for i = 1, 1000 do data[i] = i end
local out = {}
for i, v in ipairs(data) do out[i] = double(v) end
print(#out)
LUA

time_run "AER"    "$AER" "$TMP/ho.aer"
time_run "Python" "$PYTHON" "$TMP/ho.py"
if [ $LUA_AVAILABLE -eq 1 ]; then
    time_run "Lua" "$LUA" "$TMP/ho.lua"
fi

# ── Workload 5: struct field access (100 000 iterations) ─────────────────────

divider
echo "Workload 5 — struct field access (100 0000 iterations)"
divider

cat > "$TMP/struct.aer" << 'AER'
struct Point:
    x: float = 0.0
    y: float = 0.0

function translate(p, dx, dy):
    return Point(p.x + dx, p.y + dy)

p = Point(0.0, 0.0)
i = 0
for i < 1000000:
    p = translate(p, 1.0, 1.0)
    i += 1
print(p.x)
AER

cat > "$TMP/struct.py" << 'PY'
class Point:
    def __init__(self, x, y):
        self.x = x
        self.y = y

def translate(p, dx, dy):
    return Point(p.x + dx, p.y + dy)


p = Point(0.0, 0.0)
for _ in range(1000000):
    p = translate(p, 1.0, 1.0)
print(p.x)
PY

# No direct struct/record equivalent kept for Python/Lua here — this workload
# exercises AER's struct-field access path specifically, not a cross-language
# comparison. Timed for regression tracking only.
time_run "AER" "$AER" "$TMP/struct.aer"
time_run "Python" "$PYTHON" "$TMP/struct.py"

# ── Workload 6: string concatenation (5 000 appends) ─────────────────────────
# Distinct from every workload above — none of them stress string allocation.
# CPython has a well-known in-place resize fast path for `s += x` when the
# left-hand string's refcount is 1, making repeated concatenation closer to
# O(n) there; if AER always allocates a fresh string per `+=`, this workload
# should expose an O(n^2) pattern Python doesn't pay for.

divider
echo "Workload 6 — string concatenation (5 000 appends)"
divider

cat > "$TMP/strcat.aer" << 'AER'
s = ""
i = 0
for i < 5000:
    s += "x"
    i += 1
print(length(s))
AER

cat > "$TMP/strcat.py" << 'PY'
s = ""
for i in range(5000):
    s += "x"
print(len(s))
PY

cat > "$TMP/strcat.lua" << 'LUA'
local s = ""
for i = 1, 5000 do
    s = s .. "x"
end
print(#s)
LUA

time_run "AER"    "$AER" "$TMP/strcat.aer"
time_run "Python" "$PYTHON" "$TMP/strcat.py"
if [ $LUA_AVAILABLE -eq 1 ]; then
    time_run "Lua" "$LUA" "$TMP/strcat.lua"
fi

# ── Workload 7: function-call overhead (1 000 000 tiny calls) ────────────────
# Isolates CALL/RETURN cost specifically — unlike fib (recursion-heavy) or the
# counting loop (no calls at all), this is one non-recursive call per
# iteration and nothing else, so any per-call fixed overhead (frame setup,
# scope push/pop) dominates whatever the rest of the benchmark suite shows.

divider
echo "Workload 7 — function-call overhead (1 000 000 tiny calls)"
divider

cat > "$TMP/call.aer" << 'AER'
function add_one(n):
    return n + 1

total = 0
i = 0
for i < 1000000:
    total = add_one(total)
    i += 1
print(total)
AER

cat > "$TMP/call.py" << 'PY'
def add_one(n):
    return n + 1

total = 0
for i in range(1_000_000):
    total = add_one(total)
print(total)
PY

cat > "$TMP/call.lua" << 'LUA'
local function add_one(n) return n + 1 end
local total = 0
for i = 1, 1000000 do
    total = add_one(total)
end
print(total)
LUA

time_run "AER"    "$AER" "$TMP/call.aer"
time_run "Python" "$PYTHON" "$TMP/call.py"
if [ $LUA_AVAILABLE -eq 1 ]; then
    time_run "Lua" "$LUA" "$TMP/call.lua"
fi

divider
echo "Done."
divider
