"""AER against the dataframe engines, on one query at several sizes.

    SELECT region, SUM(price * quantity * (1 - discount)), COUNT(*)
    FROM sales WHERE quantity > 10 AND price < 400 GROUP BY region

Every engine builds the same four columns from the same formula, and only the QUERY is timed --
loading is a separate concern and Spark's would swamp everything else. Spark runs local[*] in its own
process so the JVM's startup and memory never land on another engine's measurement.

Two AER rows: the loop a person writes, and the whole-array form the compiler is being taught to
produce from it. The gap between them is what the remaining vectorization work is worth.
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time

GROUPS = 8
QUERY_TEXT = "quantity > 10 AND price < 400, grouped by region"

AER_LOOP = """import time

N = {n}
GROUPS = {g}
region = [0.0; N]
quantity = [0.0; N]
price = [0.0; N]
discount = [0.0; N]
i = 0
for i < N:
    region[i] = float(i % GROUPS)
    quantity[i] = float((i % 50) + 1)
    price[i] = float(i % 1000) * 0.5 + 1.0
    discount[i] = float(i % 20) * 0.01
    i += 1

revenue = [0.0; GROUPS]
counts = [0.0; GROUPS]
matched = 0.0
rows = length(quantity)
start = time.now()
for i in 0..rows:
    q = quantity[i]
    p = price[i]
    if q > 10.0 and p < 400.0:
        g = integer(region[i])
        revenue[g] = revenue[g] + p * q * (1.0 - discount[i])
        counts[g] = counts[g] + 1.0
        matched = matched + 1.0
dur = time.now() - start

total = 0.0
for gi in 0..GROUPS:
    total += revenue[gi]
print("RESULT {{dur}} {{matched}} {{integer(total * 100.0)}}")
"""

AER_ARRAY = """import time
import collection

N = {n}
GROUPS = {g}
region = [0.0; N]
quantity = [0.0; N]
price = [0.0; N]
discount = [0.0; N]
i = 0
for i < N:
    region[i] = float(i % GROUPS)
    quantity[i] = float((i % 50) + 1)
    price[i] = float(i % 1000) * 0.5 + 1.0
    discount[i] = float(i % 20) * 0.01
    i += 1

start = time.now()
mask = (quantity > 10.0) * (price < 400.0)
revenue = collection.group_sum(price * quantity * (1.0 - discount) * mask, region, GROUPS)
counts = collection.group_sum(mask, region, GROUPS)
matched = collection.sum(mask)
dur = time.now() - start

total = 0.0
for gi in 0..GROUPS:
    total += revenue[gi]
print("RESULT {{dur}} {{matched}} {{integer(total * 100.0)}}")
"""


def run_aer(binary, source, n, repeat):
    path = os.path.join(tempfile.gettempdir(), "aer_cmp_%d.aer" % os.getpid())
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(source.format(n=n, g=GROUPS))
    best, answer = None, None
    try:
        for _ in range(repeat):
            out = subprocess.run([binary, path], capture_output=True, text=True, timeout=1800)
            line = [x for x in out.stdout.splitlines() if x.startswith("RESULT")]
            if not line:
                raise RuntimeError("aer produced no result:\n%s%s" % (out.stdout, out.stderr))
            _, dur, matched, total = line[0].split()
            total = float(total) / 100.0  # AER prints floats at six significant digits
            best = float(dur) if best is None else min(best, float(dur))
            answer = (float(matched), float(total))
    finally:
        if os.path.exists(path):
            os.remove(path)
    return best, answer


NUMPY_BODY = """
import numpy as np
i = np.arange(n, dtype=np.int64)
region = (i % {g}).astype(np.float64)
quantity = ((i % 50) + 1).astype(np.float64)
price = (i % 1000).astype(np.float64) * 0.5 + 1.0
discount = (i % 20).astype(np.float64) * 0.01
del i
run = lambda: _numpy_query(region, quantity, price, discount)

def _numpy_query(region, quantity, price, discount):
    keep = (quantity > 10.0) & (price < 400.0)
    g = region[keep].astype(np.int64)
    v = price[keep] * quantity[keep] * (1.0 - discount[keep])
    revenue = np.bincount(g, weights=v, minlength={g})
    return float(keep.sum()), float(revenue.sum())
"""

PANDAS_BODY = """
import numpy as np, pandas as pd
i = np.arange(n, dtype=np.int64)
df = pd.DataFrame({{
    "region": (i % {g}).astype(np.float64),
    "quantity": ((i % 50) + 1).astype(np.float64),
    "price": (i % 1000).astype(np.float64) * 0.5 + 1.0,
    "discount": (i % 20).astype(np.float64) * 0.01,
}})
del i

def run():
    d = df[(df["quantity"] > 10.0) & (df["price"] < 400.0)]
    rev = (d["price"] * d["quantity"] * (1.0 - d["discount"])).groupby(d["region"]).sum()
    return float(len(d)), float(rev.sum())
"""

POLARS_BODY = """
import numpy as np, polars as pl
i = np.arange(n, dtype=np.int64)
df = pl.DataFrame({{
    "region": (i % {g}).astype(np.float64),
    "quantity": ((i % 50) + 1).astype(np.float64),
    "price": (i % 1000).astype(np.float64) * 0.5 + 1.0,
    "discount": (i % 20).astype(np.float64) * 0.01,
}})
del i

