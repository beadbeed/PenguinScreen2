"""Records a play session without anyone having to start it: waits for PenguinScreen2, then logs the
local character's state (status, menu, stance, action, HP, heading, position, camera yaw) a few times a
second and screenshots the emulator window every few seconds, until the emulator closes. On exit it
copies the session's emulog next to the recording.

  python session_recorder.py [out_root] [shot_every_s]

Output: <out_root>/<yyyymmdd-hhmmss>/state.csv, shot-*.png, emulog.txt.
Stays out of the way online: if the network adapter is enabled in the ini it only screenshots (no PINE),
since obsrv must never see the emulator stall. PINE reads are a few small requests per second.
"""
import math
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
import drive
from pine import Pine, PineError

INI = r"D:\Games\PS2\PenguinScreen2\inis\PenguinScreen2.ini"
EMULOG = r"D:\Games\PS2\PenguinScreen2\logs\emulog.txt"

# Per serial: (records base, stride, local slot byte, status, menu flag, overlay, eye, target)
GAMES = {
    "SLPM-65428": (0x476DD0, 0x10E0, 0x48BF7D, 0x48BF60, 0x48BF7E, 0x540004, 0x306338, 0x306344),
    "SLPM-65692": (0x47BD30, 0x1100, 0x4912BD, 0x4912A0, 0x4912BE, 0x543004, 0x313548, 0x313554),
}


def network_enabled():
    try:
        for line in open(INI, encoding="utf-8", errors="replace"):
            if line.strip().replace(" ", "").lower() == "ethenable=true":
                return True
    except OSError:
        pass
    return False


def emulator_running():
    try:
        drive.find_window()
        return True
    except Exception:
        return False


def f32(u):
    return struct.unpack("<f", struct.pack("<I", u))[0]


def s16(u):
    return struct.unpack("<h", struct.pack("<H", u))[0]


def record(out_root, shot_every):
    while not emulator_running():
        time.sleep(2)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    out = os.path.join(out_root, stamp)
    os.makedirs(out, exist_ok=True)
    online = network_enabled()
    print("recording to", out, "(screenshots only: network adapter on)" if online else "")
    pine = None
    t0 = time.time()
    last_shot = -1e9
    with open(os.path.join(out, "state.csv"), "w") as csv:
        csv.write("t,serial,status,menu,overlay,stance,action,hp,hp_max,heading,x,y,z,cam_yaw,cam_pitch\n")
        while emulator_running():
            t = time.time() - t0
            if t - last_shot >= shot_every:
                try:
                    drive.screenshot(os.path.join(out, "shot-%06.1f.png" % t), scale=2)
                except Exception:
                    pass
                last_shot = t
            if not online:
                try:
                    if pine is None:
                        pine = Pine(timeout=2.0)
                    serial = pine.game_id()
                    g = GAMES.get(serial)
                    if g:
                        base, stride, slot_addr, st, menu, ovl, eye, tgt = g
                        b = base + pine.read8(slot_addr) * stride
                        e = [f32(pine.read32(eye + i * 4)) for i in range(3)]
                        q = [f32(pine.read32(tgt + i * 4)) for i in range(3)]
                        d = [q[i] - e[i] for i in range(3)]
                        n = math.sqrt(sum(v * v for v in d)) or 1.0
                        csv.write("%.2f,%s,%d,%d,%d,%d,%d,%d,%d,%.1f,%.0f,%.0f,%.0f,%.1f,%.1f\n" % (
                            t, serial, pine.read8(st), pine.read8(menu), pine.read32(ovl), pine.read8(b + 8),
                            pine.read8(b + 0x54F), pine.read16(b + 0x544), pine.read16(b + 0x546),
                            s16(pine.read16(b + 0x92)) * 360.0 / 65536.0,
                            f32(pine.read32(b + 0x38)), f32(pine.read32(b + 0x3C)), f32(pine.read32(b + 0x40)),
                            math.degrees(math.atan2(d[0], d[2])), math.degrees(math.asin(max(-1.0, min(1.0, d[1] / n))))))
                        csv.flush()
                except (PineError, OSError, ConnectionError):
                    pine = None   # emulator still booting, PINE off, or another client connected
            time.sleep(0.25)
    time.sleep(3)
    try:
        with open(EMULOG, encoding="utf-8", errors="replace") as src, open(os.path.join(out, "emulog.txt"), "w", encoding="utf-8") as dst:
            dst.write(src.read())
    except OSError:
        pass
    print("session recorded:", out)


if __name__ == "__main__":
    record(sys.argv[1] if len(sys.argv) > 1 else r"D:\Games\PS2\sessions",
           float(sys.argv[2]) if len(sys.argv) > 2 else 5.0)
