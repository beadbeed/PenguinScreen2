"""Near-cull patch, headset-free (File #1, offline, normal speed, a portable dev install only; reads only).

    python -I nearcull_test.py <dev install> <before|after> <slot> <seconds> [zombie|down]

before: the File #1 profile as committed at git HEAD, put in the dev install's own vrprofiles override folder for the
        run (removed after); after: the profile in this checkout (the dev install's resources folder must be a junction
        to it), no override. The install needs the File #1 savestates in its sstates folder:
slot 5 (zombie): a zombie walks up and grabs Kevin 5-7 s after load; the scripted head (PCSX2_VR_TEST_INPUT, no OpenXR
        runtime) learns how the camera follows it, then faces the nearest live zombie.
slot 6 (down): calm; the head looks down at the body (pitch -70) after 3 s, then behind after 7 s.
Each sample (about 50 a second) records the eye, the nearest zombie's distance by the cull's own measure (eye to its
feet + 100 up), its class and its drawn byte (+0x198, set by the draw pass right after the draw call), the local and
partner records' drawn bytes, the focus slot and the cull's branch word. Screenshots of the emulator window and a JSON
log go to <dev install>/logs/nearcull/. The emulator is the dev install's own, killed by its PID at the end."""
import json, math, os, struct, subprocess, sys, time

DEV = sys.argv[1]
os.environ["PCSX2_INI"] = os.path.join(DEV, "inis", "PenguinScreen2.ini")  # pine.py's offline check reads this ini
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
from pine import FakeController, wait_for_pine, require_offline, PineError, pose
import drive

EXE = os.path.join(DEV, "pcsx2-qt.exe")
ISO = r"D:\Games\PS2\games\Biohazard Outbreak File 1 (SLPM-65428) EN-patched.iso"
OVR = os.path.join(DEV, "vrprofiles", "SLPM-65428.yaml")
assert os.path.exists(os.path.join(DEV, "portable.txt")), "not a portable install"
assert os.path.normcase(os.path.abspath(DEV)) != os.path.normcase(r"D:\Games\PS2\PenguinScreen2"), "never Alex's install"

variant, slot, secs = sys.argv[2], int(sys.argv[3]), float(sys.argv[4])
look = sys.argv[5] if len(sys.argv) > 5 else "zombie"
tag = "%s_s%d_%s" % (variant, slot, look)
OUT = os.path.join(DEV, "logs", "nearcull")
SHOTS = OUT
os.makedirs(OUT, exist_ok=True)

ENEMY_HEAD = 0x241C70 - 0x5E7C
REC = lambda k: 0x476DD0 + k * 0x10E0
f3 = lambda b, o: struct.unpack_from("<3f", b, o)


def enemy_class(p, typ, variant_byte):
    tab = p.read32(0x357500 + typ * 8)
    if not tab or tab >= 0x2000000:
        return -1
    b = p.read8(tab + variant_byte * 16 + 0xC)
    return b - 256 if b > 127 else b


def cull_metric(eye, pos):
    """FUN_005902F0's distance: eye to the model's feet + 100 up."""
    return math.sqrt((pos[0] - eye[0]) ** 2 + (pos[1] + 100.0 - eye[1]) ** 2 + (pos[2] - eye[2]) ** 2)


cls_cache = {}


