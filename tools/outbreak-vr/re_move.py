"""Find the local player's position and facing: snapshot EE RAM while idle / walking / turning
and keep only values whose change pattern matches. Saves dumps under D:/Games/PS2/research/dumps.
"""
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
import padd
from pine import Pine

OUT = r"D:/Games/PS2/research/dumps"
os.makedirs(OUT, exist_ok=True)


def snap(p, tag):
    ram = p.dump_ram()
    with open(os.path.join(OUT, tag + ".bin"), "wb") as f:
        f.write(ram)
    return ram


def main():
    p = Pine()
    S = []
    S.append(snap(p, "s0_idle")); time.sleep(0.6)
    S.append(snap(p, "s1_idle"))
    padd.send({"op": "hold", "b": [], "ms": 1000, "ly": 1.0}); time.sleep(0.6)
    S.append(snap(p, "s2_walked"))
    padd.send({"op": "hold", "b": [], "ms": 450, "lx": -1.0}); time.sleep(0.6)
    S.append(snap(p, "s3_turned"))
    padd.send({"op": "hold", "b": [], "ms": 1000, "ly": 1.0}); time.sleep(0.6)
    S.append(snap(p, "s4_walked"))
    padd.send({"op": "hold", "b": [], "ms": 450, "lx": 1.0}); time.sleep(0.6)
    S.append(snap(p, "s5_turned_back"))

    f = [np.frombuffer(s, dtype="<f4") for s in S]
    with np.errstate(invalid="ignore", over="ignore"):
        fin = np.all([np.isfinite(x) for x in f], axis=0) & np.all([np.abs(x) < 1e6 for x in f], axis=0)
        same = lambda a, b: np.abs(f[a] - f[b]) < 1e-4
        moved = lambda a, b: np.abs(f[a] - f[b]) > 1.0
    pos = fin & same(0, 1) & moved(1, 2) & same(2, 3) & moved(3, 4)
    print("position candidates (f32):", int(pos.sum()))
    for i in np.nonzero(pos)[0][:60]:
        print("  0x%08X  %s" % (i * 4, "  ".join("%10.3f" % x[i] for x in f)))

    with np.errstate(invalid="ignore"):
        rot = fin & same(0, 1) & same(1, 2) & (np.abs(f[2] - f[3]) > 1e-3) & same(3, 4) & (np.abs(f[4] - f[5]) > 1e-3)
    print("facing candidates (f32):", int(rot.sum()))
    for i in np.nonzero(rot)[0][:60]:
        print("  0x%08X  %s" % (i * 4, "  ".join("%10.4f" % x[i] for x in f)))

    h = [np.frombuffer(s, dtype="<i2").astype(np.int32) for s in S]
    rot16 = (h[0] == h[1]) & (h[1] == h[2]) & (h[2] != h[3]) & (h[3] == h[4]) & (h[4] != h[5])
    print("facing candidates (s16):", int(rot16.sum()))
    for i in np.nonzero(rot16)[0][:80]:
        print("  0x%08X  %s" % (i * 2, "  ".join("%6d" % x[i] for x in h)))


if __name__ == "__main__":
    main()
