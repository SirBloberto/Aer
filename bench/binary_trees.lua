local function make_tree(depth)
    if depth == 0 then
        return { left = nil, right = nil, item = 0 }
    end
    local d = depth - 1
    return { left = make_tree(d), right = make_tree(d), item = depth }
end

local function tree_checksum(node)
    if node.left == nil then
        return node.item
    end
    return node.item + tree_checksum(node.left) - tree_checksum(node.right)
end

local MIN_DEPTH = 4
local MAX_DEPTH = 10

local start = os.clock()

local stretch_depth = MAX_DEPTH + 1
local stretch_tree = make_tree(stretch_depth)
local stretch_checksum = tree_checksum(stretch_tree)
print(string.format("stretch tree of depth %d: checksum %d", stretch_depth, stretch_checksum))

local long_lived_tree = make_tree(MAX_DEPTH)

local depth = MIN_DEPTH
while depth <= MAX_DEPTH do
    local iterations = 1
    local p = 0
    while p < depth do
        iterations = iterations * 2
        p = p + 1
    end
    local total_checksum = 0
    local i = 0
    while i < iterations do
        total_checksum = total_checksum + tree_checksum(make_tree(depth))
        i = i + 1
    end
    print(string.format("%d trees of depth %d: checksum %d", iterations, depth, total_checksum))
    depth = depth + 2
end

local long_lived_checksum = tree_checksum(long_lived_tree)
print(string.format("long-lived tree of depth %d: checksum %d", MAX_DEPTH, long_lived_checksum))

local dur = os.clock() - start
print(string.format("binary_trees: %fs", dur))
