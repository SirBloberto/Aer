import time

SIZE = 720
MAX_ITER = 200

def mandelbrot_point(cx, cy, max_iter):
    x = 0.0
    y = 0.0
    it = 0
    while it < max_iter:
        x2 = x * x
        y2 = y * y
        if x2 + y2 > 4.0:
            return it
        y_new = 2.0 * x * y + cy
        x_new = x2 - y2 + cx
        y = y_new
        x = x_new
        it += 1
    return max_iter

start = time.perf_counter()
inside_count = 0
py = 0
while py < SIZE:
    px = 0
    while px < SIZE:
        cx = float(px) / float(SIZE) * 3.5 - 2.5
        cy = float(py) / float(SIZE) * 2.0 - 1.0
        if mandelbrot_point(cx, cy, MAX_ITER) == MAX_ITER:
            inside_count += 1
        px += 1
    py += 1
dur = time.perf_counter() - start
print(f"mandelbrot {SIZE}x{SIZE}: {dur}s, inside={inside_count}")
