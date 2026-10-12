"""Headset-free tests for the VR menu pointer on File #1's item screen (SLPM-65428), over PINE: the pointer's hit on
the menu screen, steering the game's own cursor to each cell, the fast click, the pause after a confirm, the command
submenu (suspendWhen), overrides, closing the screen mid-steer, the map and pause after an item-screen visit, the menu
tablet and the cell rects; and the belt pouches on File #1 or File #2 (SLPM-65692). Reads the menu pointer with
MsgVRMenuPointer op 0 (pine.menu_pointer()), the pouches with op 2 (pine.pouches()), the game's cursor struct (File #1
0x720B80, File #2 0x7671F0) and its command-submenu flag (File #1 0x720BA0, File #2 0x767210). Prints PASS / FAIL /
SKIP per scenario; exits 1 if any failed.

Offline only: it loads savestates and sends scripted input, which must never happen online (it refuses with the
network adapter on). Start the emulator with PCSX2_VR_TEST_INPUT=1 in its environment and PINE on (slot 28011): in VR
on SteamVR's null headset (python null_driver.py on, then launch_vr.ps1; run null_driver.py off afterwards), or with
no OpenXR runtime at all for --test-uv. Be standing idle in a room with first person armed (J's Bar), or pass --slot
(File #1 slot 6: J's Bar, gun equipped, calm), which is loaded before every scenario.

  python menutest.py all --slot 6 --report out/   # pointer-hit, cells, fast-click, latch, submenu, override,
                                                   # close-in-flight, map-after-items, tablet
  python menutest.py map-after-items --test-uv --slot 6   # point-to-select stays off on the map and pause
  python menutest.py cells --test-uv --slot 6      # no OpenXR runtime: point with PINE test hits (op 1)
  python menutest.py rects --test-uv --slot 6 --report out/   # the cell rects against captures of the window
  python menutest.py --tablet-ini on               # emulator closed: [VR] MenuTablet = true (ini backed up)
  python menutest.py --tablet-ini off              # restores the ini byte for byte
  python menutest.py pouches pouch-open pouch-submenu --slot 6   # the belt pouches (File #2: --slot 1 or 2)

Scenarios: pointer-hit (the hit maths at 9 points, a turned-away aim misses, the left hand alone hits), cells (cells
1, 3, 2, 0, the equipped box and 0 again, each within 0.8 s, presses <= path + 1, a landing tick per cell),
fast-click (aim at cell 3 and pull the trigger at once: held back, passed only once the cursor is there), latch
(after a confirm, pointing elsewhere presses nothing until B), submenu (a confirm on the cursor's cell with the dot
moving straight on: the submenu's flag rises, nothing is pressed while it is up wherever the dot goes, and after B
closes it the cursor stays until the dot moves to another cell), override (the right stick stops a steer, nothing
pressed for 1 s), close-in-flight (closing the screen mid-steer: no D-pad from the poll it closes, the character does
not move), map-after-items (after a visit to the item screen its cursor struct still reads 1 and matches a cell, and
the menu flag is up for the map and pause too: with the map (Y) and then pause (Select, sent as Backspace through
drive.py, so Pad 1 needs its keyboard bindings) up, pointing at every cell for 0.6 s runs no point-to-select and
presses nothing; SKIP with the map's result if pause cannot be opened), tablet (needs MenuTablet on: its size, tilt
and the hit maths), rects (each cell's white highlight box, found in a capture, lies in the profile's rect).

Belt pouches (either File; the grip is put at each pouch's centre as pine.pouches() reports it, so any anchor works):
pouches (with the item screen up, an open hand reaching into each pouch ticks once, twice when the local record's
item byte says its cell holds an item, and a squeeze brings the cursor to the pouch's cell within 1 s with nothing
confirmed: no confirm passes, the command submenu flag stays 0, the screen stays up, no menu button press),
pouch-open (with the item screen closed a squeeze opens it through the belt zone and the cursor reaches the pouch's
cell within 1 s of the screen coming up, nothing confirmed), pouch-submenu (with an item's command submenu open a
squeeze is refused and nothing is pressed for 1 s; B backs out). On File #2 only these run; the rest SKIP.

Menu safety: the only confirm sent is one trigger pull on a cell (fast-click, latch, submenu, pouch-submenu), which
opens that cell's command submenu; the runner then backs out with B (Cross) and never confirms a submenu entry.
map-after-items sends no confirm at all: Y opens the map, Select opens pause, and B (Cross) closes each.
"""
import argparse
import html
import math
import os
import shutil
import struct
import sys
import tempfile
import threading
import time
import traceback
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pine import INI, FakeController, PineError, pose, require_offline, wait_for_pine, zone

SERIAL = "SLPM-65428"

# File #1 addresses (bin/resources/vr-profiles/SLPM-65428.yaml, menuPointer and camera).
MENU_OPEN = 0x48BF7E                     # u8 1 while the item screen, the map or pause is open
ITEM_UP = 0x720B80                       # u32 1 while the item screen is up
NODE, INDEX, CUR_X, CUR_Y = 0x720B84, 0x720B8C, 0x720B8E, 0x720B90   # the cursor struct
SUBMENU = 0x720BA0                       # u8 1 while an item's command submenu is open (the profile's suspendWhen)
OWN_GRID = 0x723F94                      # u8 1 while the cursor is in the own item grid (the profile's liveWhen)
RECORDS, RECORD_STRIDE, LOCAL_SLOT, POSITION = 0x476DD0, 0x10E0, 0x48BF7D, 0x38

# The profile's cells: name, node, index, x, y, rect (u0, v0, u1, v1 of the game picture), and the neighbours (up, down,
# left, right; -1 none).
CELLS = (
    ("cell 0", 0x720C10, 0, 498, 108, (0.807, 0.184, 0.895, 0.307), (-1, 2, -1, 1)),
    ("cell 1", 0x720CA8, 1, 550, 108, (0.895, 0.184, 0.982, 0.307), (-1, 3, 0, -1)),
    ("cell 2", 0x720D40, 2, 498, 160, (0.807, 0.307, 0.895, 0.430), (0, 4, -1, 3)),
    ("cell 3", 0x720DD8, 3, 550, 160, (0.895, 0.307, 0.982, 0.430), (1, 4, 2, -1)),
    ("equipped box", 0x722928, 0, 550, 216, (0.888, 0.456, 0.979, 0.581), (2, -1, -1, -1)),
)
# The world screen (profile screen: 2.5 m out, 1.3 m tall, 4:3) and the menu tablet (screen.menu).
WORLD_HALF = (0.867, 0.65)
TABLET_DISTANCE, TABLET_HEIGHT, TABLET_DROP = 0.70, 0.36, 0.30
BELT = zone(-0.22, -0.50, 0.12)
LEFT, RIGHT = 0, 1
ARM_DELAY_S = 0.45  # the profile's armDelayMs (300) with a margin

# The belt pouches (the profiles' pouches:) on either File: the menu flag (item screen, map or pause), the item screen's
# cursor struct (u32 1 while up; node pointer at +0x4, u16 index +0xC, x +0xE, y +0x10), the local record (records +
# local slot * stride) and its item bytes (+items + k for cell k, 0 = empty), the command submenu flag (the cursor
# struct + 0x20, 1 while an item's EQUIP/RELOAD/USE... submenu is open), and the menuPointer cells as (node, index, x,
# y) in the profile's order.
GAMES = {
    "SLPM-65428": dict(menu_open=0x48BF7E, item_up=0x720B80, records=0x476DD0, stride=0x10E0, local_slot=0x48BF7D,
                       items=0xC84, submenu=0x720BA0,
                       cells=((0x720C10, 0, 498, 108), (0x720CA8, 1, 550, 108), (0x720D40, 2, 498, 160),
                              (0x720DD8, 3, 550, 160), (0x722928, 0, 550, 216))),
    "SLPM-65692": dict(menu_open=0x4912BE, item_up=0x7671F0, records=0x47BD30, stride=0x1100, local_slot=0x4912BD,
                       items=0xC94, submenu=0x767210,
                       cells=((0x767280, 0, 498, 108), (0x767318, 1, 550, 108), (0x7673B0, 2, 498, 160),
                              (0x767448, 3, 550, 160), (0x768F98, 0, 550, 216))),
}
POUCH_SCENARIOS = ("pouches", "pouch-open", "pouch-submenu")


