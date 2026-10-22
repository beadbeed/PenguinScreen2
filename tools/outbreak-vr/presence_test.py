"""VR presence ([VR] VrPresence), the offline loopback check: one headset-free emulator sends its scripted head and
hands to presence_echo.Echo, which sends them back as a fake partner claiming an AI partner's slot; the emulator then
places that partner's VR head and hands at the AI partner's character. Offline only, normal speed, no OpenXR runtime,
nothing pressed, no game memory written (PINE only reads the records' positions). Loopback only: no firewall rule.

    python -I presence_test.py <portable dev install> [slot] [--dump <folder>]

It sets [VR] VrPresence = true, VrPresencePeer = 127.0.0.1:47801 and VrPresencePort = 47800 in THAT install's ini
(never D:\\Games\\PS2\\PenguinScreen2), refuses to start while another pcsx2-qt.exe or vrserver.exe runs, and kills only the
emulator it started. Steps: the partner drawn (positions checked against the records), the head turned and a hand moved
(the partner follows), the echo claiming this console's own slot (not drawn), a wrong key (dropped), the echo stopped
(stale: not drawn)."""
import math, os, re, subprocess, sys, time
DEV = sys.argv[1]
os.environ["PCSX2_INI"] = os.path.join(DEV, "inis", "PenguinScreen2.ini")  # pine.py's offline check reads this ini
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pine import FakeController, wait_for_pine, require_offline, PineError, pose
from presence_echo import Echo, key_hash

STATE_SLOT = int(sys.argv[2]) if len(sys.argv) > 2 and not sys.argv[2].startswith("--") else 6
DUMP = sys.argv[sys.argv.index("--dump") + 1] if "--dump" in sys.argv else None
EXE = os.path.join(DEV, "pcsx2-qt.exe")
LOG = os.path.join(DEV, "logs", "emulog.txt")
INI = os.environ["PCSX2_INI"]
ISO = r"D:\Games\PS2\games\Biohazard Outbreak File 1 (SLPM-65428) EN-patched.iso"
assert os.path.normcase(os.path.abspath(DEV)) != os.path.normcase(r"D:\Games\PS2\PenguinScreen2")
assert os.path.exists(os.path.join(DEV, "portable.txt"))
RECORDS, STRIDE, POS, ID = 0x476DD0, 0x10E0, 0x38, 0x03


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


def presence_lines(since):
    return [l for l in log()[since:].splitlines() if "(VR) Presence" in l]


V3 = r"\((-?[\d.]+), (-?[\d.]+), (-?[\d.]+)\)"


def vec(s):
    return tuple(float(v) for v in s)


def sub(a, b):
    return tuple(x - y for x, y in zip(a, b))


def length(a):
    return math.sqrt(sum(x * x for x in a))


def check_positions(lines, local_head, local_hands, want_facing):
    """The newest partner line against the scripted head and hands: the partner is ours moved by (its record - the
    packet's character), turned by the mapping's yaw only, so the horizontal distance is the records' distance over
    100 and the height difference theirs over 100, and both hands moved exactly as the head."""
    part = [l for l in lines if "partner slot" in l and "VR head" in l]
    if not part:
        return None
    m = re.search(r"partner slot (\d+) VR head %s facing %s, left %s, right %s m; its record at game %s, the packet's "
                  r"character %s" % (V3, V3, V3, V3, V3, V3), part[-1])
    if not m:
        return dict(ok=False, why="unparsed: " + part[-1])
    g = m.groups()
    head, facing, left, right = vec(g[1:4]), vec(g[4:7]), vec(g[7:10]), vec(g[10:13])
    rec, char = vec(g[13:16]), vec(g[16:19])
    shift = sub(rec, char)
    d = sub(head, local_head)
    want_h = math.hypot(shift[0], shift[2]) / 100.0
    got_h = math.hypot(d[0], d[2])
    want_y = shift[1] / 100.0
    dl, dr = sub(sub(left, local_hands[0]), d), sub(sub(right, local_hands[1]), d)
    res = dict(slot=int(g[0]), head=head, facing=facing, left=left, right=right, record=rec, character=char,
               offset_vr=d, horiz_m=(got_h, want_h), height_m=(d[1], want_y), hands_vs_head_m=(length(dl), length(dr)))
    res["facing_err"] = length(sub(facing, want_facing))
    res["ok"] = res["facing_err"] < 0.02 and abs(got_h - want_h) < 0.03 and abs(d[1] - want_y) < 0.03 and length(dl) < 0.03 and length(dr) < 0.03
    return res


