"""Write a small synthetic NQ1M0001 file (a random walk over a few weekdays) for smoke tests: replay_trader --data.

Usage: python3 tools/make_test_data.py out.bin
"""
import random
import struct
import sys
from datetime import datetime, timedelta

random.seed(7)
t0 = datetime(2025, 3, 3, 18, 0)          # Monday-evening session start, New York
ts, o, h, l, c, v, adj = [], [], [], [], [], [], []
price = 20000.0
t = t0
while t < t0 + timedelta(days=5):
    if not (t.hour == 17):                # daily maintenance break 17:00-18:00
        op = price
        price = max(100.0, price + random.gauss(0, 4))
        hi = max(op, price) + abs(random.gauss(0, 2))
        lo = min(op, price) - abs(random.gauss(0, 2))
        r = lambda x: round(x * 4) / 4
        ts.append(int((t - datetime(1970, 1, 1)).total_seconds() // 60))
        o.append(r(op)); h.append(r(hi)); l.append(r(lo)); c.append(r(price))
        v.append(float(random.randint(50, 900))); adj.append(0.0)
    t += timedelta(minutes=1)
with open(sys.argv[1], "wb") as f:
    f.write(b"NQ1M0001")
    f.write(struct.pack("<q", len(ts)))
    f.write(struct.pack(f"<{len(ts)}i", *ts))
    for col in (o, h, l, c, v, adj):
        f.write(struct.pack(f"<{len(col)}f", *col))
print(f"wrote {sys.argv[1]}: {len(ts)} bars")