class Fail(Exception):
    pass


class Skip(Exception):
    pass


def check(ok, message):
    if not ok:
        raise Fail(message)


def wait_until(fn, timeout, dt=0.01):
    """Polls fn until it is truthy; returns the seconds it took, or None on timeout."""
    t0 = time.time()
    while True:
        if fn():
            return time.time() - t0
        if time.time() - t0 > timeout:
            return None
        time.sleep(dt)


def u2f(u):
    return struct.unpack("<f", struct.pack("<I", u))[0]


def add(a, b):
    return tuple(x + y for x, y in zip(a, b))


def sub(a, b):
    return tuple(x - y for x, y in zip(a, b))


def scale(a, s):
    return tuple(x * s for x in a)


def normalize(a):
    n = math.sqrt(sum(x * x for x in a))
    return tuple(x / n for x in a) if n > 1e-9 else (0.0, 0.0, 0.0)


def rotate(q, v):
    """v rotated by the unit quaternion q (x, y, z, w)."""
    qx, qy, qz, qw = q
    cx = qy * v[2] - qz * v[1]
    cy = qz * v[0] - qx * v[2]
    cz = qx * v[1] - qy * v[0]
    ccx = qy * cz - qz * cy
    ccy = qz * cx - qx * cz
    ccz = qx * cy - qy * cx
    return (v[0] + 2 * (qw * cx + ccx), v[1] + 2 * (qw * cy + ccy), v[2] + 2 * (qw * cz + ccz))


def path_length(frm, to):
    """Presses on the shortest way from cell frm to cell to over the profile's neighbours (-1 if none)."""
    seen = {frm: 0}
    queue = [frm]
    while queue:
        cur = queue.pop(0)
        if cur == to:
            return seen[cur]
        for nb in CELLS[cur][6]:
            if nb >= 0 and nb not in seen:
                seen[nb] = seen[cur] + 1
                queue.append(nb)
    return -1


def rect_centre(cell):
    u0, v0, u1, v1 = CELLS[cell][5]
    return 0.5 * (u0 + u1), 0.5 * (v0 + v1)


class TestHit:
    """With --test-uv: keeps a PINE test hit (MsgVRMenuPointer op 1) alive, re-sent every 0.1 s (it lapses after its
    ttl), so point-to-select can be driven without an OpenXR runtime."""

    def __init__(self, pine):
        self.pine = pine
        self.uv = None
        self.hand = RIGHT
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _send(self):
        with self._lock:
            uv, hand = self.uv, self.hand
        if uv is None:
            self.pine.set_test_screen_hit(hand, -1.0, 0.0, 0)
        else:
            self.pine.set_test_screen_hit(hand, uv[0], uv[1], 400)

    def _run(self):
        while not self._stop.wait(0.1):
            try:
                with self._lock:
                    live = self.uv is not None
                if live:
                    self._send()
            except (PineError, OSError):
                return

    def set(self, hand, u, v):
        with self._lock:
            self.uv, self.hand = (u, v), hand
        self._send()

    def clear(self):
        with self._lock:
            self.uv = None
        self._send()

    def stop(self):
        self._stop.set()
        self._thread.join()
        try:
            self.clear()
        except (PineError, OSError):
            pass


