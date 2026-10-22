"""VR body ([VR] VrBody, VrBodySelf), the offline check: one headset-free emulator (no OpenXR runtime, PINE scripted
head and hands, offline, normal speed) with VR presence's loopback (presence_echo.Echo sends this console's own poses
back as AI partner 1), so the partner's character (record 1) is posed from scripted VR poses. Reads the emulog's
"(VR) Body:" lines (PCSX2_VR_BODY_LOG=1): per arm the wrist target, the solve and the wrist bone the GAME built (its world
matrix in memory), and the head bone's forward against the VR head's. With --self, VrBodySelf too: your own character's
arms. Nothing pressed, no game memory written by this script (PINE reads only); kills only the emulator it started.

    python -I body_test.py <portable dev install> [slot] [--self] [--shots <folder>]

It sets [VR] VrPresence, VrPresencePeer = 127.0.0.1:47801, VrPresencePort = 47800, VrBody (and VrBodySelf) in THAT
install's ini (never D:\\Games\\PS2\\PenguinScreen2) and refuses to start while another pcsx2-qt.exe or vrserver.exe runs.
Screenshots (--shots) are of the emulator window, for inspection on this PC only; never commit them."""
import math, os, re, subprocess, sys, time
DEV = sys.argv[1]
os.environ["PCSX2_INI"] = os.path.join(DEV, "inis", "PenguinScreen2.ini")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pine import FakeController, wait_for_pine, require_offline, PineError, pose
from presence_echo import Echo

SELF = "--self" in sys.argv
SHOTS = sys.argv[sys.argv.index("--shots") + 1] if "--shots" in sys.argv else None
args = [a for a in sys.argv[2:] if not a.startswith("--") and a != SHOTS]
# --sweep: only the first pose, then a quick look-round for the screenshots
STATE_SLOT = int(args[0]) if args else 6
EXE = os.path.join(DEV, "pcsx2-qt.exe")
LOG = os.path.join(DEV, "logs", "emulog.txt")
ISO = r"D:\Games\PS2\games\Biohazard Outbreak File 1 (SLPM-65428) EN-patched.iso"
assert os.path.normcase(os.path.abspath(DEV)) != os.path.normcase(r"D:\Games\PS2\PenguinScreen2")
assert os.path.exists(os.path.join(DEV, "portable.txt"))


def running(name):
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq %s" % name, "/NH"], capture_output=True, text=True).stdout
    return name.lower() in out.lower()


def set_ini(path, section, values):
    lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    out, sec, done = [], None, set()
    for line in lines:
        m = re.match(r"^\[(.+)\]\s*$", line)
        if m:
            if sec == section:
                out += ["%s = %s" % (k, v) for k, v in values.items() if k not in done]
                done |= set(values)
            sec = m.group(1)
        elif sec == section:
            k = line.split("=", 1)[0].strip()
            if k in values:
                out.append("%s = %s" % (k, values[k]))
                done.add(k)
                continue
        out.append(line)
    if sec == section:
        out += ["%s = %s" % (k, v) for k, v in values.items() if k not in done]
        done |= set(values)
    if section not in [re.match(r"^\[(.+)\]", l).group(1) for l in lines if re.match(r"^\[(.+)\]", l)]:
        out += ["", "[%s]" % section] + ["%s = %s" % (k, v) for k, v in values.items()]
    open(path, "w", encoding="utf-8", newline="\n").write("\n".join(out) + "\n")


def log():
    return open(LOG, encoding="utf-8", errors="replace").read()


def body_lines(since):
    return [l for l in log()[since:].splitlines() if "(VR) Body" in l]


ARM = re.compile(r"arm(\d) \(wrist bone (\d+)\) weight ([\d.]+): target \(([-\d.]+), ([-\d.]+), ([-\d.]+)\), solved \(([-\d.]+), "
                 r"([-\d.]+), ([-\d.]+)\) ([\d.]+) cm off( \(out of reach\))?, in memory \(([-\d.]+), ([-\d.]+), ([-\d.]+)\) ([\d.]+) cm off")
HEAD = re.compile(r"head forward in memory \(([-\d.]+), ([-\d.]+), ([-\d.]+)\) vs VR \(([-\d.]+), ([-\d.]+), ([-\d.]+)\): ([\d.]+) deg( \(clamped\))?")


