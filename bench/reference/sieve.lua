local function sieve(limit)
    local is_composite = {}
    for i = 0, limit do
        is_composite[i] = false
    end

    local count = 0
    local p = 2
    while p <= limit do
        if not is_composite[p] then
            count = count + 1
            local multiple = p * p
            while multiple <= limit do
                is_composite[multiple] = true
                multiple = multiple + p
            end
        end
        p = p + 1
    end
    return count
end

local LIMIT = 10000000
local result = sieve(LIMIT)
print(string.format("Primes up to %d: %d", LIMIT, result))