class Runner:
    def __init__(self, p, args):
        self.p = p
        self.args = args
        self.shots = {}
        self.current = None
        self.test_hit = None
        self.aiming = {}  # hand -> (u, v, yaw_off) the hand aims at, for re-sends with buttons
        self.serial = SERIAL  # the game running (main sets it): the pouch scenarios read its GAMES entry
        self.game = GAMES[SERIAL]

    # --- game state -----------------------------------------------------------------------------------------------
    def cursor(self):
        return self.p.read32(NODE), self.p.read16(INDEX), self.p.read16(CUR_X), self.p.read16(CUR_Y)

    def cell(self):
        c = self.cursor()
        for i, cell in enumerate(CELLS):
            if c == (cell[1], cell[2], cell[3], cell[4]):
                return i
        return -1

    def position(self):
        base = RECORDS + self.p.read8(LOCAL_SLOT) * RECORD_STRIDE + POSITION
        return tuple(u2f(self.p.read32(base + i * 4)) for i in range(3))

    def menu_open(self):
        return self.p.read8(MENU_OPEN) == 1

    def mp(self):
        return self.p.menu_pointer()

    # --- controls ---------------------------------------------------------------------------------------------------
    def controller(self):
        return FakeController(self.p)

    def wait_armed(self, timeout=5.0):
        tel = {}

        def armed():
            tel.update(self.p.telemetry())
            return tel["armed"] and tel["look_at_active"]

        if wait_until(armed, timeout, 0.05) is None:
            raise Fail("first person not active after %.0f s (armed %s, lookAt %s, disarm reason: %s)" % (
                timeout, tel.get("armed"), tel.get("look_at_active"), tel.get("disarm_reason")))
        return tel

    def squeeze_belt(self, fc, hold=0.3):
        fc.set_hand(LEFT, grip=pose(pos=BELT), head_relative=True, squeeze=0.0)
        time.sleep(0.2)
        fc.set_hand(LEFT, grip=pose(pos=BELT), head_relative=True, squeeze=1.0)
        time.sleep(hold)
        fc.set_hand(LEFT, grip=pose(pos=BELT), head_relative=True, squeeze=0.0)

    def open_items(self, fc):
        """Opens the item screen with the belt (first person must be armed) and waits for point-to-select to run."""
        if self.menu_open():
            raise Fail("a menu is already open (load a calm savestate with --slot)")
        self.squeeze_belt(fc)
        fc.clear_hand(LEFT)
        if wait_until(lambda: self.menu_open() and self.p.read32(ITEM_UP) == 1, 3.0) is None:
            raise Fail("the item screen did not open on the belt squeeze (menu byte %d)" % self.p.read8(MENU_OPEN))
        m = {}

        def running():
            m.update(self.mp())
            return m["gate"]

        if wait_until(running, 2.0) is None:
            raise Fail("point-to-select did not start (menu mode %s, refused online %s, self-check failed %s; "
                       "the MenuPointer setting on?)" % (m.get("menu_mode"), m.get("refused_online"),
                                                         m.get("selfcheck_failed")))
        time.sleep(ARM_DELAY_S)

    def close_items(self, fc):
        if not self.menu_open():
            return
        self.point_clear(fc)
        self.squeeze_belt(fc)
        fc.clear_hand(LEFT)
        if wait_until(lambda: not self.menu_open(), 3.0) is None:
            raise Fail("the item screen did not close on the belt squeeze")
        time.sleep(0.3)

    def aim_pose(self, m, u, v, yaw_off=0.0):
        """An absolute aim pose in the XR base space whose ray meets the published screen m at (u, v), from in front of
        its centre (2.2 m out for the world screen, 0.45 m for the tablet); yaw_off turns it away about +Y."""
        hw, hh, q = m["half_width"], m["half_height"], m["quat"]
        target = add(m["centre"], rotate(q, ((u - 0.5) * 2.0 * hw, (0.5 - v) * 2.0 * hh, 0.0)))
        normal = rotate(q, (0.0, 0.0, 1.0))
        origin = add(m["centre"], scale(normal, 0.45 if m["tablet"] else 2.2))
        d = normalize(sub(target, origin))
        yaw = math.degrees(math.atan2(-d[0], -d[2])) + yaw_off
        pitch = math.degrees(math.asin(max(-1.0, min(1.0, d[1]))))
        return pose(yaw, pitch, origin)

    def aim(self, fc, hand, u, v, yaw_off=0.0, trigger=0.0, buttons=(), stick=(0.0, 0.0)):
        """Points hand at (u, v): a real ray at the published screen, or with --test-uv a PINE test hit (sent first, so
        a trigger in the same call never lands before it)."""
        self.aiming[hand] = (u, v, yaw_off)
        if self.test_hit is not None:
            self.test_hit.set(hand, u, v)
            time.sleep(0.02)
            fc.set_hand(hand, trigger=trigger, buttons=buttons, stick=stick)
            return
        m = self.mp()
        if not m["transform_valid"]:
            raise Fail("no screen transform published (VR running, the world screen flat with arc 0?)")
        p = self.aim_pose(m, u, v, yaw_off)
        fc.set_hand(hand, aim=p, grip=p, trigger=trigger, buttons=buttons, stick=stick)

    def point(self, fc, cell, **kw):
        u, v = rect_centre(cell)
        self.aim(fc, RIGHT, u, v, **kw)

    def point_clear(self, fc):
        self.aiming.clear()
        if self.test_hit is not None:
            self.test_hit.clear()
        fc.clear_hand(RIGHT)

    def press_b(self, fc):
        """B (Cross) for 0.15 s, the right hand still aiming where it was."""
        u, v, yaw_off = self.aiming.get(RIGHT, (None, None, 0.0))
        if u is None or self.test_hit is not None:
            fc.set_hand(RIGHT, buttons=("b",))
            time.sleep(0.15)
            fc.set_hand(RIGHT)
        else:
            self.aim(fc, RIGHT, u, v, yaw_off, buttons=("b",))
            time.sleep(0.15)
            self.aim(fc, RIGHT, u, v, yaw_off)
        time.sleep(0.3)

    def shot(self, label):
        """A Headset Window capture (or the emulator window without the null headset) for the report."""
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

    def steer_to(self, fc, cell, timeout=0.8):
        """Points at cell; returns (seconds until the cursor is there or None, presses, landing ticks)."""
        m0 = self.mp()
        self.point(fc, cell)
        took = wait_until(lambda: self.cell() == cell, timeout, 0.005)
        time.sleep(0.15)  # the last landing tick
        m1 = self.mp()
        return took, m1["presses"] - m0["presses"], m1["ticks"] - m0["ticks"]

    # --- belt pouches (either File: self.game) ----------------------------------------------------------------------
    def g_menu_open(self):
        return self.p.read8(self.game["menu_open"]) == 1

    def g_item_up(self):
        return self.p.read32(self.game["item_up"]) == 1

    def g_cursor(self):
        s = self.game["item_up"]
        return self.p.read32(s + 0x4), self.p.read16(s + 0xC), self.p.read16(s + 0xE), self.p.read16(s + 0x10)

    def g_cell(self):
        c = self.g_cursor()
        for i, cell in enumerate(self.game["cells"]):
            if c == cell:
                return i
        return -1

    def g_submenu(self):
        return self.p.read8(self.game["submenu"])

    def g_items(self):
        """The local character's four item bytes, cell 0 first (0 = an empty cell)."""
        slot = self.p.read8(self.game["local_slot"])
        base = self.game["records"] + slot * self.game["stride"] + self.game["items"]
        return [self.p.read8(base + k) for k in range(4)]

    def g_open_items(self, fc):
        """Opens the item screen with the belt zone (first person must be armed) and waits for point-to-select to run."""
        if self.g_menu_open():
            raise Fail("a menu is already open (load a calm savestate with --slot)")
        self.squeeze_belt(fc)
        fc.clear_hand(LEFT)
        if wait_until(lambda: self.g_menu_open() and self.g_item_up(), 3.0) is None:
            raise Fail("the item screen did not open on the belt squeeze (menu byte %d)" % self.p.read8(self.game["menu_open"]))
        m = {}

        def running():
            m.update(self.mp())
            return m["gate"]

        if wait_until(running, 2.0) is None:
            raise Fail("point-to-select did not start (menu mode %s, refused online %s, self-check failed %s; "
                       "the MenuPointer setting on?)" % (m.get("menu_mode"), m.get("refused_online"),
                                                         m.get("selfcheck_failed")))
        time.sleep(ARM_DELAY_S)

    def g_close_items(self, fc):
        if not self.g_menu_open():
            return
        self.point_clear(fc)
        self.squeeze_belt(fc)
        fc.clear_hand(LEFT)
        if wait_until(lambda: not self.g_menu_open(), 3.0) is None:
            raise Fail("the item screen did not close on the belt squeeze")
        time.sleep(0.3)

    def pouch_state(self):
        """pine.pouches(), failing unless the pouches run (first person armed, a pouches block in the profile)."""
        try:
            t = self.p.pouches()
        except PineError as e:
            raise Fail("PINE MsgVRMenuPointer op 2 failed (%s): a build without the belt pouches?" % e)
        if not t["active"]:
            tel = self.p.telemetry()
            raise Fail("the belt pouches are not running (armed %s, disarm reason: %s; a pouches block in the profile?)" % (
                tel["armed"], tel["disarm_reason"]))
        return t

    def mapped_pouches(self, t):
        """(pouch, cell) for each pouch whose cell this runner can read the cursor on, in the profile's order."""
        return [(k, pc["cell"]) for k, pc in enumerate(t["pouches"]) if 0 <= pc["cell"] < len(self.game["cells"])]

    def pouch_hand(self, fc, hand, pouch, squeeze):
        """The hand's grip at the pouch's centre as the emulator places it now (pine.pouches(), the XR base space, where
        the hands are), open (squeeze 0) or squeezed (1); no aim, so it points at nothing."""
        centre = self.p.pouches()["pouches"][pouch]["centre"]
        fc.set_hand(hand, grip=pose(pos=centre), squeeze=squeeze)

    def squeeze_pouch(self, fc, hand, pouch, hold=0.3):
        """Reaches into the pouch with an open hand, squeezes for hold seconds and opens the hand again (still there)."""
        self.pouch_hand(fc, hand, pouch, 0.0)
        time.sleep(0.2)
        self.pouch_hand(fc, hand, pouch, 1.0)
        time.sleep(hold)
        self.pouch_hand(fc, hand, pouch, 0.0)


# --- scenarios: each returns a one-line detail on PASS, raises Fail / Skip ---------------------------------------------

