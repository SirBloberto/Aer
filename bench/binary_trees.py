import time


class TreeNode:
    __slots__ = ("left", "right", "item")

    def __init__(self, left, right, item):
        self.left = left
        self.right = right
        self.item = item


def make_tree(depth):
    if depth == 0:
        return TreeNode(None, None, 0)
    d = depth - 1
    return TreeNode(make_tree(d), make_tree(d), depth)


def tree_checksum(node):
    if node.left is None:
        return node.item
    return node.item + tree_checksum(node.left) - tree_checksum(node.right)


MIN_DEPTH = 4
MAX_DEPTH = 10

start = time.perf_counter()

stretch_depth = MAX_DEPTH + 1
stretch_tree = make_tree(stretch_depth)
stretch_checksum = tree_checksum(stretch_tree)
print(f"stretch tree of depth {stretch_depth}: checksum {stretch_checksum}")

long_lived_tree = make_tree(MAX_DEPTH)

depth = MIN_DEPTH
while depth <= MAX_DEPTH:
    iterations = 1
    p = 0
    while p < depth:
        iterations *= 2
        p += 1
    total_checksum = 0
    i = 0
    while i < iterations:
        total_checksum += tree_checksum(make_tree(depth))
        i += 1
    print(f"{iterations} trees of depth {depth}: checksum {total_checksum}")
    depth += 2

long_lived_checksum = tree_checksum(long_lived_tree)
print(f"long-lived tree of depth {MAX_DEPTH}: checksum {long_lived_checksum}")

dur = time.perf_counter() - start
print(f"binary_trees: {dur}s")
