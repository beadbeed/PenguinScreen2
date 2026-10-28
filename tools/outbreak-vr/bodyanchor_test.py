"""Your own arms in first person ([VR] VrBody + VrBodySelf, VrBodySelfStyle), the offline measurement: one headset-free
emulator (no OpenXR runtime, PINE scripted head and hands, offline, normal speed), File #1. For each seated eye height
(the scripted head at that height, the view recentred on it) and a few seated hand poses it reads, through PINE only:

- the game's camera eye (camera.lookAt.eye 0x306338) and Kevin's bones' world matrices (the local record's model);
- the "(VR) Body: self ..." log lines (PCSX2_VR_BODY_LOG=1): per arm the target, the solve and the bones in memory.

and prints, in the view's frame (cm: up, forward, right from the eye): Kevin's head 21, neck 20 and chest 11 joints, the
drawn shoulder joints and how far the drawn upper arm (shoulder to elbow, a 6 cm sleeve round it) comes to the eye; then
for the anchored styles the wrist the cave drew against the controller (the target as the eye sees it).

    python -I bodyanchor_test.py <portable dev install> [slot] [--style Forearms|Arms|GameShoulders] [--heights 1.1,1.2,1.3]
                                  [--slot5-zombie] [--shots <folder>]

It sets [VR] VrBody, VrBodySelf, VrBodySelfStyle (VrPresence off) in THAT install's ini (never D:\\Games\\PS2\\PenguinScreen2),
uses that ini's PINE slot, refuses to start while vrserver.exe runs (Alex may be playing), presses nothing, writes no game
memory and kills only the emulator it started (by its own handle). Screenshots (--shots) stay on this PC; never commit them."""
import math, os, re, struct, subprocess, sys, time

DEV = sys.argv[1]
os.environ["PCSX2_INI"] = os.path.join(DEV, "inis", "PenguinScreen2.ini")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pine import FakeController, wait_for_pine, require_offline, PineError, pose

ARGS = sys.argv[2:]


def opt(name, default=None):
    return ARGS[ARGS.index(name) + 1] if name in ARGS else default


STYLE = opt("--style", "Forearms")
HEIGHTS = [float(h) for h in opt("--heights", "1.1,1.2,1.3").split(",")]
SHOTS = opt("--shots")
skip = {opt("--style"), opt("--heights"), SHOTS}
POS = [a for a in ARGS if not a.startswith("--") and a not in skip]
STATE_SLOT = int(POS[0]) if POS else 7
EXE = os.path.join(DEV, "pcsx2-qt.exe")
LOG = os.path.join(DEV, "logs", "emulog.txt")
ISO = r"D:\Games\PS2\games\Biohazard Outbreak File 1 (SLPM-65428) EN-patched.iso"
assert os.path.normcase(os.path.abspath(DEV)) != os.path.normcase(r"D:\Games\PS2\PenguinScreen2")
assert os.path.exists(os.path.join(DEV, "portable.txt"))

EYE = 0x306338
REC = 0x476DD0  # record 0, the local character in File #1's test saves (checked: its model has 36 bones)
BONES = [0, 2, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 28, 32]


def running(name):
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq %s" % name, "/NH"], capture_output=True, text=True).stdout
    return name.lower() in out.lower()


def ini_value(path, section, key):
    sec = None
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"^\[(.+)\]\s*$", line)
        if m:
            sec = m.group(1)
        elif sec == section and line.split("=", 1)[0].strip() == key:
            return line.split("=", 1)[1].strip()
    return None


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
    open(path, "w", encoding="utf-8", newline="\n").write("\n".join(out) + "\n")


def log_text():
    return open(LOG, encoding="utf-8", errors="replace").read()


def vec(p, a):
    return tuple(struct.unpack("<f", struct.pack("<I", p.read32(a + 4 * k)))[0] for k in range(3))


def bones(p):
    model = p.read32(REC + 0x34)
    n = p.read16(model + 0x816)
    out = {}
    for b in BONES:
        if b < n:
            s = p.read32(model + 0x820 + 4 * b)
            m = struct.unpack("<16f", p.read_block(s, 64))
            out[b] = m
    return n, out