def summary(lines, who):
    rows = [l for l in lines if ("(VR) Body: %s record" % who) in l]
    out = []
    for l in rows[-3:]:
        arms = [dict(arm=int(m.group(1)), bone=int(m.group(2)), weight=float(m.group(3)), solve_cm=float(m.group(10)),
                     reach=bool(m.group(11)), memory_cm=float(m.group(15))) for m in ARM.finditer(l)]
        h = HEAD.search(l)
        out.append(dict(arms=arms, head_deg=float(h.group(7)) if h else None, head_clamped=bool(h and h.group(8)),
                        chain=re.search(r"chest chain vs memory ([\d.]+)", l).group(1) if "chest chain" in l else None))
    return out


def main():
    if running("pcsx2-qt.exe") or running("vrserver.exe"):
        sys.exit("another pcsx2-qt.exe or vrserver.exe is running: wait for it (shared PINE slot, SteamVR untouched).")
    require_offline()
    set_ini(os.environ["PCSX2_INI"], "VR",
               {"VrPresence": "true", "VrPresencePeer": "127.0.0.1:47801", "VrPresencePort": "47800", "VrPresenceKey": "",
                "VrBody": "true", "VrBodySelf": "true" if SELF else "false"})
    env = dict(os.environ)
    env["XR_RUNTIME_JSON"] = os.path.join(DEV, "no-openxr-runtime.json")  # missing on purpose: no OpenXR runtime
    env["PCSX2_VR_TEST_INPUT"] = "1"
    env["PCSX2_VR_PRESENCE_LOOPBACK"] = "1"
    env["PCSX2_VR_BODY_LOG"] = "1"
    env.pop("PCSX2_VR_FAKE_HEADPOSE", None)
    echo = Echo(listen=47801, to=47800, slot=1).start()
    proc = subprocess.Popen([EXE, "--vr", "--", ISO], cwd=DEV, env=env)
    out = {}
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
        p.load_state(STATE_SLOT)
        time.sleep(4)
        shot = None
        if SHOTS:
            import drive
            os.makedirs(SHOTS, exist_ok=True)
            shot = lambda name: drive.screenshot(os.path.join(SHOTS, name + ".png"))
        steps = [
            # name, head yaw (deg, left +), left hand, right hand (VR metres relative to the head at the origin)
            ("1 hands forward, head turned 30 left", 30.0, (-0.22, -0.30, -0.42), (0.24, -0.28, -0.45)),
            ("2 right hand raised, head straight", 0.0, (-0.22, -0.40, -0.25), (0.25, 0.20, -0.25)),
            ("3 both out to the sides, head 90 right (clamped)", -90.0, (-0.60, -0.25, -0.05), (0.60, -0.25, -0.05)),
            ("4 hands too far (blend back)", 0.0, (-0.2, -0.3, -2.5), (0.2, -0.3, -2.5)),
        ]
        if "--sweep" in sys.argv:
            steps = steps[:1]  # straight to the look-round (the slot's own cut starts about 25 s after the load)
        with FakeController(p, head=pose(pos=(0.0, 0.0, 0.0))) as fc:
            for name, yaw, lh, rh in steps:
                mark = len(log())
                fc.set_head(pose(yaw_deg=yaw, pos=(0.0, 0.0, 0.0)))
                fc.set_hand(0, grip=pose(pos=lh), aim=pose(pos=lh), squeeze=0.3)
                fc.set_hand(1, grip=pose(pos=rh), aim=pose(pos=rh), squeeze=0.8)
                time.sleep(2 if "--sweep" in sys.argv else 6)
                L = body_lines(mark)
                out[name] = dict(lines=L, partner=summary(L, "partner"), self=summary(L, "self"))
                if shot:
                    shot("body_%s" % name.split()[0])
            # Look round (hands forward) so a shot finds the partner: for the eye only.
            if shot:
                lh, rh = (-0.22, -0.30, -0.42), (0.24, -0.28, -0.45)
                fc.set_hand(0, grip=pose(pos=lh), aim=pose(pos=lh), squeeze=0.3)
                fc.set_hand(1, grip=pose(pos=rh), aim=pose(pos=rh), squeeze=0.8)
                for yaw in range(0, 360, 30):
                    fc.set_head(pose(yaw_deg=float(yaw), pitch_deg=-10.0, pos=(0.0, 0.0, 0.0)))
                    time.sleep(1.2)
                    shot("look_%03d" % yaw)
    finally:
        echo.stop()
        try:
            proc.kill()  # only the emulator this script started
        except OSError:
            pass
        proc.wait(timeout=30)
    for k, v in out.items():
        print("==", k)
        for l in [l for l in v["lines"] if "record" not in l][-4:]:
            print("   ", l[:300])
        for who in ("partner", "self"):
            for s in v[who][-1:]:
                print("   %s:" % who, s)
        for l in [l for l in v["lines"] if "record" in l][-1:]:
            print("    last:", l[:900])
    print("echo:", echo.stats)


main()
