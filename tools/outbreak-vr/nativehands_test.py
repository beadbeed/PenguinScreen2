"""NativeHands, the offline measurement ([VR] NativeHands: the game draws your hands and the weapon it holds). One headset-free
emulator (no OpenXR runtime, PINE scripted head and hands, offline, normal speed), File #1. Scenarios:

    python -I nativehands_test.py <portable dev install> sweep   [slot]   the held weapon's drawn matrix against the controller as
                                                                          the hand moves (a gun near the face included)
    python -I nativehands_test.py <portable dev install> dropout [slot]   the overlay hand comes back the frame the native pose
                                                                          drops out, and the native hand again after 0.2 s
    python -I nativehands_test.py <portable dev install> shots   [slot]   slot 5: shots along the laser still hit (and miss)
    python -I nativehands_test.py <portable dev install> melee   [slot]   slot 5: a fast swing of the gun hand still lands

It uses THAT install's ini (never D:\\Games\\PS2\\PenguinScreen2) and its PINE slot, sets VrBody, VrBodySelf, TrueStereo and
GunObjectAim there, refuses to start while vrserver.exe runs, presses nothing but the scripted controllers, writes no game
memory and kills only the emulator it started (by its own handle). Evidence is memory (the held object's matrix, read at a
vsync, after the draw) and the emulog's `(VR) NativeHands ...` and `(VR) Gun ray ...` lines; window captures may come out
black. Set PCSX2_VR_NATIVE_NOSHIFT=1 in the environment to compare the gameplay side without the eye shift."""
import math, os, re, struct, subprocess, sys, time

DEV = sys.argv[1]
os.environ["PCSX2_INI"] = os.path.join(DEV, "inis", "PenguinScreen2.ini")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pine import FakeController, wait_for_pine, require_offline, PineError, pose

SCEN = sys.argv[2] if len(sys.argv) > 2 else "sweep"
SLOT = int(sys.argv[3]) if len(sys.argv) > 3 else (5 if SCEN in ("shots", "melee", "close") else 7)
EXE = os.path.join(DEV, "pcsx2-qt.exe")
LOG = os.path.join(DEV, "logs", "emulog.txt")
ISO = r"D:\Games\PS2\games\Biohazard Outbreak File 1 (SLPM-65428) EN-patched.iso"
assert os.path.normcase(os.path.abspath(DEV)) != os.path.normcase(r"D:\Games\PS2\PenguinScreen2")
assert os.path.exists(os.path.join(DEV, "portable.txt"))
REC = 0x476DD0
H = 1.2


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


def f32(p, a):
    return struct.unpack("<f", struct.pack("<I", p.read32(a)))[0]


WEAPON = re.compile(r"NativeHands weapon: item (\d+) type (\d+) object ([0-9A-F]+); drawn origin \(([-\d.]+), ([-\d.]+), ([-\d.]+)\), "
                    r"([-\d.]+) cm from the controller grip; vs the game's own attach maths on the posed wrist ([-\d.]+) cm / ([-\d.]+) deg; "
                    r"barrel ([-\d.]+) deg off the controller's forward; drawn muzzle \(([-\d.]+), ([-\d.]+), ([-\d.]+)\); vs the controller's "
                    r"own placement: origin ([-\d.]+) cm, axes ([-\d.]+) deg, muzzle ([-\d.]+) cm; gun ray \(shot start\) (\(none\))? ?"
                    r"([-\d.]+) cm from the drawn muzzle, its direction ([-\d.]+) deg off the barrel")


def last_weapon(mark):
    ms = list(WEAPON.finditer(log_text()[mark:]))
    return ms[-1] if ms else None


