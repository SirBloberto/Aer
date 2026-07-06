import time

limit = 1000000
start = time.perf_counter()
primes = [0] * (limit + 1)
primes[0] = 1
primes[1] = 1
p = 2
while p * p <= limit:
    if primes[p] == 0:
        i = p * p
        while i <= limit:
            primes[i] = 1
            i += p
    p += 1
results = [i for i, val in enumerate(primes) if val == 0]
end = time.perf_counter()
print(f"Manual loop finished in {end - start:.4f} seconds.")