def main():
    if running("pcsx2-qt.exe") or running("vrserver.exe"):
        sys.exit("another pcsx2-qt.exe or vrserver.exe is running: wait for it (shared PINE slot, SteamVR untouched).")
    require_offline()
    set_ini(INI, "VR", {"VrPresence": "true", "VrPresencePeer": "127.0.0.1:47801", "VrPresencePort": "47800",
                        "VrPresenceKey": "loopback-test"})
    env = dict(os.environ)
    env["XR_RUNTIME_JSON"] = os.path.join(DEV, "no-openxr-runtime.json")  # missing on purpose: no OpenXR runtime
    env["PCSX2_VR_TEST_INPUT"] = "1"
    env["PCSX2_VR_PRESENCE_LOOPBACK"] = "1"
    env.pop("PCSX2_VR_FAKE_HEADPOSE", None)
    if DUMP:
        env["PCSX2_VR_PRESENCE_DUMP"] = DUMP
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

        def records():
            r = []
            for k in range(4):
                a = RECORDS + k * STRIDE
                r.append((p.read8(a + ID), tuple(round(p.readf(a + POS + 4 * i), 1) for i in range(3))))
            return r

        head = pose(pos=(0.0, 0.0, 0.0))
        hands = [(-0.20, -0.35, -0.35), (0.22, -0.30, -0.40)]
        with FakeController(p, head=head) as fc:
            fc.set_hand(0, grip=pose(pos=hands[0]), aim=pose(pos=hands[0]), squeeze=0.2)
            fc.set_hand(1, grip=pose(pos=hands[1]), aim=pose(pos=hands[1]), squeeze=0.8)
            # 1. The partner, drawn at AI partner 1's character.
            mark = len(log())
            time.sleep(7)
            L = presence_lines(mark)
            out["1 drawn"] = dict(lines=L, records=records(), check=check_positions(L, (0.0, 0.0, 0.0), hands, (0.0, 0.0, -1.0)))
            # 2. The head turned 40 deg left and the right hand 20 cm further out: the partner's follow.
            hands2 = [hands[0], (0.42, -0.30, -0.40)]
            mark = len(log())
            fc.set_head(pose(yaw_deg=40.0, pos=(0.0, 0.0, 0.0)))
            fc.set_hand(1, grip=pose(pos=hands2[1]), aim=pose(pos=hands2[1]), squeeze=0.8)
            time.sleep(5)
            L = presence_lines(mark)
            out["2 turned and moved"] = dict(lines=L, records=records(), check=check_positions(L, (0.0, 0.0, 0.0), hands2,
                                                                                  (-math.sin(math.radians(40)), 0.0, -math.cos(math.radians(40)))))
            # 3. The echo claims this console's own slot: not drawn.
            mark = len(log())
            echo.slot = 0
            time.sleep(4)
            out["3 own slot"] = dict(lines=presence_lines(mark))
            # 4. A wrong key: dropped.
            mark = len(log())
            echo.slot = 1
            echo.key = key_hash("not our word")
            time.sleep(4)
            out["4 wrong key"] = dict(lines=presence_lines(mark))
            # 5. Back to the right key, then the echo stops answering: stale, not drawn.
            echo.key = None
            time.sleep(3)
            mark = len(log())
            echo.paused = True
            time.sleep(4)
            out["5 echo stopped"] = dict(lines=presence_lines(mark))
    finally:
        echo.stop()
        try:
            proc.kill()  # only the emulator this script started
        except OSError:
            pass
        proc.wait(timeout=30)
    for k, v in out.items():
        print("==", k)
        for l in v.get("lines", [])[-6:]:
            print("   ", l[:400])
        if "records" in v:
            print("   records (id, position):", v["records"])
        if v.get("check") is not None:
            print("   check:", v["check"])
    print("echo:", echo.stats)


main()
