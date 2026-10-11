"""Headset-free shake-to-break-free tests for File #1 (SLPM-65428): shakes a scripted VR controller over PINE
(MsgVRFakeInput) and checks, from the game's memory, that a zombie's grab ends sooner and that nothing is pressed or
waggled without one. Prints PASS / FAIL per scenario; exits 1 if any failed.

Start the emulator first, with PCSX2_VR_TEST_INPUT=1 in its environment and PINE on (slot 28011), in VR on SteamVR's
null headset (python null_driver.py on, then launch_vr.ps1) or headset-free with no OpenXR runtime. Leave
PCSX2_VR_FAKE_HEADPOSE unset. Offline only: it loads savestates, so it refuses to run with the network adapter on.

  python grabtest.py all
  python grabtest.py shake idle-shake
  python grabtest.py --list

Savestates (File #1, tools/outbreak-vr/research/2026-10-11-file1-research.md): --grab-slot (default 5) is J's Bar where
a zombie grabs Kevin about 4.7 s after the load; --calm-slot (default 6) is J's Bar with nothing near.

  idle-shake  calm slot: shake the right controller for 3 s. Kevin must not move (the waggle would walk him sideways) and
              the held word (local record +0xF80) must not change (nothing pressed).
  baseline    grab slot, the scripted hands still: how long the grab lasts with no input (about 10 s by hand).
              Fails only if no grab comes or it never ends.
  shake       grab slot, shaking from the load (the emulator sees the shake before the grab, so the presses start with
              it): the grab must end within --max-grab seconds (default 6.5; about 3.3 s was the best by hand). Then,
              shaking on, for 1 s nothing more may be pressed and Kevin must not be walked sideways (skipped if he is
              grabbed again).

The emulog shows `(VR) CameraDriver: local character grabbed; shaking a controller presses to break free (...)` and
`(VR) CameraDriver: no longer grabbed (after N s).` for each grab.
"""
import argparse
import math
import os
import struct
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pine import FakeController, PineError, pose, require_offline, wait_for_pine

SERIAL = "SLPM-65428"

# File #1 addresses (README.md, bin/resources/vr-profiles/SLPM-65428.yaml).
RECORDS, RECORD_STRIDE, LOCAL_SLOT = 0x476DD0, 0x10E0, 0x48BF7D
POSITION, STANCE, INPUT_HELD = 0x38, 0x08, 0xF80  # f32 x/y/z; u8 stance (5 grabbed); u32 held buttons
GRABBED = 5
RIGHT = 1

# Where the right hand shakes, from the head (metres: x right, y up, z back): in front of the chest, clear of the hip
# holster and the backpack zone (and the grip is never squeezed, so no zone presses anyway).
SHAKE_AT = (0.25, -0.25, -0.40)


class Fail(Exception):
    pass


def check(ok, message):
    if not ok:
        raise Fail(message)


def u2f(u):
    return struct.unpack("<f", struct.pack("<I", u))[0]


def wait_until(fn, timeout, dt=0.02):
    """Polls fn until it is truthy; returns the seconds it took, or None on timeout."""
    t0 = time.time()
    while True:
        if fn():
            return time.time() - t0
        if time.time() - t0 > timeout:
            return None
        time.sleep(dt)


class Game:
    def __init__(self, p):
        self.p = p

    def record(self):
        return RECORDS + self.p.read8(LOCAL_SLOT) * RECORD_STRIDE

    def stance(self):
        return self.p.read8(self.record() + STANCE)

    def held(self):
        return self.p.read32(self.record() + INPUT_HELD)

    def position(self):
        base = self.record() + POSITION
        return tuple(u2f(self.p.read32(base + i * 4)) for i in range(3))

    def moved_since(self, start):
        x, _, z = self.position()
        return math.hypot(x - start[0], z - start[2])


def still_hand(fc):
    at = pose(pos=SHAKE_AT)
    fc.set_hand(RIGHT, aim=at, grip=at, head_relative=True)


class Shaker:
    """Moves the scripted right controller side to side, amp metres either way at hz (4 Hz, 7 cm: up to about 1.8 m/s),
    about 90 updates a second from a thread, until stopped; then holds it still where it started."""

    def __init__(self, fc, hz=4.0, amp=0.07):
        self.fc, self.hz, self.amp = fc, hz, amp
        self.error = None
        self._stop = threading.Event()
        self._thread = None

    def __enter__(self):
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        return self

    def __exit__(self, *exc):
        self._stop.set()
        self._thread.join()
        try:
            still_hand(self.fc)
        except (PineError, OSError):
            pass

    def _run(self):
        t0 = time.time()
        while not self._stop.is_set():
            dx = self.amp * math.sin(2.0 * math.pi * self.hz * (time.time() - t0))
            at = pose(pos=(SHAKE_AT[0] + dx, SHAKE_AT[1], SHAKE_AT[2]))
            try:
                self.fc.set_hand(RIGHT, aim=at, grip=at, head_relative=True)
            except (PineError, OSError) as e:
                self.error = e
                return
            time.sleep(1.0 / 90.0)


def load(p, slot, settle):
    p.load_state(slot)
    time.sleep(settle)


