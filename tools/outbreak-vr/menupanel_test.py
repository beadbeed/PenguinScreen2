"""[VR] MenuPanel, headset-free: does first person keep the camera while a game screen is up, does the room keep
rendering around it, and is the screen split off the frame? File #1 (SLPM-65428), offline only, normal speed, no OpenXR
runtime; the head is scripted over PINE (PCSX2_VR_TEST_INPUT), the screens are opened with the game's own buttons and
closed with Cross (B) or the same button. Nothing is ever confirmed: the take-it prompt is declined with Cross.

    python -I menupanel_test.py <portable dev install> <out dir> [items] [map] [takeit] [pause] [--slot N] [--trace N] [--xr]

The install's ini needs [VR] MenuPanel = true (or false, for the before picture) and its sstates/ the slots used: items
map and pause load slot 6 (J's Bar, calm; pause is opened with Select, sent as Backspace to the emulator window, which
takes the focus for it), takeit loads slot 8 (the men's bathroom, Green Herb on the sink) unless --slot is
given. It runs that install, never D:\\Games\\PS2\\PenguinScreen2, starts and stops only its own emulator process,
and writes into <out dir>: the panel, room and finished frame PNGs (PCSX2_VR_MENUPANEL_DUMP), the first-person stereo
dump of the frame shown (PCSX2_VR_STEREODUMP), and emulog.txt. Printed per screen, sampled every 50 ms while it is up:
  - lookAt: the share of samples with first person driving the camera (telemetry look_at_active);
  - camera: the game camera's yaw from its eye to its target (0x306338 / 0x306344) against the scripted head's, as the
    head turns 40 deg while the screen is up (the camera follows the head: first person kept);
  - game: the game frame counter u8 0x48BF93 rate (212.4/s at 100%: the game keeps running);
  - the emulog's "(VR) Menu panel" and MTRACE summary lines.
On the item screen the menu pointer then steers the game's cursor to cells 1, 3 and 0 (D-pad presses only, nothing
confirmed): with no OpenXR runtime through PINE test hits at each cell's rect centre (MsgVRMenuPointer op 1); with --xr
(SteamVR, the null headset: python null_driver.py on first, off after) with the right hand's ray aimed at the published
screen transform, which must then be the menu panel (its centre, size and the hit's u, v are printed).
"""
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import time

DEV = os.path.abspath(sys.argv[1])
OUT = os.path.abspath(sys.argv[2])
os.environ["PCSX2_INI"] = os.path.join(DEV, "inis", "PenguinScreen2.ini")  # pine.py's offline check reads this ini
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pine import FakeController, PineError, pose, require_offline, wait_for_pine, zone  # noqa: E402
from menutest import CELLS, NODE, INDEX, CUR_X, CUR_Y, TestHit, add, normalize, rect_centre, rotate, scale, sub  # noqa: E402

EXE = os.path.join(DEV, "pcsx2-qt.exe")
LOG = os.path.join(DEV, "logs", "emulog.txt")
ISO = r"D:\Games\PS2\games\Biohazard Outbreak File 1 (SLPM-65428) EN-patched.iso"
assert os.path.normcase(DEV) != os.path.normcase(r"D:\Games\PS2\PenguinScreen2"), "never the real install"
assert os.path.exists(os.path.join(DEV, "portable.txt")), "a portable dev install"

OVERLAY, WHICH, SUBMODE = 0x720AA0, 0x720BE4, 0x720BE2
EYE, TARGET = 0x306338, 0x306344
VIEW = 0x3062E0
COUNTER = 0x48BF93
RECORDS, STRIDE, LOCAL_SLOT = 0x476DD0, 0x10E0, 0x48BF7D
BELT = zone(-0.22, -0.50, 0.12)
LEFT, RIGHT = 0, 1

args = [a for a in sys.argv[3:] if not a.startswith("--")]
opts = sys.argv[3:]


def opt(name, default):
    if name in opts:
        return int(opts[opts.index(name) + 1])
    return default


SCENARIOS = [a for a in args if a in ("items", "map", "takeit", "pause")] or ["items", "map", "takeit"]
SLOT = opt("--slot", 0)
XR = "--xr" in opts
STEAMXR = r"E:\ProgramFile\Steam\steamapps\common\SteamVR\steamxr_win64.json"
TRACE = opt("--trace", 3)
DUMP_EVERY = opt("--dump", 25)


def f32(p, a):
    return struct.unpack("<f", struct.pack("<I", p.read32(a)))[0]


def camera_yaw(p):
    e = [f32(p, EYE + 4 * i) for i in range(3)]
    t = [f32(p, TARGET + 4 * i) for i in range(3)]
    return math.degrees(math.atan2(t[0] - e[0], t[2] - e[2]))


