"""Several query shapes, not just the one, so a win on a single benchmark cannot pass for a win.

Each shape stresses a different part of the engine -- a scatter, a reduction, a scan with no
grouping, a sort, a multi-key grouping built by arithmetic -- and each is run through AER twice: the
loop a person writes and the whole-array form. Every engine's answer is checked identical before any
time is believed, exactly as compare_frameworks.py does.

Deliberately runs one size (16M by default). The size sweep lives in compare_frameworks.py; what
this is for is coverage.
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time

GROUPS = 8

PRELUDE_AER = """import time
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
rows = length(quantity)
"""

# Each entry: (name, aer_loop_body, aer_array_body, numpy, pandas, polars, spark).
# Every body sets `answer` (AER prints it; the Python ones return it).
QUERIES = {}

QUERIES["filtered group sum"] = (
    """
revenue = [0.0; GROUPS]
start = time.now()
for i in 0..rows:
    q = quantity[i]
    p = price[i]
    if q > 10.0 and p < 400.0:
        g = integer(region[i])
        revenue[g] = revenue[g] + p * q * (1.0 - discount[i])
dur = time.now() - start
answer = 0.0
for gi in 0..GROUPS:
    answer += revenue[gi]
""",
    """
start = time.now()
revenue = collection.group_sum(
    price * quantity * (1.0 - discount) * (quantity > 10.0) * (price < 400.0), region, GROUPS)
dur = time.now() - start
answer = 0.0
for gi in 0..GROUPS:
    answer += revenue[gi]
""",
    "keep = (quantity > 10.0) & (price < 400.0)\n"
    "answer = float(np.bincount(region[keep].astype(np.int64), "
    "weights=price[keep] * quantity[keep] * (1.0 - discount[keep]), minlength=G).sum())",
    "d = df[(df['quantity'] > 10.0) & (df['price'] < 400.0)]\n"
    "answer = float((d['price'] * d['quantity'] * (1.0 - d['discount'])).groupby(d['region']).sum().sum())",
    "answer = float(df.filter((pl.col('quantity') > 10.0) & (pl.col('price') < 400.0))"
    ".group_by('region').agg((pl.col('price') * pl.col('quantity') * (1.0 - pl.col('discount'))).sum()"
    ".alias('r'))['r'].sum())",
    "answer = float(df.filter((F.col('quantity') > 10.0) & (F.col('price') < 400.0))"
    ".groupBy('region').agg(F.sum(F.col('price') * F.col('quantity') * (1.0 - F.col('discount')))"
    ".alias('r')).agg(F.sum('r').alias('t')).collect()[0]['t'])",
)

QUERIES["group sum, no filter"] = (
    """
revenue = [0.0; GROUPS]
start = time.now()
for i in 0..rows:
    g = integer(region[i])
    revenue[g] = revenue[g] + price[i] * quantity[i]
dur = time.now() - start
answer = 0.0
for gi in 0..GROUPS:
    answer += revenue[gi]
""",
    """
start = time.now()
revenue = collection.group_sum(price * quantity, region, GROUPS)
dur = time.now() - start
answer = 0.0
for gi in 0..GROUPS:
    answer += revenue[gi]
""",
    "answer = float(np.bincount(region.astype(np.int64), weights=price * quantity, minlength=G).sum())",
    "answer = float((df['price'] * df['quantity']).groupby(df['region']).sum().sum())",
    "answer = float(df.group_by('region').agg((pl.col('price') * pl.col('quantity')).sum().alias('r'))"
    "['r'].sum())",
    "answer = float(df.groupBy('region').agg(F.sum(F.col('price') * F.col('quantity')).alias('r'))"
    ".agg(F.sum('r').alias('t')).collect()[0]['t'])",
)

QUERIES["scan, no grouping"] = (
    """
start = time.now()
answer = 0.0
for i in 0..rows:
    answer = answer + price[i] * quantity[i] * (1.0 - discount[i])
