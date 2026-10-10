"""Headset-free first-person VR tests for File #1 (SLPM-65428): drives the real VR input path with
scripted controllers over PINE (MsgVRFakeInput) and checks the game's memory and the camera's
telemetry (MsgVRTelemetry). Prints PASS / FAIL / SKIP per scenario; exits 1 if any failed.

Start the emulator first, with PCSX2_VR_TEST_INPUT=1 in its environment and PINE on (slot 28011):
in VR on SteamVR's null headset (python null_driver.py on, then launch_vr.ps1), or headset-free with
no OpenXR runtime. Leave PCSX2_VR_FAKE_HEADPOSE unset (launch_fakevr.ps1 sets it): the env fake head
outranks the scripted one. Be in a room (J's Bar) standing idle with room to walk ahead, or pass
--slot with a savestate like that (loaded before every scenario; recommended, walk-and-shoot moves you).

  python vrtest.py all                         # every scenario
  python vrtest.py arm holster point-to-aim    # some
  python vrtest.py --list
  python vrtest.py all --slot 3 --junit out/junit.xml --report out/   # report: HTML + Headset Window shots

walk-and-shoot-online-safe and pitch-online-safe only run with VR "online-safe" (or the network adapter)
on, where the scripted input must be refused, nothing may move the character and the aim servo is never
silenced (both raise the weapon with R1 on padd's virtual pad); every other scenario is skipped then.
savestate-yaw saves to --scratch-slot (default 9) and overwrites it.

To prove the runner can fail: put a copy of the profile in <data dir>/vrprofiles/ with
aim: { hand: left }; point-to-aim must then FAIL.
"""
import argparse
import html
import math
import os
import struct
import sys
import threading
import time
import traceback
from xml.etree import ElementTree as ET

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pine import FakeController, PineError, pose, wait_for_pine, zone

SERIAL = "SLPM-65428"

# File #1 addresses (README.md, bin/resources/vr-profiles/SLPM-65428.yaml).
RECORDS, RECORD_STRIDE, LOCAL_SLOT = 0x476DD0, 0x10E0, 0x48BF7D
POSITION, HEADING, STANCE = 0x38, 0x92, 0x08  # record offsets: f32 x/y/z, s16 heading, u8 stance (1 normal, 2 aimed)
EYE, TARGET = 0x306338, 0x306344
MENU_OPEN = 0x48BF7E
CUT_HANDLER, FOCUS_SLOT, NEAR_CULL = 0x58EAEC, 0x3AEF74, 0x6D6CF4
# Aim pitch: s16 record offset (0x10000 = 360 deg, positive up) and the gun-elevation servo words the profile
# NOPs while the weapon is raised offline: (address, the game's own word).
AIM_PITCH = 0xBC8
PITCH_SERVO = ((0x556968, 0x0C0D1AEC), (0x556980, 0x0C0D1AEC), (0x556998, 0x0C0D1AEC), (0x5569CC, 0xA6200BC8),
               (0x5569D8, 0xA6230BC8), (0x5569E4, 0xA6230BC8), (0x5569E8, 0xA6200BC8))
PITCH_SERVO_STORES = (0x5569CC, 0x5569D8, 0x5569E4, 0x5569E8)

# Profile values the scenarios expect.
HOLSTER = zone(0.22, -0.50, 0.12)
BELT = zone(-0.22, -0.50, 0.12)
SMOOTH_TURN_DEG_S = 120.0
AIM_MOVE_UNITS_S = 70.0
PITCH_SIGN, PITCH_CLAMP = 1.0, 8000
LEFT, RIGHT = 0, 1


class Fail(Exception):
    pass


class Skip(Exception):
    pass


def check(ok, message):
    if not ok:
        raise Fail(message)


def u2f(u):
    return struct.unpack("<f", struct.pack("<I", u))[0]


def wrap(deg):
    return (deg + 180.0) % 360.0 - 180.0


def s16(raw):
    return struct.unpack("<h", struct.pack("<H", raw & 0xFFFF))[0]


def pitch_units(deg):
    """The s16 the camera writes for an aim pitch of deg (profile pitchSign and pitchClamp)."""
    return int(round(max(-PITCH_CLAMP, min(PITCH_CLAMP, PITCH_SIGN * deg * 65536.0 / 360.0))))