def sc_pointer_hit(r):
    if r.args.test_uv:
        raise Skip("needs the null headset: it aims real rays at the published screen")
    with r.controller() as fc:
        r.wait_armed()
        r.open_items(fc)
        m = r.mp()
        check(m["transform_valid"] and not m["tablet"], "no world-screen transform published (tablet %s)" % m["tablet"])
        check(abs(m["half_width"] - WORLD_HALF[0]) < 0.01 and abs(m["half_height"] - WORLD_HALF[1]) < 0.01,
              "half extents %.3f x %.3f m, expected %.3f x %.3f (2.5 m out, 1.3 m tall, 4:3)" % (
                  m["half_width"], m["half_height"], WORLD_HALF[0], WORLD_HALF[1]))
        worst = grid_hits(r, fc)
        r.aim(fc, RIGHT, 0.5, 0.5, yaw_off=70.0)
        time.sleep(0.15)
        check(not r.mp()["hit_valid"], "the aim turned 70 deg away still hits the screen")
        fc.clear_hand(RIGHT)
        r.aiming.pop(RIGHT, None)
        r.aim(fc, LEFT, 0.5, 0.5)
        time.sleep(0.15)
        mm = r.mp()
        check(mm["hit_valid"] and not mm["hit_right"] and abs(mm["u"] - 0.5) < 0.01,
              "the left hand alone does not hit (hit %s, right %s, u %.3f)" % (mm["hit_valid"], mm["hit_right"], mm["u"]))
        fc.clear_hand(LEFT)
        r.aim(fc, RIGHT, 0.6, 0.45)
        time.sleep(0.4)
        r.shot("beam-dot-hands")
        r.close_items(fc)
    return "9 grid points within %.4f, 70 deg away misses, the left hand alone hits" % worst


def grid_hits(r, fc, hand=RIGHT):
    """Aims at a 3 x 3 grid; checks each hit's u, v within 0.01. Returns the worst error."""
    worst = 0.0
    for v in (0.2, 0.5, 0.8):
        for u in (0.2, 0.5, 0.8):
            r.aim(fc, hand, u, v)
            time.sleep(0.15)
            m = r.mp()
            check(m["hit_valid"] and m["hit_right"] == (hand == RIGHT),
                  "no %s-hand hit aiming at (%.1f, %.1f)" % ("right" if hand == RIGHT else "left", u, v))
            err = max(abs(m["u"] - u), abs(m["v"] - v))
            check(err < 0.01, "aimed at (%.2f, %.2f), hit (%.3f, %.3f)" % (u, v, m["u"], m["v"]))
            worst = max(worst, err)
    return worst


def sc_cells(r):
    with r.controller() as fc:
        r.wait_armed()
        r.open_items(fc)
        check(r.cell() == 0, "the item screen opened on cell %d (cursor %s), expected cell 0" % (r.cell(), r.cursor()))
        details = []
        cur = 0
        for target in (1, 3, 2, 0, 4, 0):
            path = path_length(cur, target)
            took, presses, ticks = r.steer_to(fc, target)
            name = CELLS[target][0]
            check(took is not None, "%s -> %s: the cursor did not get there within 0.8 s (cursor %s, presses %d, last "
                  "abort?)" % (CELLS[cur][0], name, r.cursor(), presses))
            check(presses <= path + 1, "%s -> %s took %d presses for a %d-press path" % (CELLS[cur][0], name, presses, path))
            check(ticks >= path, "%s -> %s gave %d landing tick(s) for %d cell(s)" % (CELLS[cur][0], name, ticks, path))
            details.append("%s %.2f s/%d" % (name, took, presses))
            cur = target
        r.shot("cells")
        r.close_items(fc)
    return "; ".join(details)


def sc_fast_click(r):
    with r.controller() as fc:
        r.wait_armed()
        r.open_items(fc)
        check(r.cell() == 0, "the item screen opened on cell %d, expected 0" % r.cell())
        m0 = r.mp()
        r.point(fc, 3, trigger=1.0)  # aim at cell 3 and pull the trigger at once
        held = False
        passed_cell = None
        t0 = time.time()
        while time.time() - t0 < 1.5:
            m = r.mp()
            held = held or m["confirm_held"]
            if m["confirms_passed"] > m0["confirms_passed"]:
                passed_cell = r.cell()  # read after the counter: the cursor must already be there
                break
            time.sleep(0.003)
        r.point(fc, 3, trigger=0.0)
        check(held, "the confirm was never held back while the cursor was on its way")
        check(passed_cell is not None, "the confirm never passed (aborts %d, swallowed %d)" % (
            r.mp()["aborts"] - m0["aborts"], r.mp()["confirms_swallowed"] - m0["confirms_swallowed"]))
        check(passed_cell == 3, "the confirm passed with the cursor on cell %d, not on cell 3" % passed_cell)
        time.sleep(0.5)
        r.shot("submenu")
        r.press_b(fc)  # back out of the command submenu with Cross
        r.shot("after-b")
        r.close_items(fc)
    return "held back, passed on cell 3 after %.2f s; B backed out" % (time.time() - t0)


def item_cell(r, fc):
    """A cell whose confirm opens a command submenu: one of the local character's own cells if it holds an item, else
    the equipped box (File #1 slot 6 has an empty grid and Kevin's .45 in the equipped box). Steers there first."""
    b = rec_base(r)
    for k in range(4):
        if b and r.p.read8(b + INV_OFFSET + k):
            target = k
            break
    else:
        target = 4
    if r.cell() != target:
        took, _, _ = r.steer_to(fc, target)
        check(took is not None, "could not steer to %s for the confirm (cursor %s)" % (CELLS[target][0], r.cursor()))
    return target


def rec_base(r):
    try:
        return 0x476DD0 + r.p.read8(0x48BF7D) * 0x10E0
    except Exception:
        return 0


INV_OFFSET = 0xC84


def sc_latch(r):
    with r.controller() as fc:
        r.wait_armed()
        r.open_items(fc)
        own = item_cell(r, fc)  # a confirm here opens a command submenu
        other = 1 if own != 1 else 2
        r.point(fc, own)
        time.sleep(0.2)
        m0 = r.mp()
        r.point(fc, own, trigger=1.0)  # a confirm on the cursor's own cell: its command submenu
        time.sleep(0.2)
        r.point(fc, own, trigger=0.0)
        time.sleep(0.3)
        m1 = r.mp()
        check(m1["confirms_passed"] == m0["confirms_passed"] + 1, "the confirm on the cursor's own cell did not pass")
        # File #1's profile has suspendWhen (the submenu flag), so the pause is the submenu's; without it, the latch.
        check(m1["suspended"], "steering is not paused after a confirm (no submenu suspension, no latch)")
        r.shot("submenu")
        r.point(fc, other)
        time.sleep(1.0)
        m2 = r.mp()
        check(m2["presses"] == m1["presses"], "paused, pointing at %s still pressed the D-pad %d time(s)" % (
            CELLS[other][0], m2["presses"] - m1["presses"]))
        r.press_b(fc)
        m3 = r.mp()
        check(not (m3["gate"] and m3["suspended"]), "the pause did not let go after B")
        r.close_items(fc)
    return "a confirm on %s paused steering; pointing at %s pressed nothing for 1 s; B let go" % (CELLS[own][0],
                                                                                               CELLS[other][0])