def sc_idle_shake(g, args):
    load(g.p, args.calm_slot, args.settle)
    with FakeController(g.p) as fc:
        still_hand(fc)
        time.sleep(1.0)
        check(g.stance() != GRABBED, "Kevin is grabbed in the calm slot %d" % args.calm_slot)
        start = g.position()
        values = {g.held()}
        with Shaker(fc) as shaker:
            t0 = time.time()
            while time.time() - t0 < 3.0:
                values.add(g.held())
                check(g.stance() != GRABBED, "a grab started during the idle shake: use a calm slot (--calm-slot)")
                time.sleep(0.02)
        check(shaker.error is None, "the scripted shake stopped: %s" % shaker.error)
        moved = g.moved_since(start)
    check(len(values) == 1, "shaking without a grab changed the held word (%s): something was pressed" %
          ", ".join("0x%X" % v for v in sorted(values)))
    check(moved < 15.0, "shaking without a grab moved Kevin %.1f units (the waggle walks him)" % moved)
    return "no grab: moved %.1f units, held word steady at 0x%X" % (moved, values.pop())


def sc_baseline(g, args):
    load(g.p, args.grab_slot, 0.3)
    with FakeController(g.p) as fc:
        still_hand(fc)
        t_grab = wait_until(lambda: g.stance() == GRABBED, args.grab_wait)
        check(t_grab is not None, "no grab within %.0f s of loading slot %d" % (args.grab_wait, args.grab_slot))
        t_free = wait_until(lambda: g.stance() != GRABBED, 25.0, 0.05)
    check(t_free is not None, "the grab lasted over 25 s with no input")
    return "grabbed %.1f s after the load, held %.1f s with no input" % (t_grab, t_free)


def sc_shake(g, args):
    load(g.p, args.grab_slot, 0.3)
    with FakeController(g.p) as fc:
        still_hand(fc)
        with Shaker(fc) as shaker:
            t_grab = wait_until(lambda: g.stance() == GRABBED, args.grab_wait)
            check(t_grab is not None, "no grab within %.0f s of loading slot %d" % (args.grab_wait, args.grab_slot))
            # How often the held word changes while grabbed: the presses as the game reads them (informational).
            changes, prev, t0 = 0, g.held(), time.time()
            while g.stance() == GRABBED and time.time() - t0 < 20.0:
                h = g.held()
                changes += 1 if h != prev else 0
                prev = h
                time.sleep(0.01)
            held_s = time.time() - t0
            check(g.stance() != GRABBED, "still grabbed after 20 s of shaking")
            # Free again, still shaking: nothing more is pressed or waggled (the game's own release takes a moment).
            time.sleep(0.3)
            start, values, regrabbed = g.position(), {g.held()}, False
            t1 = time.time()
            while time.time() - t1 < 1.0:
                if g.stance() == GRABBED:
                    regrabbed = True
                    break
                values.add(g.held())
                time.sleep(0.02)
            moved = g.moved_since(start)
        check(shaker.error is None, "the scripted shake stopped: %s" % shaker.error)
    detail = "grabbed %.1f s after the load, broke free after %.1f s (held word changed %d times)" % (t_grab, held_s, changes)
    check(held_s <= args.max_grab, detail + "; expected at most %.1f s" % args.max_grab)
    if regrabbed:
        return detail + "; grabbed again at once, after-check skipped"
    check(len(values) == 1, detail + "; after the grab the held word still changed (%s): presses went on" %
          ", ".join("0x%X" % v for v in sorted(values)))
    check(moved < 40.0, detail + "; after the grab Kevin moved %.1f units in 1 s (a waggle still going?)" % moved)
    return detail + "; nothing pressed after (moved %.1f units)" % moved


SCENARIOS = [
    ("idle-shake", sc_idle_shake),
    ("baseline", sc_baseline),
    ("shake", sc_shake),
]


def main():
    names = [n for n, _ in SCENARIOS]
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("scenarios", nargs="*", help="'all' or names: " + ", ".join(names))
    ap.add_argument("--list", action="store_true", help="list the scenarios")
    ap.add_argument("--pine-slot", type=int, default=28011)
    ap.add_argument("--grab-slot", type=int, default=5, help="savestate where a zombie grabs Kevin soon after the load")
    ap.add_argument("--calm-slot", type=int, default=6, help="savestate with nothing near")
    ap.add_argument("--grab-wait", type=float, default=10.0, help="seconds to wait for the grab after the load")
    ap.add_argument("--max-grab", type=float, default=6.5, help="seconds the grab may last while shaking")
    ap.add_argument("--settle", type=float, default=3.0, help="seconds to wait after loading the calm slot")
    args = ap.parse_args()

    if args.list or not args.scenarios:
        print("\n".join(names))
        return 0
    wanted = names if "all" in args.scenarios else args.scenarios
    unknown = [n for n in wanted if n not in names]
    if unknown:
        print("unknown scenario(s): %s (one of %s)" % (", ".join(unknown), ", ".join(names)))
        return 2

    try:
        p = wait_for_pine(args.pine_slot, seconds=10)
        require_offline(p)
        serial = p.game_id()
    except PineError as e:
        print("PINE: %s (emulator running, EnablePINE on?)" % e)
        return 2
    if serial != SERIAL:
        print("these scenarios are for %s; the emulator is running %r" % (SERIAL, serial))
        return 2
    try:
        p.fake_input(head=None, ttl_ms=0)
    except PineError:
        print("MsgVRFakeInput refused: start the emulator with PCSX2_VR_TEST_INPUT=1 (and online-safe off).")
        return 2

    g = Game(p)
    failed = 0
    for name in wanted:
        fn = dict(SCENARIOS)[name]
        try:
            result, detail = "PASS", fn(g, args)
        except Fail as e:
            result, detail = "FAIL", str(e)
        except (PineError, OSError) as e:
            result, detail = "ERROR", "%s: %s" % (type(e).__name__, e)
        failed += 1 if result != "PASS" else 0
        print("%-5s %-12s %s" % (result, name, detail), flush=True)
    print("%d passed, %d failed" % (len(wanted) - failed, failed))
    p.close()
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
