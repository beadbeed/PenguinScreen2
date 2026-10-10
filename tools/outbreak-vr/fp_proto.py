"""First-person camera prototype over PINE (File #1, flat, no headset): NOP the room camera's cut
handlers and the door follow camera's eye writes, then keep writing eye/target at the local
character's head along its heading, the same maths as camera.lookAt in CameraDriver.cpp.
Always restores the original code words on exit.

  python fp_proto.py [seconds] [eye_height] [fov]
"""
import math
import os
import struct
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(__file__))
from pine import MSG_READ16, MSG_READ32, MSG_WRITE32, Pine

SILENCE = [(0x58EAEC, 0x0C163B1C), (0x58EAFC, 0x0C163C8C), (0x58EB0C, 0x0C163B60),
           (0x5A91E0, 0x0C0D013C), (0x5A9340, 0x0C0D013C), (0x5A9410, 0x0C0D013C)]
EYE, TARGET, FOV = 0x306338, 0x306344, 0x306380
PLAYERS, STRIDE, LOCAL_IDX = 0x476DD0, 0x10E0, 0x48BF7D
GAME_OVL, STATUS, CAM_MODE = 0x540004, 0x48BF60, 0x3AEF75

f2u = lambda f: struct.unpack("<I", struct.pack("<f", f))[0]
u2f = lambda u: struct.unpack("<f", struct.pack("<I", u))[0]


class FirstPerson:
    def __init__(self, eye_height=155.0, eye_forward=10.0, fov=80.0, distance=100.0, pine=None):
        self.p = pine or Pine()
        self.eye_height, self.eye_forward, self.fov, self.distance = eye_height, eye_forward, fov, distance
        self.head_yaw = 0.0   # radians, CCW positive (what the headset would add)
        self.head_pitch = 0.0
        self.running = False
        self.patched = []
        self.frames = 0
        self.skipped = 0

    def guards_ok(self):
        return (self.p.read32(GAME_OVL) == 3 and self.p.read8(STATUS) == 2 and self.p.read8(CAM_MODE) == 0)

    def patch(self):
        for addr, orig in SILENCE:
            cur = self.p.read32(addr)
            if cur == orig:
                self.p.write32(addr, 0)
                self.patched.append((addr, orig))
            elif cur != 0:
                raise RuntimeError("0x%06X holds 0x%08X, expected 0x%08X; not touching anything" % (addr, cur, orig))
        print("silenced %d camera code words" % len(self.patched))

    def restore(self):
        for addr, orig in self.patched:
            if self.p.read32(addr) == 0:
                self.p.write32(addr, orig)
        print("restored %d camera code words" % len(self.patched))
        self.patched = []

    def step(self):
        idx = self.p.read8(LOCAL_IDX)
        base = PLAYERS + idx * STRIDE
        body = self.p._transact(struct.pack("<BIBIBIBI", MSG_READ32, base + 0x38, MSG_READ32, base + 0x3C,
                                            MSG_READ32, base + 0x40, MSG_READ16, base + 0x92))
        x, y, z = (u2f(v) for v in struct.unpack("<III", body[:12]))
        h = struct.unpack("<h", body[12:14])[0] * (2 * math.pi / 65536)
        yaw, pitch = h + self.head_yaw, self.head_pitch
        sy, cy, sp, cp = math.sin(yaw), math.cos(yaw), math.sin(pitch), math.cos(pitch)
        eye = (x + self.eye_forward * sy, y + self.eye_height, z + self.eye_forward * cy)
        tgt = (eye[0] + self.distance * sy * cp, eye[1] + self.distance * sp, eye[2] + self.distance * cy * cp)
        payload = b"".join(struct.pack("<BII", MSG_WRITE32, a, f2u(v)) for a, v in
                           zip([EYE, EYE + 4, EYE + 8, TARGET, TARGET + 4, TARGET + 8, FOV], [*eye, *tgt, self.fov]))
        self.p._transact(payload)
        self.frames += 1
        return (x, y, z, h)

    def loop(self, hz=120):
        self.running = True
        period = 1.0 / hz
        while self.running:
            t0 = time.perf_counter()
            try:
                if self.guards_ok():
                    if not self.patched:
                        self.patch()
                    self.last = self.step()
                else:
                    self.skipped += 1
                    if self.patched:
                        self.restore()
            except Exception as e:  # keep going; report once
                print("loop error:", e)
                time.sleep(0.2)
            dt = time.perf_counter() - t0
            if dt < period:
                time.sleep(period - dt)

    def start(self):
        self.thread = threading.Thread(target=self.loop, daemon=True)
        self.thread.start()

    def stop(self):
        self.running = False
        self.thread.join(timeout=2)
        self.restore()


if __name__ == "__main__":
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 20
    fp = FirstPerson(eye_height=float(sys.argv[2]) if len(sys.argv) > 2 else 155.0,
                     fov=float(sys.argv[3]) if len(sys.argv) > 3 else 80.0)
    fp.start()
    try:
        time.sleep(secs)
    finally:
        fp.stop()
        print("frames written:", fp.frames, "skipped (guards):", fp.skipped)