def wait_until(fn, timeout, dt=0.02):
    """Polls fn until it is truthy; returns the seconds it took, or None on timeout."""
    t0 = time.time()
    while True:
        if fn():
            return time.time() - t0
        if time.time() - t0 > timeout:
            return None
        time.sleep(dt)


class Runner:
    def __init__(self, p, args):
        self.p = p
        self.args = args
        self.shots = {}
        self.current = None

    # --- game state ---------------------------------------------------------------------------
    def record(self):
        return RECORDS + self.p.read8(LOCAL_SLOT) * RECORD_STRIDE

    def heading(self):
        raw = self.p.read16(self.record() + HEADING)
        return struct.unpack("<h", struct.pack("<H", raw))[0] * 360.0 / 65536.0

    def stance(self):
        return self.p.read8(self.record() + STANCE)

    def position(self):
        base = self.record() + POSITION
        return tuple(u2f(self.p.read32(base + i * 4)) for i in range(3))

    def aim_pitch(self):
        return s16(self.p.read16(self.record() + AIM_PITCH))

    def servo_words(self):
        """The servo words that do not hold the game's own value, as 'address=word' strings."""
        found = ((a, own, self.p.read32(a)) for a, own in PITCH_SERVO)
        return ["0x%06X=0x%08X" % (a, w) for a, own, w in found if w != own]

    def cam_yaw(self):
        """The game camera's yaw from target minus eye (0 faces +Z, counter-clockwise positive)."""
        ex, ez = u2f(self.p.read32(EYE)), u2f(self.p.read32(EYE + 8))
        tx, tz = u2f(self.p.read32(TARGET)), u2f(self.p.read32(TARGET + 8))
        return math.degrees(math.atan2(tx - ex, tz - ez))

    # --- helpers ------------------------------------------------------------------------------
    def controller(self, **kw):
        return FakeController(self.p, **kw)

    def wait_armed(self, timeout=5.0):
        tel = {}

        def armed():
            tel.update(self.p.telemetry())
            return tel["armed"] and tel["look_at_active"]

        if wait_until(armed, timeout, 0.05) is None:
            raise Fail("first person not active after %.0f s (armed %s, lookAt %s, disarm reason: %s)" % (
                timeout, tel.get("armed"), tel.get("look_at_active"), tel.get("disarm_reason")))
        return tel

    def raise_weapon(self, fc, aim=None):
        """Squeeze the right grip in the hip holster (held); returns the seconds until stance 2, or None."""
        fc.set_hand(RIGHT, aim=aim, grip=pose(pos=HOLSTER), head_relative=True, squeeze=0.0)
        time.sleep(0.15)
        fc.set_hand(RIGHT, aim=aim, grip=pose(pos=HOLSTER), head_relative=True, squeeze=1.0)
        return wait_until(lambda: self.stance() == 2, 0.5)

    def squeeze_belt(self, fc):
        fc.set_hand(LEFT, grip=pose(pos=BELT), head_relative=True, squeeze=0.0)
        time.sleep(0.2)
        fc.set_hand(LEFT, grip=pose(pos=BELT), head_relative=True, squeeze=1.0)
        time.sleep(0.3)
        fc.set_hand(LEFT, grip=pose(pos=BELT), head_relative=True, squeeze=0.0)

    def track_yaw(self, seconds, dt=0.05):
        """Unwrapped change of the camera yaw over `seconds` (degrees), and the samples' spread."""
        prev = first = self.cam_yaw()
        total, lo, hi = 0.0, 0.0, 0.0
        t0 = time.time()
        while time.time() - t0 < seconds:
            time.sleep(dt)
            y = self.cam_yaw()
            total += wrap(y - prev)
            prev = y
            lo, hi = min(lo, total), max(hi, total)
        return total, hi - lo, first

    def shot(self, label):
        """A Headset Window capture (the emulator window without the null headset) for the report."""
        if not self.args.report:
            return
        path = os.path.join(self.args.report, "%s-%s.png" % (self.current, label))
        try:
            import hmdshot
            hmdshot.shot(path)
        except Exception:
            try:
                import drive
                drive.screenshot(path, scale=2)
            except Exception as e:
                print("    (no capture: %s)" % e)
                return
        self.shots.setdefault(self.current, []).append(os.path.basename(path))