def start():
    if running("vrserver.exe"):
        sys.exit("vrserver.exe is running (Alex may be playing): not launching.")
    require_offline()
    slot = int(ini_value(os.environ["PCSX2_INI"], "EmuCore", "PINESlot") or 28200)
    set_ini(os.environ["PCSX2_INI"], "VR", {"VrPresence": "false", "VrBody": "true", "VrBodySelf": "true", "TrueStereo": "true",
                                            "GunObjectAim": "true", "PhysicalMelee": "true", "PhysicalMeleeFists": "true"})
    env = dict(os.environ)
    env["XR_RUNTIME_JSON"] = os.path.join(DEV, "no-openxr-runtime.json")  # missing on purpose: no OpenXR runtime
    env["PCSX2_VR_TEST_INPUT"] = "1"
    env["PCSX2_VR_BODY_LOG"] = "1"
    env["PCSX2_VR_NATIVE_LOG"] = "1"
    env["PCSX2_VR_GUNRAY_LOG"] = "1"
    env.pop("PCSX2_VR_FAKE_HEADPOSE", None)
    print("tasklist pcsx2-qt before launch:", running("pcsx2-qt.exe"), "| PINE slot", slot, "| scenario", SCEN, "| slot", SLOT)
    proc = subprocess.Popen([EXE, "--vr", "--", ISO], cwd=DEV, env=env)
    print("started PID", proc.pid)
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
    return proc, p


def hands(fc, rpos, rpose=None, trigger=0.0, bumper=True):
    """Right hand at rpos (relative to the head), pointing as rpose says; the left hand low and still."""
    R = tuple(a + b for a, b in zip(rpos, (0.0, H, 0.0)))
    pr = rpose or (0.0, 0.0)
    fc.set_hand(1, grip=pose(yaw_deg=pr[0], pitch_deg=pr[1], pos=R), aim=pose(yaw_deg=pr[0], pitch_deg=pr[1], pos=R), squeeze=0.3,
                trigger=trigger, buttons=("bumper",) if bumper else ())
    L = (-0.20, H - 0.45, -0.30)
    fc.set_hand(0, grip=pose(pos=L), aim=pose(pos=L), squeeze=0.3)


def sweep(p):
    poses = [
        ("aim forward", (0.22, -0.30, -0.42), (0.0, 0.0)),
        ("low, near the lap", (0.18, -0.50, -0.25), (0.0, -10.0)),
        ("gun near the face", (0.10, -0.08, -0.28), (0.0, 0.0)),
        ("gun at the face, turned 40 left, 20 up", (0.06, -0.04, -0.30), (40.0, 20.0)),
        ("out to the right, turned 60 right", (0.50, -0.25, -0.15), (-60.0, 0.0)),
        ("reaching far forward, pitched down 35", (0.20, -0.20, -0.58), (0.0, -35.0)),
        ("raised high, pitched up 50", (0.20, 0.10, -0.35), (0.0, 50.0)),
    ]
    rows = []
    with FakeController(p, head=pose(pos=(0.0, H, 0.0)), recenter=True) as fc:
        time.sleep(1.5)
        for name, rp, ro in poses:
            mark = len(log_text())
            hands(fc, rp, ro)
            time.sleep(2.2)
            m = last_weapon(mark)
            cmp_ = list(re.finditer(r"resolved weapon pose against the controller's own placement: muzzle ([-\d.]+) cm \(([-\d.]+) cm without the eye's shift\), barrel ([-\d.]+) deg", log_text()[mark:]))
            if cmp_:
                c = cmp_[-1]
                print("   resolved pose vs the controller's own placement: muzzle %s cm (%s cm if the shot started without the eye's shift), barrel %s deg" % c.groups())
            if not m:
                print("== %-42s NO weapon line" % name)
                continue
            v = [float(x) if x and not x.startswith("(") else 0.0 for x in m.groups()[6:]]
            rows.append((name, v))
            print("== %-42s item %s type %s | attach on the posed wrist %.2f cm %.2f deg | vs the controller's placement: origin %.2f cm, "
                  "axes %.2f deg, muzzle %.2f cm | barrel vs controller forward %.2f deg | gun ray start vs drawn muzzle %.2f cm, dir %.2f deg" % (
                      name, m.group(1), m.group(2), v[1], v[2], v[7], v[8], v[9], v[3], v[11], v[12]))
    if rows:
        print("worst: attach %.2f cm / %.2f deg; controller placement %.2f cm / %.2f deg / muzzle %.2f cm; barrel %.2f deg; gun ray %.2f cm / "
              "%.2f deg" % (max(r[1][1] for r in rows), max(r[1][2] for r in rows), max(r[1][7] for r in rows), max(r[1][8] for r in rows),
                            max(r[1][9] for r in rows), max(r[1][3] for r in rows), max(r[1][11] for r in rows), max(r[1][12] for r in rows)))


