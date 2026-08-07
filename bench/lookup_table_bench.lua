local h = {}
local i = 0
while i < 500 do
    h["key_" .. i] = i * 3
    i = i + 1
end

local start = os.clock()
local total = 0
local n = 0
while n < 8500000 do
    total = total + h["key_" .. (n % 500)]
    n = n + 1
end
local dur = os.clock() - start
print(string.format("500-key table, 400k repeated lookups: %fs, total=%d", dur, total))