# --- scenarios: each returns a one-line detail on PASS, raises Fail / Skip ---------------------

def sc_arm(r):
    with r.controller():
        r.wait_armed()
        cut = r.p.read32(CUT_HANDLER)
        slot = r.p.read8(LOCAL_SLOT)
        focus = r.p.read8(FOCUS_SLOT)
        cull = u2f(r.p.read32(NEAR_CULL))
        r.shot("armed")
    want_focus = 0 if slot == 3 else 3
    check(cut == 0, "cut handler 0x%06X = 0x%08X, expected 0 (silenced)" % (CUT_HANDLER, cut))
    check(focus == want_focus, "camera focus slot %d, expected %d (local slot %d)" % (focus, want_focus, slot))
    check(abs(cull - 75.0) < 1e-3, "near-cull radius %.2f, expected 75.0" % cull)
    return "armed, cut handler NOP, focus slot %d, near-cull %.1f" % (focus, cull)


def sc_smooth_turn(r):
    with r.controller() as fc:
        r.wait_armed()
        time.sleep(0.3)
        state = {"last": r.cam_yaw(), "total": 0.0}

        def follow(seconds):
            # Sampled often enough (6 deg per sample at full speed) to unwrap a half turn.
            t0 = time.time()
            while time.time() - t0 < seconds:
                time.sleep(0.05)
                y = r.cam_yaw()
                state["total"] += wrap(y - state["last"])
                state["last"] = y

        fc.set_hand(RIGHT, stick=(1.0, 0.0))
        follow(1.5)
        fc.clear_hand(RIGHT)
        follow(0.3)  # the frames still on their way when the stick let go
        total = state["total"]
        r.shot("turned")
    want = -SMOOTH_TURN_DEG_S * 1.5
    check(abs(total - want) <= 20.0, "camera yaw changed %.1f deg, expected %.0f +-20 (right stick turns right)" % (total, want))
    return "camera yaw changed %.1f deg (expected %.0f +-20)" % (total, want)


def sc_holster(r):
    with r.controller() as fc:
        r.wait_armed()
        check(r.stance() == 1, "stance %d before the holster, expected 1 (normal)" % r.stance())
        took = r.raise_weapon(fc)
        check(took is not None, "stance %d 0.5 s after squeezing in the holster, expected 2 (weapon raised)" % r.stance())
        r.shot("raised")
        fc.set_hand(RIGHT, grip=pose(pos=HOLSTER), head_relative=True, squeeze=0.0)
        back = wait_until(lambda: r.stance() == 1, 1.5)
        check(back is not None, "stance %d 1.5 s after opening the grip, expected 1" % r.stance())
    return "weapon raised after %.2f s, lowered %.2f s after release" % (took, back)


def sc_point_to_aim(r):
    with r.controller() as fc:
        r.wait_armed()
        took = r.raise_weapon(fc, aim=pose(yaw_deg=0.0))
        check(took is not None, "the holster did not raise the weapon (stance %d)" % r.stance())
        tel = r.p.telemetry()
        check(tel["weapon_raised"], "telemetry does not see the weapon raised")
        anchor = tel["yaw_anchor"]
        fc.set_hand(RIGHT, aim=pose(yaw_deg=30.0), grip=pose(pos=HOLSTER), head_relative=True, squeeze=1.0)
        time.sleep(0.4)
        heading = r.heading()
        r.shot("aimed")
    err = wrap(heading - (anchor + 30.0))
    check(abs(err) <= 2.0, "heading %.1f deg, expected anchor %.1f + 30 (off by %.1f)" % (heading, anchor, err))
    return "heading %.1f = anchor %.1f + 30 (off by %.2f)" % (heading, anchor, err)


