"""Real hand pickups ([VR] HandPickup), headset-free: a scripted right hand on a floor item, a scripted squeeze, and what
the game did. File #1 offline only, normal speed, no OpenXR runtime (PCSX2_VR_TEST_INPUT, PCSX2_VR_PICKUP_DEBUG).

    python -I pickuptest.py <portable dev install> <slot> <item id>[,<item id>...] [--turn DEG] [--release-early] [--no-squeeze] [--vr x,y,z]
    (several ids: one pickup after the other in the same run, e.g. to fill the last free cell and then try a full inventory)

Runs that install (never D:\\Games\\PS2\\PenguinScreen2), loads the slot, finds the item's volume centre in the headset's
space from the build's debug lines, optionally turns the head (and with it the character) DEG degrees to the left first,
puts the right hand there, squeezes (and holds; --release-early lets go after 0.3 s),
polls the take-it prompt flags and prints the "Hand pickup" log lines and the local record's item cells before and after.
It refuses to start while another pcsx2-qt.exe or vrserver.exe runs, and kills only the emulator it started."""
import os, re, subprocess, sys, time
os.environ["PCSX2_INI"] = os.path.join(sys.argv[1], "inis", "PenguinScreen2.ini")  # pine.py's offline check reads this ini
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pine import FakeController, wait_for_pine, require_offline, PineError, pose

DEV = sys.argv[1]
SLOT = int(sys.argv[2])
ITEMS = [int(x) for x in sys.argv[3].split(",")]
TURN = float(sys.argv[sys.argv.index("--turn") + 1]) if "--turn" in sys.argv else 0.0
EARLY = "--release-early" in sys.argv
NO_SQUEEZE = "--no-squeeze" in sys.argv
# --vr x,y,z: put the hand there instead of the debug line's centre (HandPickup off prints no debug lines)
AT = tuple(float(v) for v in sys.argv[sys.argv.index("--vr") + 1].split(",")) if "--vr" in sys.argv else None
DUMP = sys.argv[sys.argv.index("--dump") + 1] if "--dump" in sys.argv else None  # the prompt's RAM (never committed)
EXE = os.path.join(DEV, "pcsx2-qt.exe")
LOG = os.path.join(DEV, "logs", "emulog.txt")
ISO = r"D:\Games\PS2\games\Biohazard Outbreak File 1 (SLPM-65428) EN-patched.iso"
assert os.path.normcase(os.path.abspath(DEV)) != os.path.normcase(r"D:\Games\PS2\PenguinScreen2")
assert os.path.exists(os.path.join(DEV, "portable.txt"))
RECORDS, STRIDE, SLOT_BYTE = 0x476DD0, 0x10E0, 0x48BF7D


def running(name):
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq %s" % name, "/NH"], capture_output=True, text=True).stdout
    return name.lower() in out.lower()


def log():
    try:
        return open(LOG, encoding="utf-8", errors="replace").read()
    except OSError:
        return ""


def lines(since, key):
    return [l for l in log()[since:].splitlines() if key in l]


def cells(p, rec):
    return [p.read8(rec + 0xC84 + k) for k in range(4)]


def main():
    require_offline()
    for name in ("pcsx2-qt.exe", "vrserver.exe"):
        if running(name):
            sys.exit("%s is running: not starting (another agent's emulator, or Alex playing)" % name)
    env = dict(os.environ)
    env["XR_RUNTIME_JSON"] = os.path.join(DEV, "no-openxr-runtime.json")  # missing on purpose: no OpenXR runtime
    env["PCSX2_VR_TEST_INPUT"] = "1"
    env["PCSX2_VR_PICKUP_DEBUG"] = "1"
    env.pop("PCSX2_VR_FAKE_HEADPOSE", None)
    proc = subprocess.Popen([EXE, "--vr", "--", ISO], cwd=DEV, env=env)
    try:
        run(proc)
    finally:
        subprocess.run(["taskkill", "/F", "/PID", str(proc.pid)], capture_output=True)
        time.sleep(2)