def dropout(p):
    with FakeController(p, head=pose(pos=(0.0, H, 0.0)), recenter=True) as fc:
        time.sleep(1.5)
        hands(fc, (0.22, -0.30, -0.42))
        time.sleep(1.5)
        for _ in range(3):
            fc.clear_hand(1)  # the right controller untracked
            time.sleep(1.0)
            hands(fc, (0.22, -0.30, -0.42))
            time.sleep(1.5)
    for l in log_text().splitlines():
        if "NativeHands" in l and "weapon" not in l:
            print(l)

ENEMIES, STRIDE, NENEMY = 0x3FFCA0, 0xD90, 64


def enemies(p):
    """Present enemies: (slot, x, y, z, hp)."""
    out = []
    for k in range(NENEMY):
        base = ENEMIES + STRIDE * k
        if p.read8(base) == 0:
            continue
        hp = p.read16(base + 0x544)
        hp = hp - 65536 if hp >= 32768 else hp
        out.append((k, f32(p, base + 0x38), f32(p, base + 0x3C), f32(p, base + 0x40), hp))
    return out


def target_frame(p, g, base_yaw_deg):
    """Where a game point is for a head that turns to look at it: the head's yaw (degrees, VR: counter-clockwise, as the game's) and the
    point's distance across the floor and height over the eye (metres). From the character's position and the true base yaw
    (telemetry; the view is recentred on the scripted head at (0, H, 0) with no turn); the eye sits 155 cm over the character."""
    cx, cy, cz = f32(p, REC + 0x38), f32(p, REC + 0x3C), f32(p, REC + 0x40)
    dx, dz = g[0] - cx, g[2] - cz
    psi = math.degrees(math.atan2(dx, dz)) - base_yaw_deg
    psi = (psi + 180.0) % 360.0 - 180.0
    return psi, math.hypot(dx, dz) / 100.0, (g[1] - (cy + 155.0)) / 100.0


def turned(psi_deg, local):
    """A head-relative offset (x right, y up, z back) as it sits for a head turned psi_deg about +Y."""
    c, s = math.cos(math.radians(psi_deg)), math.sin(math.radians(psi_deg))
    return (local[0] * c + local[2] * s, local[1], -local[0] * s + local[2] * c)


def aim_pose(frm, to):
    """The yaw and pitch (degrees) that point a controller at `to` from `frm` (VR axes)."""
    dx, dy, dz = to[0] - frm[0], to[1] - frm[1], to[2] - frm[2]
    return math.degrees(math.atan2(-dx, -dz)), math.degrees(math.atan2(dy, math.hypot(dx, dz)))


def right_hand(fc, pos, yaw, pitch, trigger=0.0):
    fc.set_hand(1, grip=pose(yaw_deg=yaw, pitch_deg=pitch, pos=pos), aim=pose(yaw_deg=yaw, pitch_deg=pitch, pos=pos), squeeze=0.3,
                trigger=trigger, buttons=("bumper",))


def nearest_enemy(p, lo, hi, head_y_off):
    cx, cz = f32(p, REC + 0x38), f32(p, REC + 0x40)
    tb = p.telemetry()
    best = None
    for e in enemies(p):
        if e[4] <= 0:
            continue
        psi, dist, dy = target_frame(p, (e[1], e[2] + head_y_off, e[3]), tb["base_yaw_now"])
        if lo < dist < hi and (best is None or dist < best[1]):
            best = (psi, dist, dy, e)
    return best