def sc_walk_and_shoot(r):
    with r.controller() as fc:
        r.wait_armed()
        took = r.raise_weapon(fc, aim=pose())
        check(took is not None, "the holster did not raise the weapon (stance %d)" % r.stance())
        time.sleep(0.2)
        x0, _, z0 = r.position()
        yaw = r.cam_yaw()
        fc.set_hand(LEFT, stick=(0.0, 1.0))
        time.sleep(2.0)
        fc.clear_hand(LEFT)
        time.sleep(0.2)
        x1, _, z1 = r.position()
        r.shot("walked")
    dx, dz = x1 - x0, z1 - z0
    dist = math.hypot(dx, dz)
    want = AIM_MOVE_UNITS_S * 2.0
    check(abs(dist - want) <= 20.0, "moved %.1f units, expected %.0f +-20 (blocked by a wall? use --slot)" % (dist, want))
    off = wrap(math.degrees(math.atan2(dx, dz)) - yaw)
    check(abs(off) <= 15.0, "moved %.1f deg off the camera yaw %.1f" % (off, yaw))
    return "moved %.1f units, %.1f deg off the camera yaw" % (dist, off)


def sc_pitch(r):
    """Point the gun 20 deg down: the servo is NOP'd and +0xBC8 holds the controller's pitch; lower it and
    the servo is back within 6 vsyncs."""
    deg = -20.0
    want = pitch_units(deg)
    aim = pose(pitch_deg=deg)
    with r.controller() as fc:
        r.wait_armed()
        check(r.stance() == 1, "stance %d before the holster, expected 1 (normal)" % r.stance())
        live = r.servo_words()
        check(not live, "servo already changed with the weapon lowered: %s" % ", ".join(live))
        took = r.raise_weapon(fc, aim=aim)
        check(took is not None, "the holster did not raise the weapon (stance %d)" % r.stance())
        time.sleep(0.2)
        worst, samples = 0, 0
        t0 = time.time()
        while time.time() - t0 < 2.0:
            v = r.aim_pitch()
            check(abs(v - want) <= 2, "+0x%X = %d while aiming %.0f deg, expected %d +-2 (pitchSign wrong, or the servo "
                  "still runs?)" % (AIM_PITCH, v, deg, want))
            first = r.p.read32(PITCH_SERVO[0][0])
            check(first == 0, "servo word 0x%06X = 0x%08X while aiming, expected 0 (silenced)" % (PITCH_SERVO[0][0], first))
            worst, samples = max(worst, abs(v - want)), samples + 1
            time.sleep(0.05)
        patched = [a for a, _ in PITCH_SERVO if r.p.read32(a) != 0]
        check(not patched, "servo words not silenced while aiming: %s" % ", ".join("0x%06X" % a for a in patched))
        r.shot("aim-low")
        # Lower the weapon: every word goes back as soon as the stance drops (6 vsyncs at 60 Hz).
        fc.set_hand(RIGHT, aim=aim, grip=pose(pos=HOLSTER), head_relative=True, squeeze=0.0)
        check(wait_until(lambda: r.stance() != 2, 1.5, 0.005) is not None,
              "stance still 2 1.5 s after opening the grip")
        back = wait_until(lambda: not r.servo_words(), 1.0, 0.005)
        check(back is not None, "servo not restored 1 s after lowering the weapon: %s" % ", ".join(r.servo_words()))
        check(back <= 6.0 / 60.0 + 0.05, "servo restored %.0f ms after the stance dropped, expected within 6 vsyncs" %
              (back * 1000.0))
        time.sleep(0.5)
        after = r.aim_pitch()
    return "+0x%X held %d (worst off %d over %d samples); servo restored %.0f ms after lowering; pitch %d 0.5 s later" % (
        AIM_PITCH, want, worst, samples, back * 1000.0, after)


