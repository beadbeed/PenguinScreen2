"""Headset-free checks of VrGrab's shot rule on slot 5 (a zombie grabs Kevin): a trigger press only counts as a shot or
headshot when the game really fired (a round gone from the equipped gun's count) within 250 ms.

Start the emulator offline with PCSX2_VR_TEST_INPUT=1, PINE on (--pine-slot), on a copy of the savestates; it loads
savestates. The emulog (--log) tells what VrGrab did; the script reads the game's memory for the rest.

  python grabshot_test.py --log run-gshot/logs/emulog.txt --pine-slot 28190 baseline empty-clip loaded-trigger simulated-shot

  baseline         no input: how long the grab lasts.
  empty-clip       the gun's count written to 0, then the trigger pressed with the aim ray through the head.
  loaded-trigger   the gun as it is (loaded), the trigger pressed with the ray through the head: what the game does
                   with a press while held (a round gone or not).
  simulated-shot   the trigger pressed with the ray through the head and the gun's count written down by one in the
                   same moment (stands in for the game's shot; the game itself fires nothing while held).
  off-head         as simulated-shot with the ray 0.5 m beside the head (no sphere on it): no event.
"""
import argparse
import os
import re
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pine import FakeController, PineError, pose, require_offline, wait_for_pine

RECORDS, RECORD_STRIDE, LOCAL_SLOT = 0x476DD0, 0x10E0, 0x48BF7D
STANCE, EQUIPPED, METER = 0x08, 0xC7C, 0xCDD
ITEM_TABLE, ITEM_STRIDE, ITEM_ID, ITEM_COUNT = 0x3967D0, 60, 0x06, 0x24
GRABBED, RIGHT = 5, 1
HEAD_RE = re.compile(r"the grabbing zombie's head at VR \(([-\d.]+), ([-\d.]+), ([-\d.]+)\) m")


class Game:
    def __init__(self, p, log):
        self.p, self.log = p, log

    def record(self):
        # Kevin is local slot 0 in slot 5 (the raw slot byte flickers to a partner's while the game runs the partners).
        return RECORDS

    def stance(self):
        return self.p.read8(self.record() + STANCE)

    def meter(self):
        return self.p.read8(self.record() + METER) & 0x7F

    def item(self):
        eq = self.p.read8(self.record() + EQUIPPED)
        rec = ITEM_TABLE + eq * ITEM_STRIDE
        return eq, self.p.read16(rec + ITEM_ID), self.p.read16(rec + ITEM_COUNT)

    def set_ammo(self, n):
        eq = self.p.read8(self.record() + EQUIPPED)
        a = ITEM_TABLE + eq * ITEM_STRIDE + ITEM_COUNT
        self.p.write32(a, (self.p.read32(a) & 0xFFFF0000) | (n & 0xFFFF))

    def log_size(self):
        return os.path.getsize(self.log) if os.path.exists(self.log) else 0

    def log_since(self, pos):
        with open(self.log, "rb") as f:
            f.seek(pos)
            return f.read().decode("utf-8", "replace")


def wait_until(fn, timeout, dt=0.01):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if fn():
            return time.time() - t0
        time.sleep(dt)
    return None


def run(g, scenario, a):
    mark = g.log_size()
    g.p.load_state(a.grab_slot)
    # load_state returns before the state is in: wait for the emulator to say so, or the old grab reads as this one.
    if wait_until(lambda: "Loaded state from slot" in g.log_since(mark), 10.0, 0.05) is None:
        return "the state did not load"
    time.sleep(0.3)
    mark = g.log_size()
    out = {}
    with FakeController(g.p) as fc:
        at = pose(pos=(0.25, -0.25, -0.40))
        fc.set_hand(RIGHT, aim=at, grip=at, head_relative=True)
        t_grab = wait_until(lambda: "VrGrab: grabbed (" in g.log_since(mark), 15.0, 0.02)
        if t_grab is None:
            return "no grab"
        out["item"] = g.item()
        if scenario != "baseline":
            head = None
            for _ in range(100):
                m = HEAD_RE.search(g.log_since(mark))
                if m:
                    head = tuple(float(x) for x in m.groups())
                    break
                time.sleep(0.02)
            if head is None:
                return "grabbed but the head was never located (log)"
            off = 0.5 if scenario == "off-head" else 0.0
            at = pose(pos=(head[0] + off, head[1], head[2] + 0.5))
            fc.set_hand(RIGHT, aim=at, grip=at, head_relative=False)
            time.sleep(1.2)  # the hand settles; no trigger yet (the meter counts from phase 4, about 1 s into the grab)
            if scenario == "empty-clip":
                g.set_ammo(0)
            out["ammo_before"] = g.item()[2]
            fc.set_hand(RIGHT, aim=at, grip=at, head_relative=False, trigger=1.0)
            if scenario in ("simulated-shot", "off-head"):
                time.sleep(0.08)
                g.set_ammo(max(g.item()[2] - 1, 0))
            time.sleep(0.5)
            out["ammo_after"] = g.item()[2]
            fc.set_hand(RIGHT, aim=at, grip=at, head_relative=False, trigger=0.0)
        t_free = wait_until(lambda: "VrGrab: free after" in g.log_since(mark), 20.0, 0.05)
        m = re.search(r"free after ([\d.]+) s \(([^;]*);", g.log_since(mark))
        out["held_s"] = m.group(1) + " (" + m.group(2) + ")" if m else None
    time.sleep(0.3)
    log = g.log_since(mark)
    out["vrgrab"] = [l.split("VrGrab: ", 1)[1].strip() for l in log.splitlines()
                     if "VrGrab:" in l and any(k in l for k in ("headshot", "shot", "fired", "meter", "free after", "shove"))]
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("scenarios", nargs="+")
    ap.add_argument("--log", required=True)
    ap.add_argument("--pine-slot", type=int, default=28190)
    ap.add_argument("--grab-slot", type=int, default=5)
    a = ap.parse_args()
    p = wait_for_pine(a.pine_slot, seconds=10)
    require_offline(p)
    p.fake_input(head=None, ttl_ms=0)
    g = Game(p, a.log)
    for s in a.scenarios:
        print("==", s, flush=True)
        r = run(g, s, a)
        print(r if isinstance(r, str) else "\n".join("  %s: %s" % kv for kv in r.items()), flush=True)
    p.close()


if __name__ == "__main__":
    sys.exit(main())