def sc_submenu(r):
    """A confirm on the cursor's own cell opens its command submenu; the dot moves straight on to another cell (the
    race the wait after a confirm covers). While the submenu's flag (0x720BA0) is up the pointer is suspended and presses
    nothing wherever the dot goes; after B (Cross) closes it, the cursor stays put with the dot still on another cell,
    until the dot moves to a different cell."""
    with r.controller() as fc:
        r.wait_armed()
        r.open_items(fc)
        check(r.p.read8(SUBMENU) == 0, "the submenu flag reads %d with no submenu open" % r.p.read8(SUBMENU))
        own = item_cell(r, fc)  # a confirm here opens a command submenu
        other = 1 if own != 1 else 2
        third = next(c for c in (3, 2, 4) if c not in (own, other))
        r.point(fc, own)
        time.sleep(0.2)
        m0 = r.mp()
        r.point(fc, own, trigger=1.0)  # a confirm on the cursor's own cell: its command submenu
        time.sleep(0.05)
        r.point(fc, other, trigger=1.0)  # the dot goes straight on before the submenu's flag can be seen
        time.sleep(0.15)
        r.point(fc, other, trigger=0.0)
        check(wait_until(lambda: r.p.read8(SUBMENU) == 1, 1.0) is not None,
              "the command submenu's flag (0x720BA0) did not rise after the confirm")
        m1 = r.mp()
        check(m1["confirms_passed"] == m0["confirms_passed"] + 1, "the confirm on the cursor's own cell did not pass")
        check(m1["suspended"] and m1["hovered"] < 0, "steering is not suspended with the submenu open (suspended %s, "
              "hovered %d)" % (m1["suspended"], m1["hovered"]))
        r.shot("submenu")
        for cell in (other, third):
            r.point(fc, cell)
            time.sleep(0.6)
        m2 = r.mp()
        check(m2["presses"] == m0["presses"], "with the submenu open the pointer pressed the D-pad %d time(s)" % (
            m2["presses"] - m0["presses"]))
        check(r.cell() == own, "the cursor left %s with the submenu open (cursor %s)" % (CELLS[own][0], r.cursor()))
        r.press_b(fc)  # Cross backs out of the submenu; the dot stays on the third cell
        check(wait_until(lambda: r.p.read8(SUBMENU) == 0, 1.0) is not None, "B did not close the submenu")
        time.sleep(1.0)
        m3 = r.mp()
        check(m3["presses"] == m2["presses"] and r.cell() == own,
              "closing the submenu steered toward the dot (%d press(es), cursor on cell %d)" % (
                  m3["presses"] - m2["presses"], r.cell()))
        took, presses, _ = r.steer_to(fc, other)
        check(took is not None, "the dot moving to another cell did not steer again (cursor %s, presses %d)" % (
            r.cursor(), presses))
        r.close_items(fc)
    return ("a confirm on %s opened its submenu; nothing pressed while it was up; after B the cursor stayed until the "
            "dot moved, then went to %s in %.2f s" % (CELLS[own][0], CELLS[other][0], took))


def sc_override(r):
    with r.controller() as fc:
        r.wait_armed()
        r.open_items(fc)
        check(r.cell() == 0, "the item screen opened on cell %d, expected 0" % r.cell())
        m0 = r.mp()
        r.point(fc, 4)  # the equipped box: two presses away
        check(wait_until(lambda: r.mp()["presses"] > m0["presses"], 0.6, 0.003) is not None,
              "no steering toward the equipped box")
        r.point(fc, 4, stick=(1.0, 0.0))  # push the right stick right: the menu stick takes over
        time.sleep(0.15)
        r.point(fc, 4)
        m1 = r.mp()
        check(m1["aborts"] == m0["aborts"] + 1, "the right stick did not stop the steer (aborts +%d)" % (
            m1["aborts"] - m0["aborts"]))
        time.sleep(1.0)
        m2 = r.mp()
        check(m2["presses"] == m1["presses"], "%d press(es) in the second after the override" % (
            m2["presses"] - m1["presses"]))
        r.shot("override")
        r.close_items(fc)
    return "the right stick stopped the steer; nothing pressed for 1 s after"


def sc_close_in_flight(r):
    with r.controller() as fc:
        r.wait_armed()
        r.open_items(fc)
        before = r.position()
        m0 = r.mp()
        r.point(fc, 4)
        check(wait_until(lambda: r.mp()["presses"] > m0["presses"], 0.6, 0.003) is not None,
              "no steering toward the equipped box")
        fc.set_hand(LEFT, grip=pose(pos=BELT), head_relative=True, squeeze=1.0)  # close the screen at once
        t0 = time.time()
        closed_at = None
        leaked = 0
        released = False
        while time.time() - t0 < 2.0:
            menu = r.p.read8(MENU_OPEN)
            m = r.mp()
            if not menu:
                closed_at = closed_at or time.time()
                if m["dpad"] is not None:
                    leaked += 1
            if not released and time.time() - t0 > 0.4:
                fc.set_hand(LEFT, grip=pose(pos=BELT), head_relative=True, squeeze=0.0)
                released = True
            if closed_at and time.time() - closed_at > 0.3:
                break
            time.sleep(0.002)
        fc.clear_hand(LEFT)
        r.point_clear(fc)
        time.sleep(0.5)
        after = r.position()
        check(closed_at is not None, "the item screen did not close on the belt squeeze")
        check(leaked == 0, "the D-pad was still pressed in %d sample(s) after the screen closed" % leaked)
        moved = math.hypot(after[0] - before[0], after[2] - before[2])
        check(moved < 1.0, "the character moved %.2f units after closing mid-steer" % moved)
    return "closed mid-steer: no D-pad after, moved %.2f units" % moved


def stays_off(r, fc, name):
    """With a menu other than the item screen up (the map or pause): points at every cell for 0.6 s each; point-to-select
    must never run (gate down), nothing is pressed, and the item cursor struct does not change. Returns a detail."""
    if wait_until(lambda: r.mp()["menu_mode"], 2.0) is None:
        raise Fail("menu mode did not start with the %s up (first person did not stand down): nothing was tested" % name)
    time.sleep(ARM_DELAY_S)
    m0 = r.mp()
    cur0 = r.cursor()
    grid = r.p.read8(OWN_GRID)
    gate_polls = 0
    dpad_polls = 0
    for cell in range(len(CELLS)):
        r.point(fc, cell)
        t0 = time.time()
        while time.time() - t0 < 0.6:
            m = r.mp()
            gate_polls += 1 if m["gate"] else 0
            dpad_polls += 1 if m["dpad"] is not None else 0
            time.sleep(0.01)
    r.point_clear(fc)
    time.sleep(0.2)
    m1 = r.mp()
    check(gate_polls == 0, "point-to-select ran with the %s up in %d sample(s) (0x723F94 = %d, 0x720B80 = %d, cursor %s): "
          "the gate must stay down off the item screen" % (name, gate_polls, grid, r.p.read32(ITEM_UP), cur0))
    check(m1["presses"] == m0["presses"] and dpad_polls == 0, "%d D-pad press(es) (%d sample(s) pressed) with the %s up" % (
        m1["presses"] - m0["presses"], dpad_polls, name))
    check(r.cursor() == cur0, "the item cursor struct changed with the %s up (%s -> %s)" % (name, cur0, r.cursor()))
    return "%s: gate down, nothing pressed (0x723F94 = %d)" % (name, grid)


def sc_map_after_items(r):
    """The item screen's cursor struct still reads 1 (0x720B80) after the screen closes and still matches the cell it was
    left on, and the menu flag (0x48BF7E) is up for the map and pause too, so the when list alone would run
    point-to-select there and press the D-pad. The profile's liveWhen (0x723F94, the cursor in the own grid) must keep it
    off. Leaves the cursor on cell 1, closes the item screen, then opens the map (Y = Triangle) and pause (Select)."""
    with r.controller() as fc:
        r.wait_armed()
        r.open_items(fc)
        took, _, _ = r.steer_to(fc, 1)
        check(took is not None, "the cursor did not reach cell 1 (cursor %s)" % (r.cursor(),))
        r.close_items(fc)
        details = ["after closing: 0x720B80 = %d, 0x723F94 = %d, cursor on cell %d" % (
            r.p.read32(ITEM_UP), r.p.read8(OWN_GRID), r.cell())]
        r.wait_armed()
        fc.set_hand(RIGHT, buttons=("y",))  # Y: Triangle, the map
        time.sleep(0.15)
        fc.set_hand(RIGHT)
        check(wait_until(r.menu_open, 3.0) is not None, "the map did not open on Y (Triangle)")
        details.append(stays_off(r, fc, "map"))
        r.press_b(fc)
        check(wait_until(lambda: not r.menu_open(), 3.0) is not None, "the map did not close on B (Cross)")
        time.sleep(0.5)
        # Pause: Select is not on the VR controllers (it does not pause online), so it goes in from the keyboard.
        r.wait_armed()
        try:
            import drive
            drive.press("select")
        except Exception as e:
            raise Skip("; ".join(details) + "; pause NOT checked: no keyboard input (%s)" % e)
        if wait_until(r.menu_open, 3.0) is None:
            raise Skip("; ".join(details) + "; pause NOT checked: Select (Backspace) did not open it (Pad 1 needs the "
                       "keyboard bindings, README)")
        details.append(stays_off(r, fc, "pause"))
        r.press_b(fc)
        check(wait_until(lambda: not r.menu_open(), 3.0) is not None, "pause did not close on B (Cross)")
    return "; ".join(details)