def sample(p):
    s = {}
    cam = p.read_block(0x306330, 0x20)
    s["eye"] = f3(cam, 8); s["tgt"] = f3(cam, 20)
    s["focus"] = p.read8(0x3AEF74); s["local"] = loc = p.read8(0x48BF7D)
    s["hide"] = list(struct.pack("<I", p.read32(0x48AEA0)))
    s["radii"] = list(struct.unpack("<4f", p.read_block(0x6D6CF0, 16)))
    s["cullword"] = p.read32(0x590304)
    s["near"] = p.readf(0x30637C)
    s["counter"] = p.read8(0x48BF93)
    humans = []
    for k in range(4):
        b = p.read_block(REC(k), 0x1A0)
        if b[0] or b[1]:
            humans.append(dict(k=k, pos=f3(b, 0x38), drawn=b[0x198], stance=b[0x08]))
    s["humans"] = humans
    lb = p.read_block(REC(loc), 0x100)
    s["lpos"] = f3(lb, 0x38); s["stance"] = lb[0x08]
    en = []
    a = p.read32(ENEMY_HEAD)
    n = 0
    while a and a < 0x2000000 and n < 16:
        base = a & ~7
        b = p.read_block(base, 0x1A8 + 0x3B0)[a - base:]
        e = dict(rec=a, live=bool(b[0] and b[1]), type=b[3], var=b[0x54D], pos=f3(b, 0x38), drawn=b[0x198])
        key = (e["type"], e["var"])
        if key not in cls_cache:
            cls_cache[key] = enemy_class(p, *key)
        e["cls"] = cls_cache[key]
        en.append(e)
        a = struct.unpack_from("<I", b, 0x1C)[0]
        n += 1
    s["enemies"] = en
    try:
        t = p.telemetry(); s["armed"] = t["armed"]; s["active"] = t["look_at_active"]; s["reason"] = t["disarm_reason"]
    except PineError:
        s["armed"] = None
    return s


def hd(a, b):
    return math.hypot(a[0] - b[0], a[2] - b[2])


def signed_angle(f, d):
    return math.atan2(f[0] * d[1] - f[1] * d[0], f[0] * d[0] + f[1] * d[1])