dur = time.now() - start
""",
    """
start = time.now()
answer = collection.sum(price * quantity * (1.0 - discount))
dur = time.now() - start
""",
    "answer = float((price * quantity * (1.0 - discount)).sum())",
    "answer = float((df['price'] * df['quantity'] * (1.0 - df['discount'])).sum())",
    "answer = float(df.select((pl.col('price') * pl.col('quantity') * (1.0 - pl.col('discount'))).sum()"
    ".alias('r'))['r'][0])",
    "answer = float(df.agg(F.sum(F.col('price') * F.col('quantity') * (1.0 - F.col('discount')))"
    ".alias('t')).collect()[0]['t'])",
)

QUERIES["filtered count"] = (
    """
start = time.now()
answer = 0.0
for i in 0..rows:
    if quantity[i] > 25.0 and price[i] < 200.0:
        answer = answer + 1.0
dur = time.now() - start
""",
    """
start = time.now()
answer = collection.sum((quantity > 25.0) * (price < 200.0))
dur = time.now() - start
""",
    "answer = float(((quantity > 25.0) & (price < 200.0)).sum())",
    "answer = float(((df['quantity'] > 25.0) & (df['price'] < 200.0)).sum())",
    "answer = float(df.filter((pl.col('quantity') > 25.0) & (pl.col('price') < 200.0)).height)",
    "answer = float(df.filter((F.col('quantity') > 25.0) & (F.col('price') < 200.0)).count())",
)

QUERIES["min and max of an expression"] = (
    """
start = time.now()
lo = price[0] * quantity[0]
hi = lo
for i in 0..rows:
    v = price[i] * quantity[i]
    if v < lo:
        lo = v
    if v > hi:
        hi = v
dur = time.now() - start
answer = lo + hi
""",
    """
start = time.now()
v = price * quantity
answer = collection.min(v) + collection.max(v)
dur = time.now() - start
""",
    "v = price * quantity\nanswer = float(v.min() + v.max())",
    "v = df['price'] * df['quantity']\nanswer = float(v.min() + v.max())",
    "o = df.select((pl.col('price') * pl.col('quantity')).alias('v'))\n"
    "answer = float(o['v'].min() + o['v'].max())",
    "o = df.select((F.col('price') * F.col('quantity')).alias('v'))"
    ".agg(F.min('v').alias('a'), F.max('v').alias('b')).collect()[0]\n"
    "answer = float(o['a'] + o['b'])",
)

# Two grouping keys, which AER has no syntax for -- so it builds the composite key with arithmetic,
# the same thing a query engine does internally.
QUERIES["two-key group sum"] = (
    """
K = GROUPS * 51
totals = [0.0; K]
start = time.now()
for i in 0..rows:
    k = integer(region[i]) * 51 + integer(quantity[i])
    totals[k] = totals[k] + price[i]
dur = time.now() - start
answer = 0.0
for gi in 0..K:
    answer += totals[gi]
""",
    """
K = GROUPS * 51
start = time.now()
totals = collection.group_sum(price, region * 51.0 + quantity, K)
dur = time.now() - start
answer = 0.0
for gi in 0..K:
    answer += totals[gi]
""",
    "k = region.astype(np.int64) * 51 + quantity.astype(np.int64)\n"
    "answer = float(np.bincount(k, weights=price, minlength=G * 51).sum())",
    "k = df['region'].astype('int64') * 51 + df['quantity'].astype('int64')\n"
    "answer = float(df['price'].groupby(k).sum().sum())",
    "answer = float(df.group_by(['region', pl.col('quantity').cast(pl.Int64).alias('k')])"
    ".agg(pl.col('price').sum().alias('r'))['r'].sum())",
    "answer = float(df.groupBy('region', F.col('quantity').cast('long').alias('k'))"
    ".agg(F.sum('price').alias('r')).agg(F.sum('r').alias('t')).collect()[0]['t'])",
)

QUERIES["sort a column"] = (
    None,  # a hand-written sort is not what anyone would compare against
    """
