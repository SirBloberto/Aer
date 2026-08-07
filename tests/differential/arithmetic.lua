-- Differential fixture — computes exactly what arithmetic.aer computes. Kept literal rather than
-- idiomatic: the point is that two independent implementations agree on the values, so this is a
-- transcription of the same expressions, not a rewrite.

local function scaled(x)
  return math.floor(x * 1000000)
end

-- Lua prints booleans as true/false and integers without a decimal point, matching AER; a float
-- would not, which is why every fractional result goes through scaled().
local function p(v) print(v) end

local a = 7
local b = 3
p(a + b)
p(a - b)
p(a * b)
p(a // b)
p(a % b)
p(-a // b)
p(-a % b)
p(a // -b)
p(a % -b)

local x = 7.5
local y = 2.5
p(scaled(x + y))
p(scaled(x - y))
p(scaled(x * y))
p(scaled(x / y))
p(scaled(x / 3.0))
p(scaled(a / b))

p(2 + 3 * 4)
p((2 + 3) * 4)
p(2 * 3 + 4 * 5)
p(100 - 10 - 5)
p(scaled(1.0 / 2.0 / 2.0))

p(a > b)
p(a < b)
p(a >= 7)
p(a <= 7)
p(a == 7)
p(a ~= 7)
p(7 == 7.0)
p(1 < 2)

p(12 & 10)
p(12 | 10)
p(12 ~ 10)
p(1 << 10)
p(1024 >> 3)

local big = 1000000007
p(big * 3)
p(big + big)
p(big % 1000)

p(math.floor(3.7))
p(math.ceil(3.2))
p(math.abs(-42))
p(math.min(3, 9))
p(math.max(3, 9))
p(scaled(math.sqrt(2.0)))
p(scaled(2.0 ^ 10.0))

-- AER's `for i in 0..100:` is exclusive of the upper bound.
local total = 0
for i = 0, 99 do
  total = total + i * i
end
p(total)

local ftotal = 0.0
for i = 1, 20 do
  ftotal = ftotal + 1.0 / i
end
p(scaled(ftotal))
