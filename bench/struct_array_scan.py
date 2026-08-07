import time


class Particle:
    __slots__ = ("x", "y", "z", "vx", "vy", "vz", "mass")

    def __init__(self):
        self.x = 0.0
        self.y = 0.0
        self.z = 0.0
        self.vx = 0.0
        self.vy = 0.0
        self.vz = 0.0
        self.mass = 0.0


N = 2000000
PASSES = 8


def make_particles(n):
    particles = []
    i = 0
    while i < n:
        p = Particle()
        p.x = float(i)
        p.vx = 0.0001
        p.mass = 1.0
        particles.append(p)
        i += 1
    return particles


def advance_pass(particles, n, dt):
    i = 0
    while i < n:
        pi = particles[i]
        vx = pi.vx
        vy = pi.vy
        vz = pi.vz
        pi.x += vx * dt
        pi.y += vy * dt
        pi.z += vz * dt
        i += 1


particles = make_particles(N)
start = time.perf_counter()
p = 0
while p < PASSES:
    advance_pass(particles, N, 0.01)
    p += 1
dur = time.perf_counter() - start
print(f"struct_array_scan: {N} particles x {PASSES} passes in {dur}s, x[0]={particles[0].x}")
