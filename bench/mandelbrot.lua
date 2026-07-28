local SIZE = 800
local MAX_ITER = 200

local function mandelbrot_point(cx, cy, max_iter)
    local x = 0.0
    local y = 0.0
    local iter = 0
    while iter < max_iter do
        local x2 = x * x
        local y2 = y * y
        if x2 + y2 > 4.0 then
            return iter
        end
        local y_new = 2.0 * x * y + cy
        local x_new = x2 - y2 + cx
        y = y_new
        x = x_new
        iter = iter + 1
    end
    return max_iter
end

local start = os.clock()
local inside_count = 0
local py = 0
while py < SIZE do
    local px = 0
    while px < SIZE do
        local cx = px / SIZE * 3.5 - 2.5
        local cy = py / SIZE * 2.0 - 1.0
        if mandelbrot_point(cx, cy, MAX_ITER) == MAX_ITER then
            inside_count = inside_count + 1
        end
        px = px + 1
    end
    py = py + 1
end
local dur = os.clock() - start
print(string.format("mandelbrot %dx%d: %fs, inside=%d", SIZE, SIZE, dur, inside_count))
