import time

STATUS_CODES = [200, 200, 200, 200, 301, 404, 500]
PATHS = ["/", "/index.html", "/api/users", "/api/orders", "/static/app.js", "/favicon.ico"]


def make_log_lines(n, status_codes, paths):
    lines = []
    i = 0
    while i < n:
        status = status_codes[i % len(status_codes)]
        path = paths[i % len(paths)]
        line = f"{i} GET {path} {status} {i * 37 % 9973}"
        lines.append(line)
        i += 1
    return lines


def process_lines(lines):
    status_counts = {}
    path_counts = {}
    total_bytes = 0
    for line in lines:
        parts = line.split(" ")
        path = parts[2]
        status = parts[3]
        nbytes = int(parts[4])

        if status in status_counts:
            status_counts[status] += 1
        else:
            status_counts[status] = 1

        if path in path_counts:
            path_counts[path] += 1
        else:
            path_counts[path] = 1

        total_bytes += nbytes
    return status_counts, path_counts, total_bytes


N = 520000

start = time.perf_counter()
lines = make_log_lines(N, STATUS_CODES, PATHS)
status_counts, path_counts, total_bytes = process_lines(lines)
dur = time.perf_counter() - start

print(f"log_processing: {N} lines in {dur}s, total_bytes={total_bytes}")
print(f"  status 200: {status_counts['200']}, 404: {status_counts['404']}, 500: {status_counts['500']}")
print(f"  path /: {path_counts['/']}, /api/users: {path_counts['/api/users']}")
