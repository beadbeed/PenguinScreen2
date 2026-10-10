"""While someone plays: every interval, screenshot the emulator window and snapshot a few memory ranges
(for finding UI flags afterwards). Runs until the emulator closes or for max_s seconds.

  python record_play.py <out_dir> [interval_s] [max_s]
"""
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
import drive
from pine import Pine

RANGES = [(0x230000, 0x260000), (0x2F0000, 0x310000), (0x3AE000, 0x3B4000), (0x46F000, 0x492000),
          (0x6D0000, 0x730000)]


def main():
    out = sys.argv[1]
    dt = float(sys.argv[2]) if len(sys.argv) > 2 else 0.5
    max_s = float(sys.argv[3]) if len(sys.argv) > 3 else 900
    os.makedirs(out, exist_ok=True)
    p = Pine()
    t0 = time.time()
    i = 0
    while time.time() - t0 < max_s:
        t = time.time() - t0
        try:
            snap = {lo: np.frombuffer(p.read_block(lo, hi - lo), dtype=np.uint8) for lo, hi in RANGES}
            np.savez_compressed(os.path.join(out, "mem-%04d.npz" % i), t=t, **{"r%06X" % lo: v for lo, v in snap.items()})
            if i % 4 == 0:
                drive.screenshot(os.path.join(out, "shot-%04d.png" % i), scale=2)
        except Exception as e:
            print("stopped:", e)
            break
        i += 1
        time.sleep(max(0.0, dt - (time.time() - t0 - t)))
    print("recorded", i, "samples")


if __name__ == "__main__":
    main()
