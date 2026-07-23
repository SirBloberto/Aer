def sieve(limit):
    is_composite = [False] * (limit + 1)

    count = 0
    p = 2
    while p <= limit:
        if not is_composite[p]:
            count += 1
            multiple = p * p
            while multiple <= limit:
                is_composite[multiple] = True
                multiple += p
        p += 1
    return count

LIMIT = 10000000
result = sieve(LIMIT)
print("Primes up to {}: {}".format(LIMIT, result))