def sc_pitch_online_safe(r):
    """Online-safe: R1 + stick up on the virtual pad. The servo must never be silenced and the game's own
    code must tilt the aim (stick up eases +0xBC8 toward +8192)."""
    if not r.p.telemetry()["online_safe"]:
        raise Skip("needs VR online-safe or the network adapter on")
    try:
        import padd
    except ImportError:
        raise Skip("padd.py not found: cannot raise the weapon without scripted input")
    rec = r.record()
    error = []

    def hold():
        try:
            padd.send({"op": "hold", "b": ["r1"], "ms": 2000, "ly": 1.0})
        except OSError as e:
            error.append(e)

    watch_ok = True
    try:
        r.p.memwatch_arm(rec + AIM_PITCH, size=2)
    except PineError:
        watch_ok = False
    hits = []
    raised, peak, silenced = False, -32768, []
    try:
        t = threading.Thread(target=hold, daemon=True)
        t.start()
        t0 = time.time()
        while t.is_alive() and time.time() - t0 < 5.0:
            raised = raised or r.stance() == 2
            peak = max(peak, r.aim_pitch())
            silenced += [a for a, _ in PITCH_SERVO if a not in silenced and r.p.read32(a) == 0]
            time.sleep(0.05)
        t.join(1.0)
        if watch_ok:
            try:
                hits, _ = r.p.memwatch_poll()
            except PineError:
                watch_ok = False
    finally:
        if watch_ok:
            try:
                r.p.memwatch_clear()
            except PineError:
                pass
    if error:
        raise Skip("padd not running (python padd.py): %s" % error[0])
    check(raised, "R1 on the virtual pad did not raise the weapon (stance %d)" % r.stance())
    check(not silenced, "servo silenced with online-safe on: %s" % ", ".join("0x%06X" % a for a in silenced))
    check(peak > 2000, "+0x%X peaked at %d with the stick up while aiming; the game's servo should tilt it toward +8192"
          % (AIM_PITCH, peak))
    writers = sorted({h["pc"] for h in hits if h["is_write"]})
    seen = ("MemWatch writers %s" % ", ".join("0x%06X" % pc for pc in writers)) if writers else (
        "MemWatch saw no writes" if watch_ok else "MemWatch unavailable")
    if writers:
        check(any(pc in PITCH_SERVO_STORES for pc in writers),
              "+0x%X was written, but not by the servo stores (%s)" % (AIM_PITCH, seen))
    return "servo never silenced, pitch peaked at %d (the game's own tilt); %s" % (peak, seen)


def sc_walk_online_safe(r):
    if not r.p.telemetry()["online_safe"]:
        raise Skip("needs VR online-safe or the network adapter on")
    try:
        r.p.fake_input(ttl_ms=100)
        raise Fail("MsgVRFakeInput was accepted with online-safe on")
    except PineError:
        pass
    # The game's own aim mode with the stick up (virtual pad) must not move the character either.
    x0, _, z0 = r.position()
    try:
        import padd
        padd.send({"op": "hold", "b": ["r1"], "ms": 2000, "ly": 1.0})
        used_pad = True
    except OSError:
        time.sleep(2.0)
        used_pad = False
    time.sleep(0.2)
    x1, _, z1 = r.position()
    dist = math.hypot(x1 - x0, z1 - z0)
    check(dist < 5.0, "moved %.1f units with the weapon raised and online-safe on" % dist)
    return "test input refused; moved %.1f units%s" % (dist, " (R1 + stick up on the virtual pad)" if used_pad else
                                                            " (padd not running: no pad input)")


def sc_belt(r):
    with r.controller() as fc:
        r.wait_armed()
        check(r.p.read8(MENU_OPEN) == 0, "the item screen is already open")
        r.squeeze_belt(fc)
        opened = wait_until(lambda: r.p.read8(MENU_OPEN) == 1, 2.0)
        check(opened is not None, "item screen flag %d after squeezing at the belt, expected 1" % r.p.read8(MENU_OPEN))
        try:
            time.sleep(0.5)
            check(r.p.telemetry()["pause_when_hit"], "telemetry does not see pauseWhen")
            # The camera must hold still while the menu is up, even with the head turning.
            fc.set_head(pose(yaw_deg=20.0))
            _, spread, _ = r.track_yaw(1.0)
            r.shot("menu")
            fc.set_head(pose())
            check(spread < 0.5, "camera yaw moved %.2f deg with the item screen open" % spread)
            time.sleep(0.3)
        finally:
            # Close it whatever happened, so the next scenario starts in the room.
            fc.set_head(pose())
            r.squeeze_belt(fc)
        closed = wait_until(lambda: r.p.read8(MENU_OPEN) == 0, 2.5)
        check(closed is not None, "item screen flag %d after the second squeeze, expected 0" % r.p.read8(MENU_OPEN))
    return "opened after %.2f s, camera held (%.2f deg), closed after %.2f s" % (opened, spread, closed)


