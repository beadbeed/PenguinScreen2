"""[VR] CutsceneStereo: reach File #1's real-time cutscene 3 (J's Bar, kind 0) headset-free, in the run-cut dev install only
(PINE slot 28097; savestates are its own: never D:\Games\PS2\PenguinScreen2). research/2026-10-22-cutscene-vr.md, section 7.

    python -I cutwalk.py <src slot> <dst slot> <x> <z> <near cm> [--enter]

From <src> it walks Kevin with the scripted left stick, steering with the scripted head's yaw (nothing is pressed, no menu),
to (x, z) and saves <dst> within <near> cm; --enter then waits for a cutscene and times it. Environment:
CUT_NOWALK=1 (load <src> and only time the cutscene: status 4 time, kinds, distinct camera eyes, the player's frames),
CUT_OUT=<dir> (stereo dumps there), CUT_WAIT=<s>, CUT_COMEON=1 (right-stick flicks), CUT_XR=1 (SteamVR's null headset,
started beforehand), and CUT_FLAG=<n>: a TEST HARNESS that sets event flag n in 0x48AD90 (offline, own savestate), the flag
the partner's area node (type 3, flag 2, character +0xB98 == 1) would set, so the game's own script runs the cutscene."""
import os, sys, time, subprocess, math
DEV = r"D:\dev\PenguinScreen2-build\run-cut"
os.environ["PCSX2_INI"] = os.path.join(DEV, "inis", "PenguinScreen2.ini")
sys.path.insert(0, r"D:\dev\PenguinScreen2\.claude\worktrees\cutscenevr\tools\outbreak-vr")
from pine import FakeController, wait_for_pine, require_offline, PineError, pose
ISO = r"D:\Games\PS2\games\Biohazard Outbreak File 1 (SLPM-65428) EN-patched.iso"
src, dst, tx, tz, near = int(sys.argv[1]), int(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4]), float(sys.argv[5])
enter = "--enter" in sys.argv
out = os.environ.get("CUT_OUT")
tl = subprocess.run(["tasklist"], capture_output=True, text=True).stdout.lower()
if "vrserver.exe" in tl and not os.environ.get("CUT_XR"): sys.exit("vrserver running")
require_offline()
env = dict(os.environ)
env["XR_RUNTIME_JSON"] = os.path.join(DEV, "no-openxr-runtime.json")
env["PCSX2_VR_TEST_INPUT"] = "1"
if os.environ.get("CUT_XR"):
    env["XR_RUNTIME_JSON"] = r"E:\ProgramFile\Steam\steamapps\common\SteamVR\steamxr_win64.json"
    env.pop("PCSX2_VR_TEST_INPUT", None); env.pop("PCSX2_VR_CUTSCENE_TEST", None)
if out:
    os.makedirs(out, exist_ok=True)
    env["PCSX2_VR_STEREODUMP"] = out + ",20"
proc = subprocess.Popen([os.path.join(DEV, "pcsx2-qt.exe"), "--vr", "--", ISO], cwd=DEV, env=env)
def posn(p):
    b = 0x476DD0 + 0x10E0 * p.read8(0x48BF7D)
    return p.readf(b + 0x38), p.readf(b + 0x40)