def sc_tablet(r):
    if r.args.test_uv:
        raise Skip("needs the null headset: the tablet is placed from the headset's pose")
    with r.controller() as fc:
        r.wait_armed()
        r.open_items(fc)
        m = r.mp()
        if not m["tablet"]:
            r.close_items(fc)
            raise Skip("the menu is on the world screen: start the emulator with [VR] MenuTablet = true (close it, "
                       "python menutest.py --tablet-ini on; --tablet-ini off restores the ini byte for byte)")
        check(abs(m["half_height"] - 0.5 * TABLET_HEIGHT) < 0.005, "tablet half height %.3f m, expected %.3f" % (
            m["half_height"], 0.5 * TABLET_HEIGHT))
        normal = rotate(m["quat"], (0.0, 0.0, 1.0))
        tilt = math.degrees(math.asin(max(-1.0, min(1.0, normal[1]))))
        want = math.degrees(math.atan2(TABLET_DROP, TABLET_DISTANCE))
        check(abs(tilt - want) < 2.0, "the tablet faces %.1f deg up, expected %.1f (to the eyes 0.30 m above, 0.70 m "
              "back)" % (tilt, want))
        worst = grid_hits(r, fc)
        r.aim(fc, RIGHT, 0.6, 0.45)
        time.sleep(0.4)
        r.shot("tablet")
        r.close_items(fc)
    return "tablet %.2f m tall, facing %.1f deg up; 9 grid points within %.4f" % (2 * m["half_height"], tilt, worst)


# --- belt pouches (either File) -------------------------------------------------------------------------------------

def sc_pouches(r):
    with r.controller() as fc:
        r.wait_armed()
        mapped = r.mapped_pouches(r.pouch_state())
        check(len(mapped) >= 2, "only %d pouch(es) map to cells this runner reads (%s)" % (len(mapped), mapped))
        r.g_open_items(fc)
        cur = r.g_cell()
        check(cur >= 0, "the item screen opened on no mapped cell (cursor %s)" % (r.g_cursor(),))
        items = r.g_items()
        # Every pouch once, the cursor moving each time: the cell it opened on last.
        order = [pc for pc in mapped if pc[1] != cur] + [pc for pc in mapped if pc[1] == cur]
        details = []
        for k, cell in order:
            b0 = r.p.pouches()
            m0 = r.mp()
            r.pouch_hand(fc, LEFT, k, 0.0)  # reach in with an open hand
            time.sleep(0.25)
            b1 = r.p.pouches()
            check(b1["hover"][LEFT] == k, "the left hand at pouch %d's centre is in pouch %d" % (k, b1["hover"][LEFT]))
            check(b1["reaches"] == b0["reaches"] + 1, "reaching into pouch %d gave %d tick(s), expected 1" % (
                k, b1["reaches"] - b0["reaches"]))
            held = cell < 4 and items[cell] != 0
            item = b1["pouches"][k]["item"]
            if item >= 0:
                check(item == (1 if held else 0), "pouch %d reads item %d, but the item byte of cell %d is %d" % (
                    k, item, cell, items[cell] if cell < 4 else -1))
                check(b1["item_ticks"] - b0["item_ticks"] == (1 if held else 0),
                      "pouch %d gave %d double tick(s) for a cell that %s" % (
                          k, b1["item_ticks"] - b0["item_ticks"], "holds an item" if held else "is empty"))
            r.pouch_hand(fc, LEFT, k, 1.0)  # squeeze
            took = wait_until(lambda: r.g_cell() == cell, 1.0, 0.005)
            r.pouch_hand(fc, LEFT, k, 0.0)
            time.sleep(0.2)
            m1 = r.mp()
            b2 = r.p.pouches()
            check(took is not None, "pouch %d: the cursor did not reach cell %d within 1 s (cursor %s; requests sent +%d, "
                  "taken +%d, refused +%d)" % (k, cell, r.g_cursor(), b2["requests"] - b0["requests"],
                                              b2["mp_started"] - b0["mp_started"], b2["mp_refused"] - b0["mp_refused"]))
            check(m1["confirms_passed"] == m0["confirms_passed"], "a confirm reached the game after squeezing pouch %d" % k)
            check(r.g_submenu() == 0, "a command submenu opened after squeezing pouch %d" % k)
            check(r.g_menu_open() and r.g_item_up(), "the item screen is not up any more after squeezing pouch %d" % k)
            check(b2["opens"] == b0["opens"], "squeezing pouch %d with the item screen up pressed the menu button" % k)
            details.append("pouch %d -> cell %d %.2f s%s" % (k, cell, took, " (item)" if held else ""))
            fc.clear_hand(LEFT)
            time.sleep(0.3)
        r.shot("pouches")
        r.g_close_items(fc)
    return "; ".join(details)


def sc_pouch_open(r):
    with r.controller() as fc:
        r.wait_armed()
        check(not r.g_menu_open(), "a menu is already open (load a calm savestate with --slot)")
        mapped = [pc for pc in r.mapped_pouches(r.pouch_state()) if pc[1] != 0]  # the screen opens on cell 0
        check(mapped, "no pouch maps to a cell other than cell 0")
        k, cell = next((pc for pc in mapped if pc[1] == 3), mapped[-1])
        b0 = r.p.pouches()
        m0 = r.mp()
        r.pouch_hand(fc, RIGHT, k, 0.0)
        time.sleep(0.25)
        t0 = time.time()
        r.pouch_hand(fc, RIGHT, k, 1.0)
        up = wait_until(lambda: r.g_menu_open() and r.g_item_up(), 3.0, 0.005)
        t_up = time.time()
        r.pouch_hand(fc, RIGHT, k, 0.0)
        check(up is not None, "the item screen did not open on squeezing pouch %d (open presses +%d, menu byte %d)" % (
            k, r.p.pouches()["opens"] - b0["opens"], r.p.read8(r.game["menu_open"])))
        took = wait_until(lambda: r.g_cell() == cell, 1.0, 0.005)
        time.sleep(0.2)
        m1 = r.mp()
        b1 = r.p.pouches()
        check(took is not None, "the cursor did not reach cell %d within 1 s of the item screen opening (cursor %s; "
              "request taken +%d, refused +%d, lapsed +%d)" % (cell, r.g_cursor(), b1["mp_started"] - b0["mp_started"],
                                                             b1["mp_refused"] - b0["mp_refused"],
                                                             b1["mp_lapsed"] - b0["mp_lapsed"]))
        check(b1["opens"] == b0["opens"] + 1 and b1["requests"] == b0["requests"] + 1,
              "the squeeze sent %d open press(es) and %d request(s), expected one of each" % (
                  b1["opens"] - b0["opens"], b1["requests"] - b0["requests"]))
        check(m1["confirms_passed"] == m0["confirms_passed"], "a confirm reached the game")
        check(r.g_submenu() == 0, "a command submenu opened")
        r.shot("pouch-open")
        fc.clear_hand(RIGHT)
        r.g_close_items(fc)
    return "pouch %d opened the item screen %.2f s after the squeeze; the cursor was on cell %d %.2f s after it came up" % (
        k, t_up - t0, cell, took)


