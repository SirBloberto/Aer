# Lists of floats, not numpy: numpy would run this in C, and this suite compares interpreters to
# interpreters. A list of lists is the idiomatic pure-Python dense matrix, the same reasoning that
# picked bytearray over a list of bools in sieve.py.
def matmul(a, b, n):
    c = [[0.0] * n for _ in range(n)]
    for i in range(n):
        for j in range(n):
            s = 0.0
            for k in range(n):
                s += a[i][k] * b[k][j]
            c[i][j] = s
    return c


N = 256

a = [[0.0] * N for _ in range(N)]
b = [[0.0] * N for _ in range(N)]
for i in range(N):
    for j in range(N):
        a[i][j] = ((i + j) % 7) * 0.5
        b[i][j] = ((i * j) % 5) * 0.25

c = matmul(a, b, N)

trace = 0.0
for d in range(N):
    trace += c[d][d]
print("matmul {}x{}: trace={:g}".format(N, N, trace))