start = time.now()
v = collection.copy(price)
collection.sort(v)
dur = time.now() - start
answer = v[0] + v[length(v) - 1]
""",
    "v = np.sort(price)\nanswer = float(v[0] + v[-1])",
    "v = df['price'].sort_values().to_numpy()\nanswer = float(v[0] + v[-1])",
    "v = df['price'].sort()\nanswer = float(v[0] + v[len(v) - 1])",
    "v = df.select(F.col('price')).orderBy('price').collect()\n"
    "answer = float(v[0]['price'] + v[len(v) - 1]['price'])",
)

# AER prints floats at six significant digits, which cannot carry a 1e11 aggregate, so the answer
# crosses as a scaled integer -- otherwise two engines computing different things look identical.
AER_TEMPLATE = PRELUDE_AER + """{body}
print("RESULT {{dur}} {{integer(answer * 1000.0)}}")
"""

PY_SETUP = {
    "numpy": """
import numpy as np
G = {g}
i = np.arange(n, dtype=np.int64)
region = (i % G).astype(np.float64)
quantity = ((i % 50) + 1).astype(np.float64)
price = (i % 1000).astype(np.float64) * 0.5 + 1.0
discount = (i % 20).astype(np.float64) * 0.01
del i
""",
    "pandas": """
import numpy as np, pandas as pd
G = {g}
i = np.arange(n, dtype=np.int64)
df = pd.DataFrame({{"region": (i % G).astype(np.float64),
                   "quantity": ((i % 50) + 1).astype(np.float64),
                   "price": (i % 1000).astype(np.float64) * 0.5 + 1.0,
                   "discount": (i % 20).astype(np.float64) * 0.01}})
del i
""",
    "polars": """
import numpy as np, polars as pl
G = {g}
i = np.arange(n, dtype=np.int64)
df = pl.DataFrame({{"region": (i % G).astype(np.float64),
                   "quantity": ((i % 50) + 1).astype(np.float64),
                   "price": (i % 1000).astype(np.float64) * 0.5 + 1.0,
                   "discount": (i % 20).astype(np.float64) * 0.01}})
del i
""",
    "spark": """
from pyspark.sql import SparkSession, functions as F
G = {g}
spark = (SparkSession.builder.appName("aer-q").master("local[*]")
         .config("spark.driver.memory", "8g").config("spark.sql.shuffle.partitions", "12")
         .config("spark.ui.enabled", "false").getOrCreate())
spark.sparkContext.setLogLevel("ERROR")
df = spark.range(0, n).select(
    (F.col("id") % G).cast("double").alias("region"),
    ((F.col("id") % 50) + 1).cast("double").alias("quantity"),
    ((F.col("id") % 1000).cast("double") * 0.5 + 1.0).alias("price"),
    ((F.col("id") % 20).cast("double") * 0.01).alias("discount")).cache()
df.count()
""",
}

PY_TEMPLATE = """
import json, sys, time
n = int(sys.argv[1]); repeat = int(sys.argv[2])
{setup}
def run():
{body}
    return answer
best, answer = None, None
for _ in range(repeat):
    t = time.perf_counter(); answer = run(); e = time.perf_counter() - t
    best = e if best is None else min(best, e)
