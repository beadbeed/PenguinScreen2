"""Hand targeting ([VR] HandTargeting), headset-free: File #1 savestate slot 7 (J's Bar, at the counter facing the pistol).
Offline only, normal speed, no OpenXR runtime; the right hand is scripted over PINE (PCSX2_VR_TEST_INPUT). Nothing is
written to the game and nothing is pressed.

    python -I handtest.py <portable dev install>    (its ini needs [VR] HandTargeting = true, and slot 7 in sstates/)

It runs that install, never D:\\Games\\PS2\\PenguinScreen2, and prints the "Hand targeting:" lines of four steps: the
hand 40 cm ahead (a target is found within range), the hand at the volume centre the build reports (inside; the round
trip lands on the centre), 45 cm to the side (outside, still the target) and the hand gone (target lost)."""
import os, re, subprocess, sys, time
os.environ["PCSX2_INI"] = os.path.join(sys.argv[1], "inis", "PenguinScreen2.ini")  # pine.py's offline check reads this ini
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pine import FakeController, wait_for_pine, require_offline, PineError, pose

DEV = sys.argv[1]
EXE = os.path.join(DEV, "pcsx2-qt.exe")
LOG = os.path.join(DEV, "logs", "emulog.txt")
ISO = r"D:\Games\PS2\games\Biohazard Outbreak File 1 (SLPM-65428) EN-patched.iso"
assert os.path.normcase(os.path.abspath(DEV)) != os.path.normcase(r"D:\Games\PS2\PenguinScreen2")
assert os.path.exists(os.path.join(DEV, "portable.txt"))


def kill():
    subprocess.run(["taskkill", "/F", "/IM", "pcsx2-qt.exe"], capture_output=True)
    time.sleep(3)


def log():
    return open(LOG, encoding="utf-8", errors="replace").read()


def hand_lines(text, since=0):
    return [l for l in text[since:].splitlines() if "Hand targeting" in l]


def main():
    require_offline()
    kill()
    env = dict(os.environ)
    env["XR_RUNTIME_JSON"] = os.path.join(DEV, "no-openxr-runtime.json")  # missing on purpose: no OpenXR runtime
    env["PCSX2_VR_TEST_INPUT"] = "1"
    env.pop("PCSX2_VR_FAKE_HEADPOSE", None)  # the scripted PINE head (with a position) must be the head
    subprocess.Popen([EXE, "--vr", "--", ISO], cwd=DEV, env=env)
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
    p.load_state(7)
    time.sleep(4)
    out = {}
    with FakeController(p) as fc:
        # 1. A hand 40 cm ahead, 20 cm below the head: inside the 300 cm range of the pistol at the counter.
        mark = len(log())
        fc.set_hand(1, grip=pose(pos=(0.15, -0.20, -0.40)))
        time.sleep(4)
        L = hand_lines(log(), mark)
        out["start"] = L
        found = [l for l in L if "target found" in l]
        m = re.search(r"VR \((-?[\d.]+), (-?[\d.]+), (-?[\d.]+)\) m", found[-1]) if found else None
        # 2. The hand at the volume's centre as the build maps it into the headset's space.
        if m:
            c = tuple(float(v) for v in m.groups())
            mark = len(log())
            fc.set_hand(1, grip=pose(pos=c))
            time.sleep(4)
            out["at_centre"] = hand_lines(log(), mark)
            # 3. 45 cm to the side of it: outside the volume (a 40 cm square), still a target.
            mark = len(log())
            fc.set_hand(1, grip=pose(pos=(c[0] + 0.45, c[1], c[2])))
            time.sleep(4)
            out["beside"] = hand_lines(log(), mark)
        # 4. Tracking loss: the hand disappears.
        mark = len(log())
        fc.clear_hand(1)
        time.sleep(3)
        out["lost"] = hand_lines(log(), mark)
    for k, v in out.items():
        print("==", k, "(%d lines)" % len(v))
        for l in v[:8]:
            print("  ", l[:300])
    kill()


main()
