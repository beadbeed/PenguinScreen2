"""[VR] CutsceneStereo, headset-free: File #1 savestate slot 7 (J's Bar) plays a real-time cutscene by itself about 20 s
after the load (scenario status 0x48BF60 goes 2 -> 4 -> 2). Offline only, normal speed, no OpenXR runtime, the head
scripted over PINE (PCSX2_VR_TEST_INPUT) and the cutscene eyes forced with PCSX2_VR_CUTSCENE_TEST (no compositor to ask
for them). Nothing is written to the game and nothing is pressed.

    python -I cuttest.py <portable dev install> <out dir> [lean x,y,z metres] [seconds]

It runs that install (never D:\\Games\\PS2\\PenguinScreen2), kills only the emulator it started, and prints the status
timeline (when the cutscene started and ended, the game counter 0x48BF93's rate in and out of it, the camera eye / target
/ FOV per second during it), the "(VR) CutsceneStereo" and ARMED lines of the log, and the stereo dumps it wrote
(PCSX2_VR_STEREODUMP=<out dir>,15; the sidecars carry "cutscene"). Never commit the dumps."""
import json, os, struct, subprocess, sys, time
os.environ["PCSX2_INI"] = os.path.join(sys.argv[1], "inis", "PenguinScreen2.ini")  # pine.py's offline check reads this ini
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pine import FakeController, wait_for_pine, require_offline, PineError

DEV = sys.argv[1]
OUT = sys.argv[2]
LEAN = sys.argv[3] if len(sys.argv) > 3 else "0.05,0,0"
SECONDS = float(sys.argv[4]) if len(sys.argv) > 4 else 150.0
# --xr: SteamVR (its null headset: python null_driver.py on first, off after); the compositor asks for the eyes itself.
XR = "--xr" in sys.argv
STEAMXR = r"E:\ProgramFile\Steam\steamapps\common\SteamVR\steamxr_win64.json"
EXE = os.path.join(DEV, "pcsx2-qt.exe")
PINE_SLOT = 28011
for _l in open(os.path.join(DEV, "inis", "PenguinScreen2.ini"), encoding="utf-8", errors="replace"):
    if _l.strip().startswith("PINESlot"):
        PINE_SLOT = int(_l.split("=")[1])
LOG = os.path.join(DEV, "logs", "emulog.txt")
ISO = r"D:\Games\PS2\games\Biohazard Outbreak File 1 (SLPM-65428) EN-patched.iso"
assert os.path.normcase(os.path.abspath(DEV)) != os.path.normcase(r"D:\Games\PS2\PenguinScreen2")
assert os.path.exists(os.path.join(DEV, "portable.txt"))

STATUS, CUTFLAG, COUNTER = 0x48BF60, 0x48BF88, 0x48BF93
EYE, TARGET, FOV = 0x306338, 0x306344, 0x306380
# The cutscene player (0x5BD690): state struct 0x35FFD0 (frame counter u32 +8), kind u8 0x35FFFF (1: drawn by the minimal
# frame 0x341AF0, no 3D), camera-key flag u8 0x35FF60.
PLAYER, KIND, CAMKEY = 0x35FFD0, 0x35FFFF, 0x35FF60


def log_text():
    try:
        return open(LOG, encoding="utf-8", errors="replace").read()
    except OSError:
        return ""


