local start = os.clock()
local total = 0
local i = 0
while i < 200000 do
    local rec = { id = i, name = "item_" .. i, active = true, score = i * 2 }
    total = total + rec.id + rec.score
    i = i + 1
end
local dur = os.clock() - start
print(string.format("200k small (4-key) dicts, build+read: %fs, total=%d", dur, total))