print("RESULT " + json.dumps([best, answer]))
"""


def run_aer(binary, body, n, repeat):
    src = AER_TEMPLATE.format(n=n, g=GROUPS, body=body)
    path = os.path.join(tempfile.gettempdir(), "aer_q_%d.aer" % os.getpid())
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(src)
    best, answer = None, None
    try:
        for _ in range(repeat):
            out = subprocess.run([binary, path], capture_output=True, text=True, timeout=3600)
            line = [x for x in out.stdout.splitlines() if x.startswith("RESULT")]
            if not line:
                raise RuntimeError((out.stdout + out.stderr).strip().splitlines()[-1][:160])
            _, dur, ans = line[0].split()
            best = float(dur) if best is None else min(best, float(dur))
            answer = float(ans) / 1000.0
    finally:
        if os.path.exists(path):
            os.remove(path)
    return best, answer


def run_python(engine, body, n, repeat):
    indented = "\n".join("    " + l for l in body.strip().splitlines())
    src = PY_TEMPLATE.format(setup=PY_SETUP[engine].format(g=GROUPS), body=indented)
    path = os.path.join(tempfile.gettempdir(), "aer_q_%d.py" % os.getpid())
    with open(path, "w", encoding="utf-8") as f:
        f.write(src)
    try:
        out = subprocess.run(
            [sys.executable, path, str(n), str(repeat)], capture_output=True, text=True, timeout=3600
        )
        line = [x for x in out.stdout.splitlines() if x.startswith("RESULT")]
        if not line:
            raise RuntimeError((out.stdout[-1500:] + out.stderr[-3000:]).strip().splitlines()[-1][:160])
        best, answer = json.loads(line[0][len("RESULT ") :])
        return best, float(answer)
    finally:
        if os.path.exists(path):
            os.remove(path)


ENGINES = ["AER (loop)", "AER (array)", "NumPy", "pandas", "Polars", "Spark"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=os.path.join("binary", "aer.exe"))
    ap.add_argument("--rows", type=int, default=16000000)
    ap.add_argument("--repeat", type=int, default=3)
    ap.add_argument("--only", default="")
    args = ap.parse_args()

    names = [q for q in QUERIES if not args.only or args.only.lower() in q.lower()]
    table = {}
    for qname in names:
        loop, array, npy, pdz, plz, spk = QUERIES[qname]
        print("\n=== %s (%s rows) ===" % (qname, f"{args.rows:,}"), flush=True)
        answers = {}
        for engine, body, runner in [
            ("AER (loop)", loop, "aer"),
            ("AER (array)", array, "aer"),
            ("NumPy", npy, "numpy"),
            ("pandas", pdz, "pandas"),
            ("Polars", plz, "polars"),
            ("Spark", spk, "spark"),
        ]:
            if body is None:
                print("  %-14s --  (not a meaningful hand-written comparison)" % engine, flush=True)
                continue
            try:
                if runner == "aer":
                    secs, answer = run_aer(args.binary, body, args.rows, args.repeat)
                else:
                    secs, answer = run_python(runner, body, args.rows, args.repeat)
            except Exception as exc:
                print("  %-14s FAILED  %s" % (engine, str(exc)[:110]), flush=True)
                continue
            table[(qname, engine)] = secs
            answers[engine] = answer
            print("  %-14s %9.4fs   answer=%.10g" % (engine, secs, answer), flush=True)
        vals = list(answers.values())
        if vals and max(abs(v - vals[0]) for v in vals) > max(1e-6, abs(vals[0]) * 1e-9):
            print("  !! ANSWERS DISAGREE: %s" % answers, flush=True)

    print("\n\n=== seconds, best of %d, %s rows ===" % (args.repeat, f"{args.rows:,}"))
    print("%-26s" % "query" + "".join("%13s" % e for e in ENGINES))
    for qname in names:
        row = "%-26s" % qname[:26]
        for e in ENGINES:
            v = table.get((qname, e))
            row += "%13s" % ("--" if v is None else "%.4f" % v)
        print(row)

    print("\n=== AER (array) against each, >1 means AER is faster ===")
    print("%-26s" % "query" + "".join("%13s" % e for e in ENGINES if e != "AER (array)"))
    for qname in names:
        row = "%-26s" % qname[:26]
        base = table.get((qname, "AER (array)"))
        for e in ENGINES:
            if e == "AER (array)":
                continue
            v = table.get((qname, e))
            row += "%13s" % ("--" if not v or not base else "%.2fx" % (v / base))
        print(row)


if __name__ == "__main__":
    main()
