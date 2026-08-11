import time

N = 4000000
GROUPS = 8

region = [0] * N
quantity = [0] * N
price = [0.0] * N
discount = [0.0] * N

for i in range(N):
    region[i] = i % GROUPS
    quantity[i] = (i % 50) + 1
    price[i] = (i % 1000) * 0.5 + 1.0
    discount[i] = (i % 20) * 0.01

revenue = [0.0] * GROUPS
counts = [0] * GROUPS

start = time.perf_counter()
matched = 0
for i in range(N):
    q = quantity[i]
    p = price[i]
    if q > 10 and p < 400.0:
        g = region[i]
        revenue[g] = revenue[g] + p * q * (1.0 - discount[i])
        counts[g] = counts[g] + 1
        matched += 1
dur = time.perf_counter() - start

total = 0.0
for g in range(GROUPS):
    total += revenue[g]
print("columnar %d rows: %fs, matched=%d, revenue=%g" % (N, dur, matched, total))