def main():
    require_offline()
    if os.path.exists(OVR):
        sys.exit("run-dev already has a File #1 override; not touching it")
    if variant == "before":
        head = subprocess.run(["git", "-C", REPO, "show", "HEAD:bin/resources/vr-profiles/SLPM-65428.yaml"],
                              capture_output=True, check=True).stdout
        open(OVR, "wb").write(head)
    elif variant != "after":
        sys.exit("variant: before or after")
    env = dict(os.environ)
    env["XR_RUNTIME_JSON"] = os.path.join(DEV, "no-openxr-runtime.json")  # missing on purpose: no OpenXR runtime
    env["PCSX2_VR_TEST_INPUT"] = "1"
    env.pop("PCSX2_VR_FAKE_HEADPOSE", None)
    proc = subprocess.Popen([EXE, "--vr", "--", ISO], cwd=DEV, env=env)
    log, shots = [], {}
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
        p.load_state(slot)
        t0 = time.time()
        yaw = 0.0
        cal = None
        grab_t = None
        with FakeController(p, head=pose(0.0, 0.0)) as fc:
            phase = 0
            while time.time() - t0 < secs:
                t = time.time() - t0
                s = sample(p)
                s["t"] = round(t, 3)
                s["head_yaw"] = yaw
                live = [e for e in s["enemies"] if e["live"]]
                near = min(live, key=lambda e: hd(e["pos"], s["lpos"])) if live else None
                if near:
                    s["near_h"] = round(hd(near["pos"], s["eye"]), 1)
                    s["near_metric"] = round(cull_metric(s["eye"], near["pos"]), 1)
                    s["near_cls"] = near["cls"]
                    s["near_drawn"] = near["drawn"]
                    f = (s["tgt"][0] - s["eye"][0], s["tgt"][2] - s["eye"][2])
                    d = (near["pos"][0] - s["eye"][0], near["pos"][2] - s["eye"][2])
                    err = math.degrees(signed_angle(f, d))
                    s["bearing_err"] = round(err, 1)
                    cy = math.degrees(math.atan2(f[0], f[1]))
                    if look == "zombie" and s.get("active"):
                        # learn how the camera yaw follows the scripted head yaw once, then aim straight at the zombie
                        if cal is None:
                            cal = [t, cy, None]
                            yaw = 30.0
                            fc.set_head(pose(yaw, -5.0))
                        elif cal[2] is None and t - cal[0] > 0.6:
                            dy = (cy - cal[1] + 180.0) % 360.0 - 180.0
                            cal[2] = 1.0 if dy > 0 else -1.0
                            cal[1] = cy - cal[2] * yaw   # the camera yaw at head yaw 0
                            print("  calibrated: camera yaw moved %.1f for head +30; sign %+d" % (dy, cal[2]), flush=True)
                        elif cal[2] is not None:
                            want = math.degrees(math.atan2(d[0], d[1]))
                            yaw = cal[2] * ((want - cal[1] + 180.0) % 360.0 - 180.0)
                            fc.set_head(pose(yaw, -5.0))
                if look == "down":
                    if phase == 0 and t > 3.0:
                        fc.set_head(pose(0.0, -70.0)); phase = 1
                    elif phase == 1 and t > 7.0:
                        fc.set_head(pose(180.0, -30.0)); phase = 2
                log.append(s)
                print("t=%5.2f armed=%s act=%s st=%d focus=%d loc=%d hide=%s cull=%08x r=%s near_h=%s metric=%s cls=%s drawn=%s err=%s local_drawn=%s partners=%s" % (
                    t, s.get("armed"), s.get("active"), s["stance"], s["focus"], s["local"], s["hide"], s["cullword"],
                    [round(x) for x in s["radii"]], s.get("near_h"), s.get("near_metric"), s.get("near_cls"), s.get("near_drawn"),
                    s.get("bearing_err"), [h["drawn"] for h in s["humans"] if h["k"] == s["local"]],
                    [(h["k"], h["drawn"], round(hd(h["pos"], s["eye"]))) for h in s["humans"] if h["k"] != s["local"]]), flush=True)

                def shot(label):
                    if label in shots:
                        return
                    path = os.path.join(SHOTS, "%s_%s.png" % (tag, label))
                    try:
                        drive.screenshot(path)
                        shots[label] = dict(path=path, t=round(time.time() - t0, 2), near_h=s.get("near_h"),
                                            metric=s.get("near_metric"), drawn=s.get("near_drawn"), stance=s["stance"],
                                            err=s.get("bearing_err"), active=s.get("active"))
                        print("  shot", label, shots[label], flush=True)
                    except Exception as ex:
                        print("  shot failed", label, ex, flush=True)
                if t > 2.0:
                    shot("start")
                if look == "zombie" and s.get("active") and s.get("near_h") is not None and abs(s.get("bearing_err", 99)) < 25:
                    if s["near_h"] < 120:
                        shot("close120")
                    if s["near_h"] < 80:
                        shot("close80")
                    if s["near_h"] < 55:
                        shot("close55")
                    if s["near_h"] < 45:
                        shot("close45")
                if look == "down" and phase == 1 and t > 5.5:
                    shot("down")
                if look == "down" and phase == 2 and t > 9.5:
                    shot("back")
                if s["stance"] == 5 and grab_t is None:
                    grab_t = t
                if grab_t is not None and t - grab_t > 1.5:
                    shot("grab")
                time.sleep(0.01)
    finally:
        subprocess.run(["taskkill", "/F", "/PID", str(proc.pid)], capture_output=True)
        time.sleep(2)
        if variant == "before" and os.path.exists(OVR):
            os.remove(OVR)
        with open(os.path.join(OUT, "log_%s.json" % tag), "w") as fh:
            json.dump(dict(log=log, shots=shots), fh, indent=0)
        print("override present after run:", os.path.exists(OVR))
        summary(log)


def summary(log):
    arm = [s for s in log if s.get("active") and s["local"] == 0]
    near = [s for s in arm if s.get("near_metric") is not None and s["near_metric"] < 80.0]
    drawn = [s["near_metric"] for s in arm if s.get("near_drawn")]
    gone = [s["near_metric"] for s in arm if s.get("near_metric") is not None and not s.get("near_drawn")]
    local = sum(1 for s in arm for h in s["humans"] if h["k"] == 0 and h["drawn"])
    print("== %s: %d samples in first person; zombie nearer than 80 (the class-1 radius) in %d, not drawn in %d; nearest "
          "drawn %s; farthest not drawn %s; local body drawn in %d" % (
              tag, len(arm), len(near), sum(1 for s in near if not s["near_drawn"]),
              ("%.1f" % min(drawn)) if drawn else "-", ("%.1f" % max(gone)) if gone else "-", local))


main()