try:
    p = wait_for_pine(slot=28097, seconds=120)
    for _ in range(120):
        try:
            if p.game_id() and p.status() == "running": break
        except PineError: pass
        time.sleep(1)
    require_offline(p)
    time.sleep(8)
    p.load_state(src)
    time.sleep(2)
    import contextlib
    with (contextlib.nullcontext() if os.environ.get("CUT_XR") else FakeController(p)) as fc:
        time.sleep(0.2)
        if os.environ.get("CUT_NOWALK"):
            t1 = time.time(); start = end = None; kinds = set(); eyes = set(); fr = []
            while time.time() - t1 < 120:
                st = p.read8(0x48BF60)
                if st == 4 and start is None: start = time.time()
                if st == 4:
                    kinds.add(p.read8(0x35FFFF)); eyes.add(tuple(round(p.readf(0x306338 + 4 * i)) for i in range(3)))
                    fr.append(p.read32(0x35FFD8))
                if start and st != 4 and end is None: end = time.time(); print("status back to", st)
                if end and time.time() - end > 6: break
                time.sleep(0.03)
            print("cutscene: started %.2f s after the load, %.2f s, kinds %s, %d distinct eyes, player frames %d..%d" % (
                start - t1, (end or time.time()) - start, sorted(kinds), len(eyes), min(fr), max(fr)) if start else "no cutscene")
            raise SystemExit
        time.sleep(1.3)
        x0, z0 = posn(p)
        fc.set_hand(0, stick=(0.0, 1.0)); time.sleep(0.8); fc.set_hand(0, stick=(0.0, 0.0)); time.sleep(0.5)
        x1, z1 = posn(p)
        a0 = math.atan2(x1 - x0, z1 - z0)
        fc.set_head(pose(yaw_deg=45)); time.sleep(0.6)
        fc.set_hand(0, stick=(0.0, 1.0)); time.sleep(0.8); fc.set_hand(0, stick=(0.0, 0.0)); time.sleep(0.5)
        x2, z2 = posn(p)
        a1 = math.atan2(x2 - x1, z2 - z1)
        d = math.degrees((a1 - a0 + math.pi) % (2 * math.pi) - math.pi)
        s = 1.0 if d > 0 else -1.0
        print("probe: step0 %.0f cm dir %.0f deg; yaw 45 -> dir %.0f deg (sign %+d)" % (math.hypot(x1-x0, z1-z0), math.degrees(a0), math.degrees(a1), s))
        def comeon():
            # The right stick flicked up: the game's own "Come on!" ad-lib (no menu, nothing confirmed).
            fc.set_hand(1, stick=(0.0, 1.0)); time.sleep(0.15); fc.set_hand(1, stick=(0.0, 0.0)); time.sleep(0.3)
        if os.environ.get("CUT_COMEON"): comeon(); time.sleep(1.5)
        if os.environ.get("CUT_FLAG"):
            # TEST HARNESS (own install, offline, own savestate): set the event flag the partner's area node would set,
            # so the game's own script starts its real-time cutscene. Logged.
            fl = int(os.environ["CUT_FLAG"], 0)
            a = 0x48AD90 + 4 * (fl >> 5)
            v = p.read32(a); p.write32(a, v | (1 << (fl & 31)))
            print("test harness: event flag %#x set (word %#x: %#x -> %#x)" % (fl, a, v, p.read32(a)))
        t0 = time.time(); saved = False; stuck = 0; last = (x2, z2)
        while time.time() - t0 < 60:
            st = p.read8(0x48BF60)
            if st != 2:
                print("status", st, "at", posn(p)); break
            x, z = posn(p)
            dist = math.hypot(tx - x, tz - z)
            if dist < near and not saved:
                fc.set_hand(0, stick=(0.0, 0.0)); time.sleep(0.6)
                p.save_state(dst); time.sleep(3); saved = True
                print("saved slot", dst, "at", posn(p), "dist %.0f" % dist)
                if not enter: break
            want = math.atan2(tx - x, tz - z)
            yaw = s * math.degrees(want - a0)
            fc.set_head(pose(yaw_deg=((yaw + 180) % 360) - 180))
            fc.set_hand(0, stick=(0.0, 1.0 if dist > 60 else 0.6))
            time.sleep(0.25)
            if math.hypot(x - last[0], z - last[1]) < 2: stuck += 1
            else: stuck = 0
            last = (x, z)
            if stuck > 12: print("stuck at", (x, z), "dist %.0f" % dist); break
        fc.set_hand(0, stick=(0.0, 0.0))
        if enter:
            # Hands off; follow the cutscene if one starts.
            t1 = time.time(); start = end = None; kinds = set(); eyes = set()
            while time.time() - t1 < 120:
                st = p.read8(0x48BF60)
                if st == 4 and start is None: start = time.time()
                if st == 4:
                    kinds.add(p.read8(0x35FFFF))
                    eyes.add(tuple(round(p.readf(0x306338 + 4 * i)) for i in range(3)))
                if start and st != 4 and end is None: end = time.time(); print("status back to", st)
                if end and time.time() - end > 6: break
                if start is None and os.environ.get("CUT_COMEON") and int(time.time() - t1) % 8 == 0 and time.time() - t1 > 1:
                    comeon(); time.sleep(0.6)
                if start is None and time.time() - t1 > float(os.environ.get("CUT_WAIT", "20")): print("no cutscene; partner", [round(p.readf(0x476DD0 + 0x10E0 + 0x38)), round(p.readf(0x476DD0 + 0x10E0 + 0x40))], "flags", p.read_block(0x48AD90, 16).hex()); break
                time.sleep(0.03)
            if start:
                print("cutscene: %.2f s, kinds %s, %d distinct eyes" % (((end or time.time()) - start), sorted(kinds), len(eyes)))
finally:
    proc.kill(); proc.wait(timeout=90)