def sc_body_follow(r):
    with r.controller() as fc:
        r.wait_armed()
        time.sleep(0.3)
        check(r.stance() == 1, "stance %d, expected 1 (idle) for body-follow" % r.stance())
        anchor = r.p.telemetry()["yaw_anchor"]
        fc.set_head(pose(yaw_deg=40.0))
        time.sleep(0.6)
        cam = r.cam_yaw()
        heading = r.heading()
        r.shot("follow")
    cam_err = wrap(cam - (anchor + 40.0))
    check(abs(cam_err) <= 2.0, "camera yaw %.1f, expected anchor %.1f + 40 (head yaw)" % (cam, anchor))
    err = wrap(heading - cam)
    check(abs(err) <= 2.0, "heading %.1f does not follow the camera yaw %.1f (off by %.1f)" % (heading, cam, err))
    return "camera %.1f, heading %.1f (off by %.2f)" % (cam, heading, err)


def sc_savestate_yaw(r):
    slot = r.args.scratch_slot
    with r.controller() as fc:
        r.wait_armed()
        time.sleep(0.5)
        saved_heading = r.heading()
        before = r.cam_yaw()
        r.p.save_state(slot)
        time.sleep(3.0)
        # Turn the view about 90 deg right, so the view no longer matches the saved heading.
        fc.set_hand(RIGHT, stick=(1.0, 0.0))
        time.sleep(0.75)
        fc.clear_hand(RIGHT)
        time.sleep(0.5)
        turned = r.cam_yaw()
        check(abs(wrap(turned - before)) >= 45.0, "the view did not turn before the load (%.1f -> %.1f)" % (before, turned))
        r.p.load_state(slot)
        time.sleep(r.args.settle)
        r.wait_armed()
        time.sleep(0.3)
        after = r.cam_yaw()
        r.shot("loaded")
    err = wrap(after - saved_heading)
    check(abs(err) <= 5.0, "after the load the camera faces %.1f, the saved heading is %.1f (kept the pre-load view %.1f?)" % (
        after, saved_heading, turned))
    return "camera %.1f vs saved heading %.1f (off by %.2f)" % (after, saved_heading, err)


SCENARIOS = [
    ("arm", sc_arm),
    ("smooth-turn", sc_smooth_turn),
    ("holster", sc_holster),
    ("point-to-aim", sc_point_to_aim),
    ("walk-and-shoot", sc_walk_and_shoot),
    ("walk-and-shoot-online-safe", sc_walk_online_safe),
    ("pitch", sc_pitch),
    ("pitch-online-safe", sc_pitch_online_safe),
    ("belt", sc_belt),
    ("body-follow", sc_body_follow),
    ("savestate-yaw", sc_savestate_yaw),
]

# The only ones that run with online-safe on (and skip without it).
ONLINE_SAFE_SCENARIOS = (sc_walk_online_safe, sc_pitch_online_safe)


def write_junit(path, results):
    suite = ET.Element("testsuite", name="outbreak-vr", tests=str(len(results)),
                       failures=str(sum(1 for r in results if r[1] == "FAIL")),
                       errors=str(sum(1 for r in results if r[1] == "ERROR")),
                       skipped=str(sum(1 for r in results if r[1] == "SKIP")))
    for name, result, detail, secs in results:
        case = ET.SubElement(suite, "testcase", classname="outbreak-vr." + SERIAL, name=name, time="%.2f" % secs)
        if result == "FAIL":
            ET.SubElement(case, "failure", message=detail.splitlines()[0] if detail else "").text = detail
        elif result == "ERROR":
            ET.SubElement(case, "error", message=detail.splitlines()[0] if detail else "").text = detail
        elif result == "SKIP":
            ET.SubElement(case, "skipped", message=detail)
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    ET.ElementTree(suite).write(path, encoding="utf-8", xml_declaration=True)