def sc_pouch_submenu(r):
    with r.controller() as fc:
        r.wait_armed()
        mapped = r.mapped_pouches(r.pouch_state())
        r.g_open_items(fc)
        items = r.g_items()
        # A cell whose confirm opens a command submenu: the cursor's own if it holds an item, else the first that does
        # (brought there with its pouch), else File #1's equipped box (the special item).
        cur = r.g_cell()
        with_item = [c for c in range(4) if items[c] != 0]
        target = cur if cur in with_item else (with_item[0] if with_item else (4 if len(r.game["cells"]) > 4 else cur))
        if target != cur:
            k = next((pk for pk, pc in mapped if pc == target), None)
            check(k is not None, "no pouch for cell %d, which holds an item" % target)
            r.squeeze_pouch(fc, LEFT, k)
            check(wait_until(lambda: r.g_cell() == target, 1.0, 0.005) is not None,
                  "pouch %d did not bring the cursor to cell %d" % (k, target))
            fc.clear_hand(LEFT)
            time.sleep(0.3)
        m0 = r.mp()
        # The only confirm: one trigger pull on the cursor's own cell (the right hand aims at nothing), which opens that
        # cell's command submenu; B backs out of it below.
        fc.set_hand(RIGHT, trigger=1.0)
        time.sleep(0.2)
        fc.set_hand(RIGHT, trigger=0.0)
        time.sleep(0.5)
        check(r.mp()["confirms_passed"] == m0["confirms_passed"] + 1, "the trigger on cell %d did not pass" % target)
        flag = r.g_submenu()
        r.shot("submenu")
        other = next((pc for pc in mapped if pc[1] != target), None)
        check(other is not None, "no pouch for a cell other than %d" % target)
        b0 = r.p.pouches()
        m1 = r.mp()
        r.squeeze_pouch(fc, LEFT, other[0])
        time.sleep(1.0)
        b1 = r.p.pouches()
        m2 = r.mp()
        refused = (b1["refused"] - b0["refused"]) + (b1["mp_refused"] - b0["mp_refused"])
        check(m2["presses"] == m1["presses"], "with the submenu open the squeeze pressed the D-pad %d time(s)" % (
            m2["presses"] - m1["presses"]))
        check(r.g_cell() == target, "the cursor left cell %d with the submenu open (cursor %s)" % (target, r.g_cursor()))
        check(refused >= 1, "the squeeze was not refused (pouch side +%d, menu pointer +%d; requests taken +%d)" % (
            b1["refused"] - b0["refused"], b1["mp_refused"] - b0["mp_refused"], b1["mp_started"] - b0["mp_started"]))
        fc.clear_hand(LEFT)
        r.press_b(fc)  # back out of the command submenu with Cross
        r.shot("after-b")
        r.g_close_items(fc)
    return "a confirm on cell %d opened its submenu (flag %d); pouch %d's squeeze was refused, nothing pressed for 1 s; " \
           "B backed out" % (target, flag, other[0])


# --- rects: the white highlight box in a capture of the emulator window -----------------------------------------------

def read_png(path):
    """(width, height, bytes per pixel, rows) of an 8-bit RGB or RGBA PNG."""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, idat, width, height, ctype = 8, b"", 0, 0, 2
    while pos < len(data):
        n = struct.unpack(">I", data[pos:pos + 4])[0]
        kind, body = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if kind == b"IHDR":
            width, height, depth, ctype = struct.unpack(">IIBB", body[:10])
            if depth != 8 or ctype not in (2, 6):
                raise ValueError("only 8-bit RGB/RGBA")
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
    raw = zlib.decompress(idat)
    bpp = 3 if ctype == 2 else 4
    stride = width * bpp
    rows, prev, i = [], bytearray(stride), 0
    for _ in range(height):
        f, line = raw[i], bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        if f == 0:  # what drive.py writes: nothing to undo
            rows.append(bytes(line))
            prev = line
            continue
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1:
                line[x] = (line[x] + a) & 0xFF
            elif f == 2:
                line[x] = (line[x] + b) & 0xFF
            elif f == 3:
                line[x] = (line[x] + ((a + b) >> 1)) & 0xFF
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 0xFF
        rows.append(bytes(line))
        prev = line
    return width, height, bpp, rows