def sub(a, b):
    return tuple(x - y for x, y in zip(a, b))


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def norm(a):
    l = math.sqrt(dot(a, a))
    return tuple(x / l for x in a) if l > 1e-9 else a


def seg_dist(p, a, b):
    ab = sub(b, a)
    t = max(0.0, min(1.0, dot(sub(p, a), ab) / max(dot(ab, ab), 1e-9)))
    q = tuple(a[i] + ab[i] * t for i in range(3))
    return math.sqrt(dot(sub(p, q), sub(p, q))), q


def view_frame(p):
    eye = vec(p, EYE)
    tgt = vec(p, EYE + 12)
    f = sub(tgt, eye)
    fh = norm((f[0], 0.0, f[2]))
    right = (-fh[2], 0.0, fh[0])  # up x forward is the left (the model's +X side): the right is its opposite
    return eye, fh, right


def in_view(eye, fh, right, w):
    d = sub(w, eye)
    return (d[1], dot(d, fh), dot(d, right))


ARM = re.compile(r"arm(\d) \(wrist bone (\d+)\) weight ([\d.]+): target \(([-\d.]+), ([-\d.]+), ([-\d.]+)\), solved \(([-\d.]+), "
                 r"([-\d.]+), ([-\d.]+)\) ([\d.]+) cm off( \(out of reach\))?, in memory \(([-\d.]+), ([-\d.]+), ([-\d.]+)\) ([\d.]+) cm off")


