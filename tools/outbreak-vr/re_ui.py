"""Find a UI-state flag (item screen, map, ...): toggle it with a pad button A/B/A/B/A, snapshot a few
memory ranges each time, keep bytes that are the same in every A and in every B and differ between.

  python re_ui.py <open-button> [close-button] [label]
"""
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
import drive
import padd
from pine import Pine

RANGES = [(0x230000, 0x260000), (0x2F0000, 0x310000), (0x3AE000, 0x3B4000), (0x46F000, 0x492000),
          (0x6D0000, 0x730000)]


def snap(p):
    return {lo: np.frombuffer(p.read_block(lo, hi - lo), dtype=np.uint8) for lo, hi in RANGES}


def main():
    open_b = sys.argv[1]
    close_b = sys.argv[2] if len(sys.argv) > 2 else open_b
    label = sys.argv[3] if len(sys.argv) > 3 else open_b
    p = Pine()
    states = []
    for i in range(5):
        states.append(snap(p))
        if i == 1:
            drive.screenshot(f"D:/Games/PS2/screens/ui-{label}-open.png", scale=2)
        padd.send({"op": "press", "b": open_b if i % 2 == 0 else close_b, "ms": 120})
        time.sleep(1.8)
    A, B = states[0::2], states[1::2]
    for lo, hi in RANGES:
        a = np.stack([s[lo] for s in A])
        b = np.stack([s[lo] for s in B])
        same_a = np.all(a == a[0], axis=0)
        same_b = np.all(b == b[0], axis=0)
        cand = np.nonzero(same_a & same_b & (a[0] != b[0]))[0]
        for off in cand[:40]:
            print("  0x%06X: closed=0x%02X open=0x%02X" % (lo + off, a[0][off], b[0][off]))
        if len(cand) > 40:
            print("  ... %d more in 0x%06X-0x%06X" % (len(cand) - 40, lo, hi))
        print("range 0x%06X-0x%06X: %d candidate byte(s)" % (lo, hi, len(cand)))


if __name__ == "__main__":
    main()
