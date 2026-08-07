import time

start = time.perf_counter()
total = 0
i = 0
while i < 3300000:
    rec = {"id": i, "name": f"item_{i}", "active": True, "score": i * 2}
    total += rec["id"] + rec["score"]
    i += 1
dur = time.perf_counter() - start
print(f"200k small (4-key) dicts, build+read: {dur}s, total={total}")