def main():
    if running("vrserver.exe"):
        sys.exit("vrserver.exe is running (Alex may be playing): not launching.")
    require_offline()
    slot = int(ini_value(os.environ["PCSX2_INI"], "EmuCore", "PINESlot") or 28011)
    set_ini(os.environ["PCSX2_INI"], "VR", {"VrPresence": "false", "VrBody": "true", "VrBodySelf": "true", "VrBodySelfStyle": STYLE})
    env = dict(os.environ)
    env["XR_RUNTIME_JSON"] = os.path.join(DEV, "no-openxr-runtime.json")  # missing on purpose: no OpenXR runtime
    env["PCSX2_VR_TEST_INPUT"] = "1"
    env["PCSX2_VR_BODY_LOG"] = "1"
    env.pop("PCSX2_VR_FAKE_HEADPOSE", None)
    print("tasklist pcsx2-qt before launch:", running("pcsx2-qt.exe"), "| PINE slot", slot, "| style", STYLE, "| slot", STATE_SLOT)
    proc = subprocess.Popen([EXE, "--vr", "--", ISO], cwd=DEV, env=env)
    print("started PID", proc.pid)
    results = []
    try:
        p = wait_for_pine(slot=slot, seconds=120)
        for _ in range(120):
            try:
                if p.game_id() and p.status() == "running":
                    break
            except PineError:
                pass
            time.sleep(1)
        require_offline(p)
        time.sleep(8)
        shot = None
        if SHOTS:
            import drive
            os.makedirs(SHOTS, exist_ok=True)
            shot = lambda name: drive.screenshot(os.path.join(SHOTS, name + ".png"))
        # Seated hand poses relative to the head (metres, OpenXR: x right, y up, -z forward), controllers pointing ahead.
        poses = [
            ("aim forward", 0.0, (-0.20, -0.32, -0.38), (0.22, -0.30, -0.42)),
            ("low, near the lap", 0.0, (-0.18, -0.50, -0.25), (0.18, -0.50, -0.25)),
            ("reach far forward", 0.0, (-0.18, -0.20, -0.58), (0.18, -0.20, -0.58)),
            ("right raised, left low", 0.0, (-0.22, -0.45, -0.30), (0.25, 0.10, -0.30)),
            ("out to the sides", 0.0, (-0.55, -0.28, -0.10), (0.55, -0.28, -0.10)),
            ("aim forward, looking down 50", -50.0, (-0.20, -0.32, -0.38), (0.22, -0.30, -0.42)),
        ]
        if "--quick" in ARGS:
            poses = poses[:1] + poses[-1:]
        for h in HEIGHTS:
            # Each height from a fresh load (the slots' own cuts start some 20 s in and take first person away).
            p.load_state(STATE_SLOT)
            time.sleep(3)
            with FakeController(p, head=pose(pos=(0.0, h, 0.0)), recenter=True) as fc:
                time.sleep(1.5)
                for name, pitch, lh, rh in poses:
                    mark = len(log_text())
                    fc.set_head(pose(pitch_deg=pitch, pos=(0.0, h, 0.0)))
                    L = tuple(a + b for a, b in zip(lh, (0.0, h, 0.0)))
                    R = tuple(a + b for a, b in zip(rh, (0.0, h, 0.0)))
                    fc.set_hand(0, grip=pose(pos=L), aim=pose(pos=L), squeeze=0.3)
                    fc.set_hand(1, grip=pose(pos=R), aim=pose(pos=R), squeeze=0.3)
                    time.sleep(2.2)
                    eye, fh, right = view_frame(p)
                    n, bs = bones(p)
                    J = {b: (m[12], m[13], m[14]) for b, m in bs.items()}
                    # The drawn upper arm: shoulder joint 13 / 17 to elbow 14 / 18 (a 6 cm sleeve round the bone).
                    arm = {}
                    for side, (s, e) in (("L", (13, 14)), ("R", (17, 18))):
                        d, q = seg_dist(eye, J[s], J[e])
                        arm[side] = dict(shoulder=in_view(eye, fh, right, J[s]), elbow=in_view(eye, fh, right, J[e]),
                                         eye_to_upper_arm_cm=d - 6.0, upper_arm_top_below_eye_cm=eye[1] - max(J[s][1], J[e][1]) - 6.0)
                    lines = [l for l in log_text()[mark:].splitlines() if "(VR) Body: self record" in l]
                    wr = [dict(arm=int(m.group(1)), weight=float(m.group(3)), solve_cm=float(m.group(10)), reach=bool(m.group(11)),
                               memory_cm=float(m.group(15))) for m in ARM.finditer(lines[-1])] if lines else []
                    extra = re.findall(r"\[anchor[^\]]*\]", lines[-1]) if lines else []
                    r = dict(height=h, pose=name, bones=n, eye_y_over_char=None,
                             head21=in_view(eye, fh, right, J[21]), neck20=in_view(eye, fh, right, J[20]),
                             chest11=in_view(eye, fh, right, J[11]), arm=arm, wrists=wr, anchor=extra)
                    try:
                        cy = vec(p, REC + 0x38)[1]
                        r["eye_y_over_char"] = eye[1] - cy
                    except PineError:
                        pass
                    results.append(r)
                    if shot:
                        shot("anchor_%s_%.1f_%s" % (STYLE, h, name.split(",")[0].replace(" ", "_")))
                    print(fmt(r), flush=True)
    finally:
        try:
            proc.kill()  # only the emulator this script started
        except OSError:
            pass
        proc.wait(timeout=30)
        print("stopped PID", proc.pid)


def fmt(r):
    f = lambda v: "(%+.1f up, %+.1f fwd, %+.1f right)" % v
    s = ["== eye %.1f m, %s (%d bones), eye %.1f cm over the character" % (r["height"], r["pose"], r["bones"], r["eye_y_over_char"] or 0)]
    s.append("   from the eye: head21 %s neck20 %s chest11 %s" % (f(r["head21"]), f(r["neck20"]), f(r["chest11"])))
    for side, a in r["arm"].items():
        s.append("   %s shoulder %s elbow %s | sleeve to the eye %.1f cm, sleeve top %.1f cm below the eye" % (
            side, f(a["shoulder"]), f(a["elbow"]), a["eye_to_upper_arm_cm"], a["upper_arm_top_below_eye_cm"]))
    for w in r["wrists"]:
        s.append("   arm%d weight %.2f: solve %.1f cm off%s, in memory %.1f cm off" % (
            w["arm"], w["weight"], w["solve_cm"], " (out of reach)" if w["reach"] else "", w["memory_cm"]))
    for a in r["anchor"]:
        s.append("   " + a)
    return "\n".join(s)


main()
