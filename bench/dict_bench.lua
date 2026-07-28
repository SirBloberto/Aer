local start = os.clock()
local h = {}
local i = 0
while i < 200000 do
    h["key_" .. i] = i
    i = i + 1
end
local sum = 0
i = 0
while i < 200000 do
    sum = sum + h["key_" .. i]
    i = i + 1
end
local dur = os.clock() - start
print(string.format("dict 200k insert+lookup: %fs, sum=%d", dur, sum))