def run():
    out = (
        df.filter((pl.col("quantity") > 10.0) & (pl.col("price") < 400.0))
        .group_by("region")
        .agg(
            (pl.col("price") * pl.col("quantity") * (1.0 - pl.col("discount"))).sum().alias("rev"),
            pl.len().alias("n"),
        )
    )
    return float(out["n"].sum()), float(out["rev"].sum())
"""

SPARK_BODY = """
from pyspark.sql import SparkSession, functions as F
spark = (
    SparkSession.builder.appName("aer-cmp")
    .master("local[*]")
    .config("spark.driver.memory", "8g")
    .config("spark.sql.shuffle.partitions", "12")
    .config("spark.ui.enabled", "false")
    .getOrCreate()
)
spark.sparkContext.setLogLevel("ERROR")
df = (
    spark.range(0, n)
    .select(
        (F.col("id") % {g}).cast("double").alias("region"),
        ((F.col("id") % 50) + 1).cast("double").alias("quantity"),
        ((F.col("id") % 1000).cast("double") * 0.5 + 1.0).alias("price"),
        ((F.col("id") % 20).cast("double") * 0.01).alias("discount"),
    )
).cache()
df.count()  # force the cache before timing

def run():
    out = (
        df.filter((F.col("quantity") > 10.0) & (F.col("price") < 400.0))
        .groupBy("region")
        .agg(
            F.sum(F.col("price") * F.col("quantity") * (1.0 - F.col("discount"))).alias("rev"),
            F.count(F.lit(1)).alias("n"),
        )
        .agg(F.sum("n").alias("n"), F.sum("rev").alias("rev"))
        .collect()[0]
    )
    return float(out["n"]), float(out["rev"])
"""

RUNNER = """
import json, sys, time
n = int(sys.argv[1])
repeat = int(sys.argv[2])
{body}
best, answer = None, None
for _ in range(repeat):
    t = time.perf_counter()
    answer = run()
    e = time.perf_counter() - t
    best = e if best is None else min(best, e)
print("RESULT " + json.dumps([best, answer[0], answer[1]]))
"""


def run_python(body, n, repeat):
    src = RUNNER.format(body=body.format(g=GROUPS))
    path = os.path.join(tempfile.gettempdir(), "aer_cmp_%d.py" % os.getpid())
    with open(path, "w", encoding="utf-8") as f:
        f.write(src)
    try:
        out = subprocess.run(
            [sys.executable, path, str(n), str(repeat)], capture_output=True, text=True, timeout=3600
        )
        line = [x for x in out.stdout.splitlines() if x.startswith("RESULT")]
        if not line:
            raise RuntimeError(out.stdout[-2000:] + out.stderr[-4000:])
        best, matched, total = json.loads(line[0][len("RESULT ") :])
        return best, (matched, total)
    finally:
        if os.path.exists(path):
            os.remove(path)


ENGINES = [
    ("AER (loop)", lambda b, n, r: run_aer(b, AER_LOOP, n, r)),
    ("AER (array)", lambda b, n, r: run_aer(b, AER_ARRAY, n, r)),
    ("NumPy", lambda b, n, r: run_python(NUMPY_BODY, n, r)),
    ("pandas", lambda b, n, r: run_python(PANDAS_BODY, n, r)),
    ("Polars", lambda b, n, r: run_python(POLARS_BODY, n, r)),
    ("Spark (local[*])", lambda b, n, r: run_python(SPARK_BODY, n, r)),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=os.path.join("binary", "aer.exe"))
    ap.add_argument("--sizes", default="1000000,4000000,16000000,32000000")
    ap.add_argument("--repeat", type=int, default=3)
    ap.add_argument("--only", default="", help="comma-separated engine name substrings")
    args = ap.parse_args()

    sizes = [int(s) for s in args.sizes.split(",")]
    engines = [e for e in ENGINES if not args.only or any(o.lower() in e[0].lower() for o in args.only.split(","))]

    results = {}
    for n in sizes:
        print("\n=== %s rows ===" % f"{n:,}", flush=True)
        for name, fn in engines:
            t0 = time.time()
            try:
                secs, answer = fn(args.binary, n, args.repeat)
            except Exception as exc:  # a size an engine cannot hold is a result, not a crash
                print("  %-18s FAILED (%s)" % (name, str(exc).strip().splitlines()[-1][:90]), flush=True)
                results[(n, name)] = None
                continue
            results[(n, name)] = (secs, answer)
            print(
                "  %-18s %8.4fs   matched=%.0f revenue=%.6g   [%.0fs wall]"
                % (name, secs, answer[0], answer[1], time.time() - t0),
                flush=True,
            )

    print("\n\n=== query time in seconds (best of %d) ===" % args.repeat)
    names = [e[0] for e in engines]
    print("%-18s" % "rows" + "".join("%14s" % f"{n:,}" for n in sizes))
    for name in names:
        row = "%-18s" % name
        for n in sizes:
            r = results.get((n, name))
            row += "%14s" % ("--" if r is None else "%.4f" % r[0])
        print(row)

    base = "AER (loop)"
    if base in names:
        print("\n=== speed relative to %s (>1 means faster than AER's loop) ===" % base)
        print("%-18s" % "rows" + "".join("%14s" % f"{n:,}" for n in sizes))
        for name in names:
            row = "%-18s" % name
            for n in sizes:
                r, b = results.get((n, name)), results.get((n, base))
                row += "%14s" % ("--" if not r or not b else "%.2fx" % (b[0] / r[0]))
            print(row)


if __name__ == "__main__":
    main()