def item_centre(since, item, wait=6.0):
    end = time.time() + wait
    while time.time() < end:
        found = []
        for l in lines(since, "Hand pickup debug: item"):
            m = re.search(r"node (0x[0-9A-F]+) item (\d+) record (\d+) centre game \((-?[\d.]+), (-?[\d.]+), (-?[\d.]+)\) "
                          r"VR \((-?[\d.]+), (-?[\d.]+), (-?[\d.]+)\)", l)
            if m and int(m.group(2)) == item:
                found.append(m)
        if found:
            m = found[-1]
            return m.group(1), int(m.group(3)), tuple(float(m.group(k)) for k in (4, 5, 6)), tuple(float(m.group(k)) for k in (7, 8, 9))
        time.sleep(0.5)
    return None


def run(proc):
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
    p.load_state(SLOT)
    time.sleep(4)
    with FakeController(p) as fc:
        time.sleep(1.5)
        slot = p.read8(SLOT_BYTE)
        rec = RECORDS + slot * STRIDE
        print("local slot", slot, "record 0x%X" % rec, "position (%.1f, %.1f, %.1f)" % (p.readf(rec + 0x38), p.readf(rec + 0x3C),
              p.readf(rec + 0x40)), "heading %d" % p.read16(rec + 0x92))
        before = cells(p, rec)
        print("cells before", before)
        mark = len(log())
        if TURN:
            # The character turns with the head in first person (a heading written into the record is driven back at
            # once), so the scripted head looks TURN degrees to the left and the hand stays on the item.
            h0 = p.read16(rec + 0x92)
            fc.set_head(pose(yaw_deg=TURN))
            time.sleep(2.0)
            h1 = p.read16(rec + 0x92)
            d = ((h1 - h0 + 32768) % 65536 - 32768) * 360.0 / 65536
            print("head turned %.0f deg left: heading %d -> %d (%.1f deg)" % (TURN, h0, h1, d))
            mark = len(log())
        for item in ITEMS:
            c = ("?", -1, None, AT) if AT else item_centre(mark, item)
            if not c:
                print("item %d: no debug line (not in the trigger list?)" % item)
                for l in lines(0, "Hand pickup")[-15:]:
                    print("  ", l)
                continue
            node, record, game, vr = c
            print("item %d: node %s record %d centre game %s VR %s" % (item, node, record, game, vr))
            fc.set_hand(1, grip=pose(pos=vr), squeeze=0.0)
            time.sleep(1.0)
            if DUMP:
                with open(DUMP + ".before", "wb") as f:
                    f.write(p.read_block(0x700000, 0x30000))
                    f.write(p.read_block(rec, 0x10E0))
            if not NO_SQUEEZE:
                fc.set_hand(1, grip=pose(pos=vr), squeeze=1.0)
            t0 = time.time()
            seen = []
            released = False
            dumped = False
            while time.time() - t0 < 6.0:
                if EARLY and not released and time.time() - t0 > 0.3:
                    fc.set_hand(1, grip=pose(pos=vr), squeeze=0.0)
                    released = True
                s = (p.read32(0x720AA0), p.read8(0x720BE4), p.read8(0x720BE2), p.read8(0x723F94), p.read16(0x720B8C),
                     p.read8(rec + 0xC78), p.read8(rec + 0xC7A), p.read8(rec + 0x54F))
                if not seen or seen[-1][1] != s:
                    seen.append((round(time.time() - t0, 2), s))
                if DUMP and s[0] == 1 and s[2] == 2 and not dumped:
                    time.sleep(0.4)
                    with open(DUMP, "wb") as f:
                        f.write(p.read_block(0x700000, 0x30000))
                        f.write(p.read_block(rec, 0x10E0))
                    dumped = True
                    print("dumped the prompt's menu RAM to", DUMP)
                time.sleep(0.05)
            fc.set_hand(1, grip=pose(pos=vr), squeeze=0.0)
            time.sleep(2.0)
            after = cells(p, rec)
            print("flags over time (t, (overlay, which, sub, grid, cursor index, status, result, action)):")
            for t, s in seen:
                print("  ", t, s)
            print("cells after", after, "-> item record %d %s" % (record, "IN THE CELLS" if record in after and record not in before else "not taken"))
            print("overlay now", p.read32(0x720AA0))
            for l in lines(mark, "Hand pickup")[:]:
                if "debug" not in l:
                    print("  ", l)
            mark = len(log())
            before = cells(p, rec)
            fc.set_hand(1, grip=pose(pos=(0.3, -0.5, -0.2)), squeeze=0.0)
            time.sleep(1.0)


main()