def find_picture(width, height, bpp, rows):
    """The game picture's box (x0, y0, x1, y1) in a window capture: the non-black area round its middle."""
    def lit(x, y):
        o = x * bpp
        r = rows[y]
        return max(r[o], r[o + 1], r[o + 2]) > 24

    band = range(int(height * 0.4), int(height * 0.6), 3)
    xs = [x for x in range(width) if any(lit(x, y) for y in band)]
    if not xs:
        raise ValueError("no picture in the capture")
    x0, x1 = min(xs), max(xs)
    cols = range(x0 + (x1 - x0) * 3 // 10, x0 + (x1 - x0) * 7 // 10, 3)

    def row_lit(y):
        return any(lit(x, y) for x in cols)

    # Grow from the middle, stepping over short dark runs inside the picture but not the letterbox band.
    def grow(y, step):
        last, gap = y, 0
        while 0 <= y + step < height and gap <= 8:
            y += step
            if row_lit(y):
                last, gap = y, 0
            else:
                gap += 1
        return last

    mid = height // 2
    return x0, grow(mid, -1), x1, grow(mid, 1)


def find_highlight(path):
    """The cursor's white highlight box in a capture: (u, v) of its centre on the game picture, and the box in pixels;
    None when no box is found. Rows and columns of the grid's area (right side, upper middle) with at least 40
    near-white pixels (scaled to the picture's width) make the box."""
    width, height, bpp, rows = read_png(path)
    x0, y0, x1, y1 = find_picture(width, height, bpp, rows)
    pw, ph = x1 - x0 + 1, y1 - y0 + 1
    need = max(12, int(round(40 * pw / 777.0)))
    rx0, rx1 = x0 + int(0.75 * pw), x1
    ry0, ry1 = y0 + int(0.12 * ph), y0 + int(0.66 * ph)

    def white(x, y):
        o = x * bpp
        r = rows[y]
        return r[o] >= 200 and r[o + 1] >= 200 and r[o + 2] >= 200

    row_hits = [y for y in range(ry0, ry1 + 1) if sum(1 for x in range(rx0, rx1 + 1) if white(x, y)) >= need]
    col_hits = [x for x in range(rx0, rx1 + 1) if sum(1 for y in range(ry0, ry1 + 1) if white(x, y)) >= need]
    if not row_hits or not col_hits:
        return None
    bx0, bx1, by0, by1 = min(col_hits), max(col_hits), min(row_hits), max(row_hits)
    return ((0.5 * (bx0 + bx1) - x0) / pw, (0.5 * (by0 + by1) - y0) / ph, (bx0, by0, bx1, by1), (x0, y0, x1, y1))


def sc_rects(r):
    import drive
    with r.controller() as fc:
        r.wait_armed()
        r.open_items(fc)
        folder = r.args.report or tempfile.mkdtemp(prefix="menutest-")
        found = []
        cur = r.cell()
        for target in (1, 3, 2, 0, 4):
            took, _, _ = r.steer_to(fc, target, timeout=1.0)
            check(took is not None, "the cursor did not reach %s (cursor %s)" % (CELLS[target][0], r.cursor()))
            time.sleep(0.4)  # the highlight settles
            path = os.path.join(folder, "%s-%s.png" % (r.current, CELLS[target][0].replace(" ", "-")))
            drive.screenshot(path, scale=1)
            if r.args.report:
                r.shots.setdefault(r.current, []).append(os.path.basename(path))
            box = find_highlight(path)
            check(box is not None, "no white highlight box found for %s (%s)" % (CELLS[target][0], path))
            u, v, px, picture = box
            u0, v0, u1, v1 = CELLS[target][5]
            check(u0 - 0.01 <= u <= u1 + 0.01 and v0 - 0.01 <= v <= v1 + 0.01,
                  "%s: the highlight box's centre (%.3f, %.3f) is outside the profile rect %s (box %s px, picture %s px)" % (
                      CELLS[target][0], u, v, CELLS[target][5], px, picture))
            found.append("%s (%.3f, %.3f)" % (CELLS[target][0], u, v))
            cur = target
        r.close_items(fc)
    return "highlight centres inside the rects: " + ", ".join(found)


SCENARIOS = [
    ("pointer-hit", sc_pointer_hit),
    ("cells", sc_cells),
    ("fast-click", sc_fast_click),
    ("latch", sc_latch),
    ("submenu", sc_submenu),
    ("override", sc_override),
    ("close-in-flight", sc_close_in_flight),
    ("map-after-items", sc_map_after_items),
    ("tablet", sc_tablet),
    ("rects", sc_rects),
    ("pouches", sc_pouches),
    ("pouch-open", sc_pouch_open),
    ("pouch-submenu", sc_pouch_submenu),
]
# "all" runs these; rects is run on its own (it captures the emulator window).
ALL = ("pointer-hit", "cells", "fast-click", "latch", "submenu", "override", "close-in-flight", "map-after-items",
       "tablet") + POUCH_SCENARIOS


def tablet_ini(mode, ini=INI):
    """Emulator closed: on backs the ini up (once) and sets [VR] MenuTablet = true; off puts the backup back byte for
    byte."""
    backup = ini + ".menutest-backup"
    if mode == "off":
        if not os.path.exists(backup):
            print("no backup at %s: nothing to restore" % backup)
            return 1
        shutil.copyfile(backup, ini)
        os.remove(backup)
        print("restored %s from %s" % (ini, backup))
        return 0
    if not os.path.exists(backup):
        shutil.copyfile(ini, backup)
    with open(ini, "rb") as fh:
        data = fh.read()
    bom = data.startswith(b"\xef\xbb\xbf")
    text = data.decode("utf-8-sig")
    newline = "\r\n" if "\r\n" in text else "\n"
    lines = text.split(newline)
    out, section, done = [], None, False
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            if section == "VR" and not done:
                out.append("MenuTablet = true")
                done = True
            section = stripped[1:-1]
        elif section == "VR" and stripped.split("=")[0].strip() == "MenuTablet":
            if not done:
                out.append("MenuTablet = true")
                done = True
            continue
        out.append(line)
    if not done:
        if section != "VR":
            out.append("[VR]")
        out.append("MenuTablet = true")
    with open(ini, "wb") as fh:
        fh.write((b"\xef\xbb\xbf" if bom else b"") + newline.join(out).encode("utf-8"))
    print("MenuTablet = true in %s (backup %s; --tablet-ini off restores it)" % (ini, backup))
    return 0


def write_report(folder, results, shots, serial=SERIAL):
    rows = []
    for name, result, detail, secs in results:
        imgs = "".join('<a href="%s"><img src="%s" width="480"></a>' % (html.escape(s), html.escape(s))
                       for s in shots.get(name, []))
        rows.append("<tr class='%s'><td>%s</td><td>%s</td><td><pre>%s</pre></td><td>%.1f s</td><td>%s</td></tr>" % (
            result.lower(), html.escape(name), result, html.escape(detail), secs, imgs))
    page = ("<!doctype html><meta charset='utf-8'><title>menutest %s</title><style>"
            "body{font-family:sans-serif;margin:16px}td{vertical-align:top;padding:4px 8px;border-bottom:1px solid #ccc}"
            "pre{white-space:pre-wrap;margin:0}.pass td:nth-child(2){color:#070}.fail td:nth-child(2),"
            ".error td:nth-child(2){color:#b00}.skip td:nth-child(2){color:#777}</style>"
            "<h1>menutest %s, %s</h1><table>%s</table>" % (serial, serial, time.strftime("%Y-%m-%d %H:%M"), "".join(rows)))
    with open(os.path.join(folder, "report.html"), "w", encoding="utf-8") as f:
        f.write(page)


def main():
    names = [n for n, _ in SCENARIOS]
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("scenarios", nargs="*", help="'all' (" + ", ".join(ALL) + ") or names: " + ", ".join(names))
    ap.add_argument("--list", action="store_true", help="list the scenarios")
    ap.add_argument("--pine-slot", type=int, default=28011)
    ap.add_argument("--slot", type=int, help="savestate slot to load before every scenario (File #1 slot 6, File #2 slot 1 or 2)")
    ap.add_argument("--settle", type=float, default=3.0, help="seconds to wait after loading a state")
    ap.add_argument("--report", help="write report.html and captures into this folder")
    ap.add_argument("--test-uv", action="store_true",
                    help="point with PINE test hits (MsgVRMenuPointer op 1) instead of aiming: no OpenXR runtime needed")
    ap.add_argument("--tablet-ini", choices=("on", "off"),
                    help="emulator closed: set [VR] MenuTablet = true in the ini (backed up), or restore it byte for byte")
    args = ap.parse_args()

    if args.tablet_ini:
        return tablet_ini(args.tablet_ini)
    if args.list or not args.scenarios:
        print("\n".join(names))
        return 0
    wanted = list(ALL) if "all" in args.scenarios else args.scenarios
    unknown = [n for n in wanted if n not in names]
    if unknown:
        print("unknown scenario(s): %s (one of %s)" % (", ".join(unknown), ", ".join(names)))
        return 2
    if args.report:
        os.makedirs(args.report, exist_ok=True)

    try:
        p = wait_for_pine(args.pine_slot, seconds=10)
        require_offline(p)
        serial = p.game_id()
        p.menu_pointer()
    except PineError as e:
        print("PINE: %s (emulator running, EnablePINE on, a build with MsgVRMenuPointer?)" % e)
        return 2
    if serial not in GAMES:
        print("these scenarios are for %s; the emulator is running %r" % (" and ".join(GAMES), serial))
        return 2
    try:
        p.fake_input(head=None, ttl_ms=0)
        if args.test_uv:
            p.set_test_screen_hit(RIGHT, -1.0, 0.0, 0)
    except PineError:
        print("scripted input refused: start the emulator with PCSX2_VR_TEST_INPUT=1, offline, online-safe off.")
        return 2

    runner = Runner(p, args)
    runner.serial = serial
    runner.game = GAMES[serial]
    if args.test_uv:
        runner.test_hit = TestHit(p)
    results = []
    try:
        for name in wanted:
            fn = dict(SCENARIOS)[name]
            runner.current = name
            runner.aiming.clear()
            t0 = time.time()
            try:
                if serial != SERIAL and name not in POUCH_SCENARIOS:
                    raise Skip("File #1 only (on %s only the pouch scenarios run)" % serial)
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
            if runner.test_hit is not None:
                runner.test_hit.clear()
            secs = time.time() - t0
            results.append((name, result, detail, secs))
            print("%-5s %-16s %s" % (result, name, detail.splitlines()[-1] if result == "ERROR" else detail), flush=True)
    finally:
        if runner.test_hit is not None:
            runner.test_hit.stop()

    failed = sum(1 for res in results if res[1] in ("FAIL", "ERROR"))
    print("%d passed, %d failed, %d skipped" % (sum(1 for res in results if res[1] == "PASS"), failed,
                                                sum(1 for res in results if res[1] == "SKIP")))
    if args.report:
        write_report(args.report, results, runner.shots, runner.serial)
    p.close()
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