def shots(p):
    """Slot 5, the 45 Auto: the head turns to the zombie; three shots with the laser's ray on it, three with it 50 deg over."""
    results = []
    with FakeController(p, head=pose(pos=(0.0, H, 0.0)), recenter=True) as fc:
        time.sleep(0.8)
        local_hand = (0.20, -0.28, -0.40)
        fc.set_hand(0, grip=pose(pos=(-0.2, H - 0.45, -0.3)), aim=pose(pos=(-0.2, H - 0.45, -0.3)), squeeze=0.3)
        for case, up in (("on the zombie", 0.0), ("50 deg over it", 50.0)):
            for n in range(2):
                tgt = nearest_enemy(p, 0.5, 3.5, 120.0)
                if not tgt:
                    print("no enemy within 3.5 m; waiting")
                    time.sleep(1.0)
                    continue
                psi, dist, dy, e = tgt
                fc.set_head(pose(yaw_deg=psi, pos=(0.0, H, 0.0)))
                off = turned(psi, local_hand)
                hand = (off[0], H + off[1], off[2])
                yaw_l, pitch = aim_pose(local_hand, (0.0, dy, -dist))
                mark = len(log_text())
                hp0 = e[4]
                right_hand(fc, hand, psi + yaw_l, pitch + up)
                time.sleep(1.0)  # raised, the character turned to it (slot 5's zombie grabs at 5-7 s: shoot before)
                right_hand(fc, hand, psi + yaw_l, pitch + up, trigger=1.0)
                time.sleep(0.4)
                right_hand(fc, hand, psi + yaw_l, pitch + up, trigger=0.0)
                time.sleep(0.5)
                now = {x[0]: x[4] for x in enemies(p)}
                new = log_text()[mark:]
                shot_lines = [l for l in new.splitlines() if "Gun ray: shot" in l]
                hit_lines = [l for l in new.splitlines() if "hit, enemy" in l]
                cmp_ = list(re.finditer(r"resolved weapon pose against the controller's own placement: muzzle ([-\d.]+) cm \(([-\d.]+) cm without the eye's shift\), barrel ([-\d.]+) deg", new))
                print("%s #%d: enemy %d (%.2f m away, head turned %.0f deg) HP %d -> %s, %d shot(s), %d hit line(s); resolved pose vs the controller: %s%s" % (
                    case, n + 1, e[0], dist, psi, hp0, now.get(e[0], "gone"), len(shot_lines), len(hit_lines),
                    ("muzzle %s cm, barrel %s deg" % (cmp_[-1].group(1), cmp_[-1].group(3))) if cmp_ else "n/a",
                    (" | " + hit_lines[-1].strip()[:120]) if hit_lines else ""))
                results.append((case, hp0, now.get(e[0], None), len(shot_lines)))
    for case in ("on the zombie", "50 deg over it"):
        r = [x for x in results if x[0] == case]
        print("%s: %d shots counted, %d with an HP drop" % (case, sum(x[3] for x in r), sum(1 for x in r if x[2] is not None and x[2] < x[1])))


