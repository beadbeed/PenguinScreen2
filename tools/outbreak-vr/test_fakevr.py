"""Headset-free check of the first-person build (launch with launch_fakevr.ps1 first, File #1).

Phase "armed": the C++ lookAt should be ARMED with the fake swaying head pose. Checks the silence
words, that the camera yaw swings around a fixed anchor (no spin) while the stick is held up, and
that the character's heading follows the camera yaw (walks where the head looks). Saves slot 3.
Phase "features": holds (focus slot 3, near-cull radius 70), body follows view while idle, and the
camera holding still while the item screen is open (pauseWhen).
Phase "flat": in a flat launch, load slot 3 and check the guarded silence words were restored.

  python test_fakevr.py armed | features | flat
"""
import math
import os
import struct
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(__file__))
import drive
import padd
from pine import Pine, PineError

SILENCE = [0x58EAEC, 0x58EAFC, 0x58EB0C, 0x5A91E0, 0x5A9340, 0x5A9410, 0x630B00]
ORIGINAL = [0x0C163B1C, 0x0C163C8C, 0x0C163B60, 0x0C0D013C, 0x0C0D013C, 0x0C0D013C, 0xA2220FAA]


def u2f(u):
    return struct.unpack("<f", struct.pack("<I", u))[0]


def wait_ready(p):
    for _ in range(90):
        try:
            if p.game_id() and p.status() == "running":
                return
        except PineError:
            pass
        time.sleep(1)


def deg(a):
    return (a + 180.0) % 360.0 - 180.0


def armed(p):
    log = open(r"D:/Games/PS2/PenguinScreen2/logs/emulog.txt", encoding="utf-8", errors="replace").read()
    for line in log.splitlines():
        if "CameraDriver" in line or "Screen:" in line or "Stereo: first person" in line or "ProfileDB" in line and "SLPM" in line:
            print("  log:", line.strip()[:160])
    words = [p.read32(a) for a in SILENCE]
    print("silence words NOP'd:", sum(1 for w in words if w == 0), "/", len(words), ["0x%08X" % w for w in words])
    base = 0x476DD0 + p.read8(0x48BF7D) * 0x10E0
    print("fov=%.1f roll=%d" % (u2f(p.read32(0x306380)), p.read32(0x30638C)))

    samples = []
    stop = threading.Event()

    def sampler():
        while not stop.is_set():
            ex, ez, tx, tz = (u2f(p.read32(a)) for a in (0x306338, 0x306340, 0x306344, 0x30634C))
            cam = math.degrees(math.atan2(tx - ex, tz - ez))
            head = struct.unpack("<h", struct.pack("<H", p.read16(base + 0x92)))[0] * 360 / 65536
            samples.append((time.time(), cam, head))
            time.sleep(0.05)

    th = threading.Thread(target=sampler, daemon=True)
    th.start()
    padd.send({"op": "hold", "b": [], "ms": 6000, "ly": 1.0})
    stop.set()
    th.join()
    cams = [c for _, c, _ in samples]
    c0 = cams[0]
    spread = [deg(c - c0) for c in cams]
    lag = [abs(deg(c - h)) for _, c, h in samples[len(samples) // 3:]]
    print("camera yaw relative to start: min %.1f max %.1f (expect within about +-50: head sway only, no spin)" % (min(spread), max(spread)))
    print("heading vs camera yaw while walking: median %.1f deg, max %.1f deg (small = walks where it looks)" % (
        sorted(lag)[len(lag) // 2], max(lag)))
    drive.screenshot("D:/Games/PS2/screens/fakevr-walk.png", scale=2)
    p.save_state(3)
    time.sleep(3)
    print("saved slot 3 while armed")


def cam_yaw(p):
    ex, ez, tx, tz = (u2f(p.read32(a)) for a in (0x306338, 0x306340, 0x306344, 0x30634C))
    return math.degrees(math.atan2(tx - ex, tz - ez))


def sample(fn, secs, dt=0.05):
    out = []
    t0 = time.time()
    while time.time() - t0 < secs:
        out.append(fn())
        time.sleep(dt)
    return out


def features(p):
    base = 0x476DD0 + p.read8(0x48BF7D) * 0x10E0
    print("holds: focus slot %d (expect 3), near-cull radius %.1f (expect 75)" % (p.read8(0x3AEF74), u2f(p.read32(0x6D6CF4))))

    def pair():
        head = struct.unpack("<h", struct.pack("<H", p.read16(base + 0x92)))[0] * 360 / 65536
        return cam_yaw(p), head

    s = sample(pair, 4.0)
    d = sorted(abs(deg(c - h)) for c, h in s)
    spread = max(deg(c - s[0][0]) for c, _ in s) - min(deg(c - s[0][0]) for c, _ in s)
    print("body follows view (idle): camera swings %.0f deg; heading off by median %.1f, max %.1f deg (expect small)" % (
        spread, d[len(d) // 2], d[-1]))

    padd.send({"op": "press", "b": "start", "ms": 150})
    time.sleep(1.5)
    flag = p.read8(0x48BF7E)
    ys = sample(lambda: cam_yaw(p), 3.0)
    drive.screenshot("D:/Games/PS2/screens/fakevr-menu.png", scale=2)
    print("item screen open (flag %d): camera yaw range %.2f deg (expect ~0: held still)" % (flag, max(ys) - min(ys)))
    padd.send({"op": "press", "b": "cross", "ms": 150})
    time.sleep(1.5)
    ys = sample(lambda: cam_yaw(p), 3.0)
    print("item screen closed (flag %d): camera yaw range %.1f deg (expect the sway again)" % (p.read8(0x48BF7E), max(ys) - min(ys)))


def flat(p):
    p.load_state(3)
    time.sleep(5)
    words = [p.read32(a) for a in SILENCE]
    ok = [w == o for w, o in zip(words, ORIGINAL)]
    print("after loading the armed savestate in flat mode, original code words back:", sum(ok), "/", len(ok))
    print("holds put back: focus slot %d (expect 0), near-cull radius %.1f (expect 80)" % (p.read8(0x3AEF74), u2f(p.read32(0x6D6CF4))))
    for a, w, o in zip(SILENCE, words, ORIGINAL):
        if w != o:
            print("  0x%06X = 0x%08X (original 0x%08X)" % (a, w, o))


if __name__ == "__main__":
    pine = Pine()
    wait_ready(pine)
    {"armed": armed, "features": features, "flat": flat}[sys.argv[1]](pine)
