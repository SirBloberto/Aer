def sieve(limit):
    # bytearray, not a list of bools -- Python's own idiomatic dense-flag-array choice (one byte
    # per flag, no per-element object boxing), the fair counterpart to AER's int32 typed array.
    is_composite = bytearray(limit + 1)

    count = 0
    p = 2
    while p <= limit:
        if not is_composite[p]:
            count += 1
            multiple = p * p
            while multiple <= limit:
                is_composite[multiple] = 1
                multiple += p
        p += 1
    return count

LIMIT = 13000000
result = sieve(LIMIT)
print("Primes up to {}: {}".format(LIMIT, result))