def main():
    require_offline()
    os.makedirs(OUT, exist_ok=True)
    # SteamVR may be in use (Alex): refuse. Another emulator on the same PINE slot would answer our calls: refuse unless
    # this install's ini gives it a slot of its own ([EmuCore] PINESlot, not the default 28011).
    _tl = subprocess.run(["tasklist"], capture_output=True, text=True).stdout.lower()
    if "vrserver.exe" in _tl:
        sys.exit("refused: vrserver.exe is running")
    if "pcsx2-qt.exe" in _tl and PINE_SLOT == 28011:
        sys.exit("refused: another pcsx2-qt.exe is running on the default PINE slot")
    env = dict(os.environ)
    if XR:
        env["XR_RUNTIME_JSON"] = STEAMXR
        env.pop("PCSX2_VR_CUTSCENE_TEST", None)
    else:
        env["XR_RUNTIME_JSON"] = os.path.join(DEV, "no-openxr-runtime.json")  # missing on purpose: no OpenXR runtime
        env["PCSX2_VR_TEST_INPUT"] = "1"
        env["PCSX2_VR_CUTSCENE_TEST"] = LEAN
    env["PCSX2_VR_STEREODUMP"] = OUT + ",15"
    env.pop("PCSX2_VR_FAKE_HEADPOSE", None)
    proc = subprocess.Popen([EXE, "--vr", "--", ISO], cwd=DEV, env=env)
    try:
        p = wait_for_pine(slot=PINE_SLOT, seconds=120)
        for _ in range(120):
            try:
                if p.game_id() and p.status() == "running":
                    break
            except PineError:
                pass
            time.sleep(1)
        require_offline(p)
        time.sleep(8)
        p.load_state(7)
        time.sleep(1)
        t0 = time.time()
        events, samples = [], []
        last_status, last_count, count_total = None, None, 0
        counts = {}  # status -> (counter ticks, seconds)
        prev_t = time.time()
        cut_start = cut_end = None
        import contextlib
        with (contextlib.nullcontext() if XR else FakeController(p)):
            while time.time() - t0 < SECONDS:
                now = time.time()
                st = p.read8(STATUS)
                c = p.read8(COUNTER)
                if last_count is not None:
                    d = (c - last_count) & 0xFF
                    k = counts.setdefault(st, [0, 0.0])
                    k[0] += d
                    k[1] += now - prev_t
                last_count, prev_t = c, now
                if st != last_status:
                    events.append((round(now - t0, 2), st, p.read8(CUTFLAG)))
                    if st == 4 and cut_start is None:
                        cut_start = now
                    if last_status == 4 and st != 4 and cut_end is None:
                        cut_end = now
                    last_status = st
                if st == 4 and (not samples or now - samples[-1][0] >= 1.0):
                    eye = tuple(p.readf(EYE + 4 * i) for i in range(3))
                    tgt = tuple(p.readf(TARGET + 4 * i) for i in range(3))
                    samples.append((now, eye, tgt, p.readf(FOV), p.read8(PLAYER), p.read32(PLAYER + 8), p.read8(KIND),
                                    p.read8(CAMKEY)))
                if cut_end and now - cut_end > 12.0:
                    break
                time.sleep(0.03)
    finally:
        proc.kill()  # only the emulator this script started
        try:
            proc.wait(timeout=90)
        except subprocess.TimeoutExpired:
            print("== the emulator did not exit in 90 s (pid %d)" % proc.pid)
    print("== status timeline (s since load, status, 0x48BF88):", events)
    if cut_start:
        print("== cutscene: started %.2f s after the load, %s" % (
            cut_start - t0, ("lasted %.2f s" % (cut_end - cut_start)) if cut_end else "did not end in the run"))
    for st, (ticks, secs) in sorted(counts.items()):
        print("== status %d: counter %.1f counts/s over %.1f s" % (st, ticks / secs if secs else 0, secs))
    for (t, eye, tgt, fov, pst, pframe, kind, camkey) in samples:
        print("   t+%.1f eye (%.0f, %.0f, %.0f) target (%.0f, %.0f, %.0f) fov %.2f player state %d frame %d kind %d camkey %d" % (
            (t - cut_start,) + eye + tgt + (fov, pst, pframe, kind, camkey)))
    text = log_text()
    for l in text.splitlines():
        if (("CutsceneStereo" in l and "draw " not in l and "mapped " not in l and "output " not in l) or "CameraDriver: ARMED" in l
                or "CameraDriver: DISARMED" in l or "TrueStereo: first" in l or "xrEndFrame" in l or "Screen: " in l):
            print("  ", l[:260])
    cut_dumps = 0
    for f in sorted(os.listdir(OUT)):
        if f.endswith(".json"):
            try:
                j = json.load(open(os.path.join(OUT, f)))
            except Exception:
                continue
            if j.get("cutscene"):
                cut_dumps += 1
    print("== dumps with cutscene true: %d (in %s)" % (cut_dumps, OUT))


main()
