import time

start = time.perf_counter()
h = {}
i = 0
while i < 800000:
    h[f"key_{i}"] = i
    i += 1
total = 0
i = 0
while i < 800000:
    total += h[f"key_{i}"]
    i += 1
dur = time.perf_counter() - start
print(f"dict 200k insert+lookup: {dur}s, sum={total}")