def write_report(folder, results, shots):
    rows = []
    for name, result, detail, secs in results:
        imgs = "".join('<a href="%s"><img src="%s" width="480"></a>' % (html.escape(s), html.escape(s))
                       for s in shots.get(name, []))
        rows.append("<tr class='%s'><td>%s</td><td>%s</td><td><pre>%s</pre></td><td>%.1f s</td><td>%s</td></tr>" % (
            result.lower(), html.escape(name), result, html.escape(detail), secs, imgs))
    page = ("<!doctype html><meta charset='utf-8'><title>vrtest %s</title><style>"
            "body{font-family:sans-serif;margin:16px}td{vertical-align:top;padding:4px 8px;border-bottom:1px solid #ccc}"
            "pre{white-space:pre-wrap;margin:0}.pass td:nth-child(2){color:#070}.fail td:nth-child(2),"
            ".error td:nth-child(2){color:#b00}.skip td:nth-child(2){color:#777}</style>"
            "<h1>vrtest %s, %s</h1><table>%s</table>" % (SERIAL, SERIAL, time.strftime("%Y-%m-%d %H:%M"), "".join(rows)))
    with open(os.path.join(folder, "report.html"), "w", encoding="utf-8") as f:
        f.write(page)


def main():
    names = [n for n, _ in SCENARIOS]
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("scenarios", nargs="*", help="'all' or names: " + ", ".join(names))
    ap.add_argument("--list", action="store_true", help="list the scenarios")
    ap.add_argument("--pine-slot", type=int, default=28011)
    ap.add_argument("--slot", type=int, help="savestate slot to load before every scenario")
    ap.add_argument("--scratch-slot", type=int, default=9, help="slot savestate-yaw saves to (overwritten)")
    ap.add_argument("--settle", type=float, default=3.0, help="seconds to wait after loading a state")
    ap.add_argument("--junit", help="write a JUnit XML file here")
    ap.add_argument("--report", help="write report.html and captures into this folder")
    args = ap.parse_args()

    if args.list or not args.scenarios:
        print("\n".join(names))
        return 0
    wanted = names if "all" in args.scenarios else args.scenarios
    unknown = [n for n in wanted if n not in names]
    if unknown:
        print("unknown scenario(s): %s (one of %s)" % (", ".join(unknown), ", ".join(names)))
        return 2
    if args.report:
        os.makedirs(args.report, exist_ok=True)

    try:
        p = wait_for_pine(args.pine_slot, seconds=10)
        serial = p.game_id()
        tel = p.telemetry()
    except PineError as e:
        print("PINE: %s (emulator running, EnablePINE on, a build with MsgVRTelemetry?)" % e)
        return 2
    if serial != SERIAL:
        print("these scenarios are for %s; the emulator is running %r" % (SERIAL, serial))
        return 2
    online_safe = tel["online_safe"]
    if not online_safe:
        try:
            p.fake_input(head=None, ttl_ms=0)
        except PineError:
            print("MsgVRFakeInput refused: start the emulator with PCSX2_VR_TEST_INPUT=1 in its environment.")
            return 2
    print("%s, telemetry v%d, online-safe %s" % (serial, tel["version"], "ON" if online_safe else "off"))

    runner = Runner(p, args)
    results = []
    for name in wanted:
        fn = dict(SCENARIOS)[name]
        runner.current = name
        t0 = time.time()
        try:
            if online_safe and fn not in ONLINE_SAFE_SCENARIOS:
                raise Skip("online-safe is on: scripted input is refused")
            if args.slot is not None:
                p.load_state(args.slot)
                time.sleep(args.settle)
            result, detail = "PASS", fn(runner)
        except Skip as e:
            result, detail = "SKIP", str(e)
        except Fail as e:
            result, detail = "FAIL", str(e)
        except Exception:
            result, detail = "ERROR", traceback.format_exc().strip()
        secs = time.time() - t0
        results.append((name, result, detail, secs))
        print("%-5s %-28s %s" % (result, name, detail.splitlines()[-1] if result == "ERROR" else detail), flush=True)

    failed = sum(1 for r in results if r[1] in ("FAIL", "ERROR"))
    print("%d passed, %d failed, %d skipped" % (sum(1 for r in results if r[1] == "PASS"), failed,
                                                sum(1 for r in results if r[1] == "SKIP")))
    if args.junit:
        write_junit(args.junit, results)
    if args.report:
        write_report(args.report, results, runner.shots)
    p.close()
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