def view_yaw(p):
    """The yaw of the view matrix the game built (0x3062E0, row vectors: the camera looks down -column 2)."""
    m = [f32(p, VIEW + 4 * i) for i in (2, 6, 10)]
    return math.degrees(math.atan2(-m[0], -m[2]))


def wrap(d):
    return (d + 180.0) % 360.0 - 180.0


def log_text():
    try:
        return open(LOG, encoding="utf-8", errors="replace").read()
    except OSError:
        return ""


def wait_until(fn, timeout, dt=0.02):
    t0 = time.time()
    while time.time() - t0 < timeout:
        v = fn()
        if v:
            return v
        time.sleep(dt)
    return None


def local_base(p):
    return RECORDS + p.read8(LOCAL_SLOT) * STRIDE


class Run:
    def __init__(self, p):
        self.p = p

    def armed(self, timeout=8.0):
        t = {}

        def ok():
            t.update(self.p.telemetry())
            return t["armed"] and t["look_at_active"]

        if not wait_until(ok, timeout, 0.05):
            raise RuntimeError("first person not active (%s)" % t.get("disarm_reason"))
        return t

    def squeeze_belt(self, fc):
        fc.set_hand(LEFT, grip=pose(pos=BELT), head_relative=True, squeeze=0.0)
        time.sleep(0.25)
        fc.set_hand(LEFT, grip=pose(pos=BELT), head_relative=True, squeeze=1.0)
        time.sleep(0.35)
        fc.set_hand(LEFT, grip=pose(pos=BELT), head_relative=True, squeeze=0.0)
        time.sleep(0.1)
        fc.clear_hand(LEFT)

    def press(self, fc, button, hold=0.15):
        fc.set_hand(RIGHT, buttons=(button,))
        time.sleep(hold)
        fc.set_hand(RIGHT)
        time.sleep(0.1)

    def sample(self, fc, seconds, head_from, head_to):
        """While a screen is up: turn the scripted head from head_from to head_to degrees of yaw over the first half,
        hold it the second half; sample every 50 ms."""
        p = self.p
        rows = []
        t0 = time.time()
        c0 = p.read8(COUNTER)
        counted = 0
        last = c0
        while time.time() - t0 < seconds:
            k = min(1.0, (time.time() - t0) / (seconds * 0.5))
            yaw = head_from + (head_to - head_from) * k
            fc.set_head(pose(yaw_deg=yaw))
            time.sleep(0.05)
            tel = p.telemetry()
            c = p.read8(COUNTER)
            counted += (c - last) & 0xFF
            last = c
            rows.append(dict(t=time.time() - t0, head=yaw, cam=camera_yaw(p), view=view_yaw(p), up=p.read32(OVERLAY),
                             which=p.read8(WHICH), look=tel["look_at_active"], pause=tel["pause_when_hit"],
                             anchor=tel["yaw_anchor"], matched=tel["frames_matched"]))
        rate = counted / max(1e-3, time.time() - t0)
        return rows, rate

    def report(self, name, rows, rate, mark):
        up = [r for r in rows if r["up"] == 1]
        print("== %s: %d samples, %d with the screen up (u8 0x720BE4 = %s)" % (
            name, len(rows), len(up), sorted(set(r["which"] for r in up))))
        if not up:
            return
        look = sum(1 for r in up if r["look"]) / len(up)
        print("   lookAt active in %.0f%% of them; pauseWhen hit in %.0f%%" % (
            100 * look, 100 * sum(1 for r in up if r["pause"]) / len(up)))
        # The camera's yaw against the head's: first person keeps the camera when it moves with the head.
        cam0, head0 = up[0]["cam"], up[0]["head"]
        errs = [abs(wrap((r["cam"] - cam0) - (r["head"] - head0))) for r in up]
        span_cam = wrap(up[-1]["cam"] - cam0)
        span_head = up[-1]["head"] - head0
        print("   camera turned %.1f deg as the head turned %.1f deg (largest gap %.1f deg, median %.1f)" % (
            span_cam, span_head, max(errs), sorted(errs)[len(errs) // 2]))
        # The view matrix the game built from it: the room is rendered from the head when this turns too.
        span_view = wrap(up[-1]["view"] - up[0]["view"])
        last = up[-1]
        settled = [r for r in up if r["t"] > last["t"] - 1.0]
        verr = max(abs(wrap((r["view"] - up[0]["view"]) - (r["head"] - head0))) for r in settled) if settled else 0.0
        print("   the game's view matrix turned %.1f deg (within %.1f deg of the head over the last 1 s); render poses "
              "matched to a write: %d" % (span_view, verr, last["matched"] - up[0]["matched"]))
        print("   game counter %.1f/s (212.4/s = 100%%)" % rate)
        lines = [l for l in log_text()[mark:].splitlines() if "(VR) Menu panel" in l or "MTRACE vsync" in l or
                 "CameraDriver: a game screen" in l]
        for l in lines[:40]:
            print("   |", l[:220])

    def cell(self):
        c = (self.p.read32(NODE), self.p.read16(INDEX), self.p.read16(CUR_X), self.p.read16(CUR_Y))
        for i, cell in enumerate(CELLS):
            if c == (cell[1], cell[2], cell[3], cell[4]):
                return i
        return -1

    def point(self, fc, hit, u, v):
        """The right hand at (u, v) of the game picture: a PINE test hit, or (--xr) a ray at the published transform."""
        if hit is not None:
            hit.set(RIGHT, u, v)
            return None
        m = self.p.menu_pointer()
        if not m["transform_valid"]:
            return None
        q = m["quat"]
        target = add(m["centre"], rotate(q, ((u - 0.5) * 2.0 * m["half_width"], (0.5 - v) * 2.0 * m["half_height"], 0.0)))
        origin = add(m["centre"], scale(rotate(q, (0.0, 0.0, 1.0)), 0.6))
        d = normalize(sub(target, origin))
        yaw = math.degrees(math.atan2(-d[0], -d[2]))
        pitch = math.degrees(math.asin(max(-1.0, min(1.0, d[1]))))
        a = pose(yaw, pitch, origin)
        fc.set_hand(RIGHT, aim=a, grip=a)
        return m

    def steer(self, fc):
        hit = None if XR else TestHit(self.p)
        try:
            time.sleep(0.5)
            m = self.p.menu_pointer()
            if XR:
                print("   pointer screen: valid %s, centre (%.2f, %.2f, %.2f), %.2f x %.2f m, tablet %s" % (
                    m["transform_valid"], m["centre"][0], m["centre"][1], m["centre"][2], 2 * m["half_width"],
                    2 * m["half_height"], m["tablet"]))
            for target in (1, 3, 0):
                u, v = rect_centre(target)
                m0 = self.p.menu_pointer()
                t0 = time.time()
                got = None
                while time.time() - t0 < 1.5:
                    self.point(fc, hit, u, v)
                    if self.cell() == target:
                        got = time.time() - t0
                        break
                    time.sleep(0.03)
                time.sleep(0.15)
                m1 = self.p.menu_pointer()
                extra = ""
                if XR and m1["hit_valid"]:
                    extra = ", ray hit (%.3f, %.3f) for (%.3f, %.3f)" % (m1["u"], m1["v"], u, v)
                print("   steer to %s: %s, %d D-pad presses, confirms passed %d%s" % (
                    CELLS[target][0], ("there in %.2f s" % got) if got is not None else "NOT reached (cell %d)" % self.cell(),
                    m1["presses"] - m0["presses"], m1["confirms_passed"] - m0["confirms_passed"], extra))
        finally:
            if hit is not None:
                hit.stop()
            fc.clear_hand(RIGHT)

    def baseline(self, fc):
        """The game counter's rate with no screen up, the same way it is sampled under one."""
        rows, rate = self.sample(fc, 2.0, 0.0, 0.0)
        print("   no screen: game counter %.1f/s" % rate)

    def items(self, fc):
        self.baseline(fc)
        mark = len(log_text())
        self.squeeze_belt(fc)
        if not wait_until(lambda: self.p.read32(OVERLAY) == 1, 4.0):
            raise RuntimeError("the item screen did not open")
        time.sleep(0.6)
        rows, rate = self.sample(fc, 4.0, 0.0, 40.0)
        self.report("item screen", rows, rate, mark)
        fc.set_head(pose(yaw_deg=40.0))
        self.steer(fc)
        self.squeeze_belt(fc)
        if not wait_until(lambda: self.p.read32(OVERLAY) == 0, 4.0):
            self.press(fc, "b")
            wait_until(lambda: self.p.read32(OVERLAY) == 0, 3.0)
        fc.set_head(pose(yaw_deg=0.0))
        time.sleep(1.0)

    def map(self, fc):
        mark = len(log_text())
        self.press(fc, "y")
        if not wait_until(lambda: self.p.read32(OVERLAY) == 1, 4.0):
            raise RuntimeError("the map did not open")
        time.sleep(0.6)
        rows, rate = self.sample(fc, 4.0, 0.0, 40.0)
        self.report("map", rows, rate, mark)
        self.press(fc, "b")
        if not wait_until(lambda: self.p.read32(OVERLAY) == 0, 4.0):
            self.press(fc, "y")
            wait_until(lambda: self.p.read32(OVERLAY) == 0, 3.0)
        fc.set_head(pose(yaw_deg=0.0))
        time.sleep(1.0)

    def pause(self, fc):
        """Pause (Select, sent as Backspace to the emulator window: Pad 1 needs its keyboard binding). Closed with Select."""
        import drive
        mark = len(log_text())
        drive.press("select")
        if not wait_until(lambda: self.p.read32(OVERLAY) == 1, 4.0):
            raise RuntimeError("pause did not open (Select / Backspace)")
        time.sleep(0.6)
        rows, rate = self.sample(fc, 4.0, 0.0, 40.0)
        self.report("pause", rows, rate, mark)
        drive.press("select")
        if not wait_until(lambda: self.p.read32(OVERLAY) == 0, 4.0):
            self.press(fc, "b")
            wait_until(lambda: self.p.read32(OVERLAY) == 0, 3.0)
        fc.set_head(pose(yaw_deg=0.0))
        time.sleep(1.0)

    def takeit(self, fc, anchor):
        p = self.p
        mark = len(log_text())
        # Stand at the sink facing the Green Herb (research/2026-10-12-ui-flags-research.md: (9602, 12990) facing 0):
        # the position written (offline), the facing from the head, which the standing body follows.
        base = local_base(p)
        p.write32(base + 0x38, struct.unpack("<I", struct.pack("<f", 9602.0))[0])
        p.write32(base + 0x40, struct.unpack("<I", struct.pack("<f", 12990.0))[0])
        fc.set_head(pose(yaw_deg=-anchor))
        time.sleep(1.5)
        heading = p.read16(base + 0x92) * 360.0 / 65536.0
        print("   (at %.0f, %.0f, heading %.1f deg)" % (f32(p, base + 0x38), f32(p, base + 0x40), wrap(heading)))
        self.press(fc, "a")
        if not wait_until(lambda: p.read32(OVERLAY) == 1, 5.0):
            raise RuntimeError("no take-it prompt (heading %.1f)" % wrap(heading))
        time.sleep(0.6)
        print("   take-it prompt up: sub-mode u8 0x720BE2 = %d" % p.read8(SUBMODE))
        rows, rate = self.sample(fc, 4.0, -anchor, -anchor + 40.0)
        self.report("take-it prompt", rows, rate, mark)
        # Declined: Cross, never Circle.
        self.press(fc, "b")
        wait_until(lambda: p.read32(OVERLAY) == 0, 4.0)
        fc.set_head(pose(yaw_deg=-anchor))
        time.sleep(1.0)


def main():
    require_offline()
    os.makedirs(OUT, exist_ok=True)
    env = dict(os.environ)
    # Missing on purpose (no OpenXR runtime), or with --xr SteamVR (the null headset, set up by null_driver.py on).
    env["XR_RUNTIME_JSON"] = STEAMXR if XR else os.path.join(DEV, "no-openxr-runtime.json")
    env["PCSX2_VR_TEST_INPUT"] = "1"
    env.pop("PCSX2_VR_FAKE_HEADPOSE", None)  # the scripted PINE head must be the head
    env["PCSX2_VR_MENUTRACE"] = str(TRACE)
    env["PCSX2_VR_MENUPANEL_DUMP"] = os.path.join(OUT, "panel") + ",%d" % DUMP_EVERY
    env["PCSX2_VR_STEREODUMP"] = os.path.join(OUT, "stereo") + ",40"
    if XR:
        env.pop("PCSX2_VR_STEREODUMP", None)
    proc = subprocess.Popen([EXE, "--vr", "--", ISO], cwd=DEV, env=env)
    try:
        p = wait_for_pine(seconds=120)
        for _ in range(120):
            try:
                if p.game_id() and p.status() == "running":
                    break
            except PineError:
                pass
            time.sleep(1)
        require_offline(p)
        time.sleep(8)
        r = Run(p)
        for name in SCENARIOS:
            slot = SLOT or (8 if name == "takeit" else 6)
            p.load_state(slot)
            time.sleep(3.5)
            with FakeController(p) as fc:
                tel = r.armed()
                print("-- %s (slot %d): armed, yaw anchor %.1f deg" % (name, slot, tel["yaw_anchor"]))
                try:
                    if name == "takeit":
                        r.takeit(fc, tel["yaw_anchor"])
                    else:
                        getattr(r, name)(fc)
                except RuntimeError as e:
                    print("   FAILED: %s" % e)
    finally:
        time.sleep(6)  # the dumps' PNG writes finish on their own threads
        try:
            proc.terminate()
            proc.wait(15)
        except Exception:
            subprocess.run(["taskkill", "/F", "/PID", str(proc.pid)], capture_output=True)
        time.sleep(2)
        try:
            shutil.copy(LOG, os.path.join(OUT, "emulog.txt"))
        except OSError:
            pass


main()
