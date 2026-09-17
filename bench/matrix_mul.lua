-- Nested tables are Lua's dense-matrix idiom: there is no typed-array type to match AER's rows, so
-- this is the fair counterpart rather than a handicap.
local function matmul(a, b, n)
    local c = {}
    for i = 0, n - 1 do
        c[i] = {}
        for j = 0, n - 1 do
            local s = 0.0
            for k = 0, n - 1 do
                s = s + a[i][k] * b[k][j]
            end
            c[i][j] = s
        end
    end
    return c
end

local N = 256

local a = {}
local b = {}
for i = 0, N - 1 do
    a[i] = {}
    b[i] = {}
    for j = 0, N - 1 do
        a[i][j] = ((i + j) % 7) * 0.5
        b[i][j] = ((i * j) % 5) * 0.25
    end
end

local c = matmul(a, b, N)

local trace = 0.0
for d = 0, N - 1 do
    trace = trace + c[d][d]
end
print(string.format("matmul %dx%d: trace=%g", N, N, trace))
