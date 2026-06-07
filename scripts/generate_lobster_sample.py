#!/usr/bin/env python3
"""Generate a LOBSTER-format NASDAQ message sample (synthetic, NASDAQ-like microstructure)."""
import random
import os

OUT = os.path.join(os.path.dirname(__file__), "..", "data", "lobster", "AMZN_sample_message.csv")
random.seed(20120621)

MID = 220000   # $220.00 in LOBSTER price units (USD × 1000)
SPREAD = 10    # $0.01
t = 34200.0    # 09:30:00
oid = 206_800_000
orders = {}
lines = []

def emit(typ, order_id, size, price, direction):
    global t
    t += random.uniform(0.001, 0.05)
    lines.append(f"{t:.9f},{typ},{order_id},{size},{price},{direction}")

for _ in range(4000):
    r = random.random()
    if r < 0.45:
        side = -2 if random.random() < 0.52 else -1
        if side == -2:
            px = MID - SPREAD - random.randint(0, 30)   # bid below mid
        else:
            px = MID + SPREAD + random.randint(0, 30)   # ask above mid
        qty = random.choice([100, 200, 300, 500])
        emit(1, oid, qty, px, side)
        orders[oid] = (side, px, qty)
        oid += 1
    elif r < 0.65 and orders:
        k = random.choice(list(orders.keys()))
        side, px, qty = orders[k]
        if qty > 100 and random.random() < 0.5:
            dec = min(qty - 50, random.choice([50, 100]))
            emit(2, k, dec, px, side)
            orders[k] = (side, px, qty - dec)
        else:
            emit(3, k, qty, px, side)
            del orders[k]
    elif r < 0.85 and orders:
        k = random.choice(list(orders.keys()))
        side, px, qty = orders[k]
        fill = qty if qty <= 300 else random.choice([qty, qty // 2])
        emit(4, k, fill, px, -side)
        if fill >= qty:
            del orders[k]
        else:
            orders[k] = (side, px, qty - fill)
    else:
        MID += random.randint(-1, 1)

os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, "w", newline="\n") as f:
    f.write("\n".join(lines))
print(f"Wrote {len(lines)} events to {OUT}")
