-- Differential fixture — computes what strings.aer computes. AER indexes from 0 with exclusive
-- slice ends; Lua indexes from 1 with inclusive ones, so the calls below are translated rather
-- than copied. The claim under test is that the two agree on the resulting values.

local s = "Hello, World"

print(#s)
print(string.upper(s))
print(string.lower(s))
print((("   padded   "):gsub("^%s*(.-)%s*$", "%1")))
print(string.find(s, "World", 1, true) ~= nil)
print(string.find(s, "world", 1, true) ~= nil)
print(s:sub(1, #"Hello") == "Hello")
print(s:sub(-#"World") == "World")
-- AER's index_of is a 0-based byte offset, and -1 when absent.
local function index_of(hay, needle)
  local i = string.find(hay, needle, 1, true)
  if i == nil then return -1 end
  return i - 1
end
print(index_of(s, "o"))
print(index_of(s, "zz"))
print((s:gsub("World", "there")))
print(("ab"):rep(3))

print(s:sub(1, 5))
print(s:sub(8))
print(s:sub(1, 5))
print(s:sub(-5))

local parts = {}
for piece in ("a,b,c,d"):gmatch("[^,]+") do parts[#parts + 1] = piece end
print(#parts)
print(parts[1])
print(parts[4])
print(table.concat(parts, "-"))

local n = 42
print("n is " .. string.upper("ok"))
print("interp " .. n .. " and " .. s:sub(1, 5))

local count = 0
for i = 1, #s do
  if s:sub(i, i) == "o" then count = count + 1 end
end
print(count)

-- AER's collection.sort on strings orders by byte value, which is Lua's default too.
local words = {"pear", "Apple", "banana", "apple"}
table.sort(words)
print(table.concat(words, ","))

local nums = {5, 3, 9, 1, 7}
table.sort(nums)
local digits = ""
for _, v in ipairs(nums) do digits = digits .. tostring(v) end
print(digits)

local function array_index_of(arr, want)
  for i, v in ipairs(arr) do
    if v == want then return i - 1 end
  end
  return -1
end
local letters = {"x", "y", "z"}
print(array_index_of(letters, "y"))
print(array_index_of(letters, "q"))
print(array_index_of(letters, "y") >= 0)
print(array_index_of(letters, "q") >= 0)
