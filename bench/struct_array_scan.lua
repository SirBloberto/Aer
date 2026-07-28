local N = 2000000
local PASSES = 50

local function make_particles(n)
    local particles = {}
    local i = 0
    while i < n do
        local p = { x = 0.0, y = 0.0, z = 0.0, vx = 0.0, vy = 0.0, vz = 0.0, mass = 0.0 }
        p.x = i + 0.0
        p.vx = 0.0001
        p.mass = 1.0
        particles[i + 1] = p
        i = i + 1
    end
    return particles
end

local function advance_pass(particles, n, dt)
    local i = 1
    while i <= n do
        local pi = particles[i]
        local vx = pi.vx
        local vy = pi.vy
        local vz = pi.vz
        pi.x = pi.x + vx * dt
        pi.y = pi.y + vy * dt
        pi.z = pi.z + vz * dt
        i = i + 1
    end
end

local particles = make_particles(N)
local start = os.clock()
local p = 0
while p < PASSES do
    advance_pass(particles, N, 0.01)
    p = p + 1
end
local dur = os.clock() - start
print(string.format("struct_array_scan: %d particles x %d passes in %fs, x[0]=%s", N, PASSES, dur, tostring(particles[1].x)))
