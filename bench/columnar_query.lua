local N = 4000000
local GROUPS = 8

local region = {}
local quantity = {}
local price = {}
local discount = {}

for i = 0, N - 1 do
    region[i] = i % GROUPS
    quantity[i] = (i % 50) + 1
    price[i] = (i % 1000) * 0.5 + 1.0
    discount[i] = (i % 20) * 0.01
end

local revenue = {}
local counts = {}
for g = 0, GROUPS - 1 do
    revenue[g] = 0.0
    counts[g] = 0
end

local start = os.clock()
local matched = 0
for i = 0, N - 1 do
    local q = quantity[i]
    local p = price[i]
    if q > 10 and p < 400.0 then
        local g = region[i]
        revenue[g] = revenue[g] + p * q * (1.0 - discount[i])
        counts[g] = counts[g] + 1
        matched = matched + 1
    end
end
local dur = os.clock() - start

local total = 0.0
for g = 0, GROUPS - 1 do
    total = total + revenue[g]
end
print(string.format("columnar %d rows: %fs, matched=%d, revenue=%g", N, dur, matched, total))