def close(p):
    """Slot 5: the zombie still draws when it is close (its drawn-this-frame byte, +0x198, read every 50 ms for 4 s, with the head turned to
    it and the hands native); Kevin's own bones are collapsed while the game draws only his forearms and hands."""
    with FakeController(p, head=pose(pos=(0.0, H, 0.0)), recenter=True) as fc:
        time.sleep(1.0)
        hands(fc, (0.22, -0.30, -0.42))
        tgt = nearest_enemy(p, 0.0, 3.0, 120.0)
        if tgt:
            fc.set_head(pose(yaw_deg=tgt[0], pos=(0.0, H, 0.0)))
        time.sleep(1.0)
        seen = {}
        for _ in range(80):
            cx, cz = f32(p, REC + 0x38), f32(p, REC + 0x40)
            for k in range(NENEMY):
                base = ENEMIES + STRIDE * k
                if p.read8(base) == 0:
                    continue
                d = math.hypot(f32(p, base + 0x38) - cx, f32(p, base + 0x40) - cz) / 100.0
                if d < 2.5:
                    n, drawn, dmin = seen.get(k, (0, 0, 9.0))
                    seen[k] = (n + 1, drawn + (1 if p.read8(base + 0x198) else 0), min(dmin, d))
            time.sleep(0.05)
        for k, (n, drawn, dmin) in sorted(seen.items()):
            print("enemy %d: drawn-this-frame set in %d of %d samples (nearest %.2f m)" % (k, drawn, n, dmin))
        bones = []
        model = p.read32(REC + 0x34)
        for b in (0, 21, 11):
            s = p.read32(model + 0x820 + 4 * b)
            bones.append((b, [round(f32(p, s + 4 * q), 2) for q in range(4)]))
        print("Kevin's bones 0, 21, 11 (first row of each world matrix; zero = collapsed, drawn nowhere):", bones)


def melee(p):
    """Slot 5: the head turns to the zombie and a fast swing of the gun hand goes through it (PhysicalMelee + PhysicalMeleeFists: a gun-butt
    strike); the log's contact line."""
    with FakeController(p, head=pose(pos=(0.0, H, 0.0)), recenter=True) as fc:
        time.sleep(2.0)
        fc.set_hand(0, grip=pose(pos=(-0.2, H - 0.45, -0.3)), aim=pose(pos=(-0.2, H - 0.45, -0.3)), squeeze=0.3)
        swings = 0
        t_end = time.time() + 45
        while time.time() < t_end and swings < 6:
            tgt = nearest_enemy(p, 0.0, 0.95, 100.0)
            if not tgt:
                time.sleep(0.25)
                continue
            psi, dist, dy, e = tgt
            fc.set_head(pose(yaw_deg=psi, pos=(0.0, H, 0.0)))
            mark = len(log_text())
            hp0 = e[4]
            a, b = max(dist - 0.32, 0.25), dist + 0.10
            start_l = (0.0, dy * a / dist - 0.05, -a)
            end_l = (0.0, dy * b / dist - 0.05, -b)
            p0, p1 = turned(psi, (start_l[0], start_l[1], start_l[2])), turned(psi, (end_l[0], end_l[1], end_l[2]))
            p0 = (p0[0], H + p0[1], p0[2])
            p1 = (p1[0], H + p1[1], p1[2])
            yaw_l, pitch = aim_pose(start_l, end_l)
            right_hand(fc, p0, psi + yaw_l, pitch)
            time.sleep(1.4)
            steps = 6
            for i in range(1, steps + 1):
                q = tuple(p0[k] + (p1[k] - p0[k]) * i / steps for k in range(3))
                right_hand(fc, q, psi + yaw_l, pitch)
                time.sleep(0.02)
            time.sleep(1.0)
            now = {x[0]: x[4] for x in enemies(p)}
            new = log_text()[mark:]
            swings += 1
            mel = [l.strip()[:150] for l in new.splitlines() if "(VR) Melee:" in l]
            print("swing %d at enemy %d (%.2f m away): HP %d -> %s | %s" % (swings, e[0], dist, hp0, now.get(e[0], "gone"),
                                                                           " / ".join(mel[-3:]) if mel else "no melee line"))
            right_hand(fc, turned(psi, (0.2, -0.4, -0.3)) and (turned(psi, (0.2, -0.4, -0.3))[0], H - 0.4, turned(psi, (0.2, -0.4, -0.3))[2]), psi, 0.0)
            time.sleep(1.5)


def main():
    proc, p = start()
    try:
        p.load_state(SLOT)
        time.sleep(3)
        {"sweep": sweep, "dropout": dropout, "shots": shots, "melee": melee, "close": close}.get(SCEN, lambda q: print("scenario not written yet"))(p)
    finally:
        try:
            proc.kill()  # only the emulator this script started
        except OSError:
            pass
        proc.wait(timeout=30)
        print("stopped PID", proc.pid)


main()
