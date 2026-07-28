import time

h = {}
i = 0
while i < 500:
    h[f"key_{i}"] = i * 3
    i += 1

start = time.perf_counter()
total = 0
n = 0
while n < 400000:
    total += h[f"key_{n % 500}"]
    n += 1
dur = time.perf_counter() - start
print(f"500-key table, 400k repeated lookups: {dur}s, total={total}")
