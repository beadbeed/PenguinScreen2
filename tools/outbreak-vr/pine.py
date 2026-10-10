"""Small PINE client for PenguinScreen2 (PCSX2 fork) on Windows, used to reverse-engineer
Resident Evil Outbreak's memory for the first-person VR camera.

PINE listens on TCP 127.0.0.1:<slot> (default 28011) when [EmuCore] EnablePINE = true.
Wire format: request  = u32 total_len (incl. these 4 bytes) + commands back to back;
             reply    = u32 total_len + u8 result (0 OK, 0xFF fail) + replies back to back.

Fork extensions for headset-free VR tests: fake_input() / FakeController drive the real VR input path
with scripted controllers (only when the emulator was started with PCSX2_VR_TEST_INPUT=1, and refused
with online-safe or the network adapter on); telemetry() reads what the first-person camera did.
"""
import math
import socket
import struct
import threading
import time

MSG_READ8, MSG_READ16, MSG_READ32, MSG_READ64 = 0x00, 0x01, 0x02, 0x03
MSG_WRITE8, MSG_WRITE16, MSG_WRITE32, MSG_WRITE64 = 0x04, 0x05, 0x06, 0x07
MSG_VERSION, MSG_SAVESTATE, MSG_LOADSTATE = 0x08, 0x09, 0x0A
MSG_TITLE, MSG_ID, MSG_UUID, MSG_GAMEVERSION, MSG_STATUS = 0x0B, 0x0C, 0x0D, 0x0E, 0x0F
MSG_VR_MEMWATCH, MSG_VR_MEMWATCH_POLL = 0xE1, 0xE2
MSG_VR_FAKE_INPUT, MSG_VR_TELEMETRY = 0xE3, 0xE4

MEMCHECK_READ, MEMCHECK_WRITE, MEMCHECK_WRITE_ONCHANGE = 0x01, 0x02, 0x04
EE_RAM_SIZE = 32 * 1024 * 1024
MAX_REPLY = 450000 - 64  # server's MAX_IPC_RETURN_SIZE, with headroom

# MsgVRFakeInput button bits, in VRHandState field order (bit 0 first).
FAKE_BUTTONS = ("a", "b", "x", "y", "menu", "thumbstick_click", "dpad_up", "dpad_down", "dpad_left",
                "dpad_right", "bumper", "view")
# MsgVRTelemetry disarm reasons (CameraDriver::DisarmReason).
DISARM_REASONS = ("armed", "no VM", "no profile/camera", "switched off", "VM not running", "no head pose",
                  "profile guard failed")
IDENTITY_POSE = ((0.0, 0.0, 0.0, 1.0), (0.0, 0.0, 0.0))


class PineError(RuntimeError):
    pass


def pose(yaw_deg=0.0, pitch_deg=0.0, pos=(0.0, 0.0, 0.0)):
    """A VR pose ((qx, qy, qz, qw), (x, y, z)) in OpenXR axes (x right, y up, z back): turned yaw_deg
    about +Y (counter-clockwise seen from above, i.e. left) then pitched pitch_deg about +X (up)."""
    y, p = math.radians(yaw_deg) * 0.5, math.radians(pitch_deg) * 0.5
    cy, sy, cp, sp = math.cos(y), math.sin(y), math.cos(p), math.sin(p)
    return (cy * sp, sy * cp, -sy * sp, cy * cp), tuple(float(c) for c in pos)


def zone(side, height, forward):
    """Head-relative position of a profile zone placement (metres: side + right, height + up,
    forward + ahead), for a hand sent with head_relative=True."""
    return (float(side), float(height), -float(forward))


def fake_hand(aim=None, grip=None, head_relative=False, trigger=0.0, squeeze=0.0, stick=(0.0, 0.0), buttons=()):
    """One scripted hand for fake_input(): aim/grip are poses (None = not tracked); head_relative places
    them from the head's position and floor yaw, like the profile's zones; squeeze is the grip axis;
    buttons are names from FAKE_BUTTONS."""
    for b in buttons:
        if b not in FAKE_BUTTONS:
            raise ValueError("unknown button %r (one of %s)" % (b, ", ".join(FAKE_BUTTONS)))
    return dict(aim=aim, grip=grip, head_relative=bool(head_relative), trigger=float(trigger),
                squeeze=float(squeeze), stick=(float(stick[0]), float(stick[1])), buttons=tuple(buttons))


class Pine:
    """One connection; PINE serves a single client at a time, so share this object (it is thread-safe)
    instead of opening a second one."""

    def __init__(self, slot=28011, host="127.0.0.1", timeout=15.0):
        self.lock = threading.Lock()
        self.sock = socket.create_connection((host, slot), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def close(self):
        self.sock.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def _recv_exact(self, n):
        buf = bytearray()
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise PineError("connection closed")
            buf += chunk
        return bytes(buf)

    def _transact(self, payload):
        with self.lock:
            return self._transact_locked(payload)

    def _transact_locked(self, payload):
        self.sock.sendall(struct.pack("<I", len(payload) + 4) + payload)
        total = struct.unpack("<I", self._recv_exact(4))[0]
        body = self._recv_exact(total - 4)
        if body[0] != 0:
            raise PineError("PINE command failed")
        return body[1:]

    # --- basic ops -------------------------------------------------------
    def read8(self, a):
        return self._transact(struct.pack("<BI", MSG_READ8, a))[0]

    def read16(self, a):
        return struct.unpack("<H", self._transact(struct.pack("<BI", MSG_READ16, a)))[0]

    def read32(self, a):
        return struct.unpack("<I", self._transact(struct.pack("<BI", MSG_READ32, a)))[0]

    def readf(self, a):
        return struct.unpack("<f", struct.pack("<I", self.read32(a)))[0]

    def write32(self, a, v):
        self._transact(struct.pack("<BII", MSG_WRITE32, a, v & 0xFFFFFFFF))

    def writef(self, a, f):
        self.write32(a, struct.unpack("<I", struct.pack("<f", f))[0])

    def read_block(self, addr, size):
        """Read [addr, addr+size) with batched 64-bit reads. addr/size must be 8-aligned."""
        assert addr % 8 == 0 and size % 8 == 0
        out = bytearray()
        per_batch = MAX_REPLY // 8
        a, end = addr, addr + size
        while a < end:
            n = min(per_batch, (end - a) // 8)
            payload = b"".join(struct.pack("<BI", MSG_READ64, a + i * 8) for i in range(n))
            out += self._transact(payload)
            a += n * 8
        return bytes(out)

    def dump_ram(self):
        return self.read_block(0, EE_RAM_SIZE)

    def status(self):
        return {0: "running", 1: "paused", 2: "shutdown"}.get(
            struct.unpack("<I", self._transact(bytes([MSG_STATUS])))[0], "?")

    def _string(self, op):
        body = self._transact(bytes([op]))
        n = struct.unpack("<I", body[:4])[0]
        return body[4:4 + n].split(b"\0", 1)[0].decode("utf-8", "replace")

    def title(self):
        return self._string(MSG_TITLE)

    def game_id(self):
        return self._string(MSG_ID)

    def game_version(self):
        return self._string(MSG_GAMEVERSION)

    def save_state(self, slot):
        self._transact(struct.pack("<BB", MSG_SAVESTATE, slot))

    def load_state(self, slot):
        self._transact(struct.pack("<BB", MSG_LOADSTATE, slot))

    # --- MemWatch (fork extension) ---------------------------------------
    def memwatch_arm(self, addr, size=4, cond=MEMCHECK_WRITE, stop=False):
        body = self._transact(struct.pack("<BIBBB", MSG_VR_MEMWATCH, addr, size, cond, 1 if stop else 0))
        return body[0]

    def memwatch_clear(self):
        self._transact(struct.pack("<BIBBB", MSG_VR_MEMWATCH, 0, 0, 0, 0))

    def memwatch_poll(self):
        body = self._transact(bytes([MSG_VR_MEMWATCH_POLL]))
        size = struct.unpack("<I", body[:4])[0]
        data = body[4:4 + size]
        count, dropped = struct.unpack("<II", data[:8])
        hits = []
        for i in range(count):
            row = data[8 + i * 280: 8 + (i + 1) * 280]
            pc, op, nhits, frame, watch, is_write = struct.unpack("<IIIIBB", row[:18])
            gpr = struct.unpack("<32Q", row[24:24 + 256])
            hits.append(dict(pc=pc, op=op, hits=nhits, frame=frame, watch=watch, is_write=bool(is_write), gpr=gpr))
        return hits, dropped

    # --- VR test input and telemetry (fork extension) ---------------------
    def fake_input(self, head=IDENTITY_POSE, hands=(None, None), ttl_ms=400, recenter=False):
        """Scripted VR controllers (MsgVRFakeInput, version 1). head: a pose, or None to keep the real
        head. hands: (left, right), each None (the real hand stays) or a dict from fake_hand(). The input
        lapses after ttl_ms (the emulator caps it at 500 ms; 0 drops it now), so keep re-sending
        (FakeController does). recenter: recenter the view on this head once it is applied.
        Raises PineError when refused: PCSX2_VR_TEST_INPUT not set, or online-safe / network adapter on."""
        def pack_pose(p):
            q, pos = p if p is not None else IDENTITY_POSE
            return struct.pack("<7f", q[0], q[1], q[2], q[3], pos[0], pos[1], pos[2])

        flags = (0x01 if head is not None else 0) | (0x02 if recenter else 0)
        payload = struct.pack("<BBBH", MSG_VR_FAKE_INPUT, 1, flags, max(0, min(int(ttl_ms), 0xFFFF))) + pack_pose(head)
        for h in hands:
            if h is None:
                payload += struct.pack("<B", 0) + pack_pose(None) + pack_pose(None) + struct.pack("<4fI", 0, 0, 0, 0, 0)
                continue
            hand_flags = (0x08 | (0x01 if h["aim"] is not None else 0) | (0x02 if h["grip"] is not None else 0) |
                          (0x04 if h["head_relative"] else 0))
            bits = 0
            for name in h["buttons"]:
                bits |= 1 << FAKE_BUTTONS.index(name)
            payload += (struct.pack("<B", hand_flags) + pack_pose(h["aim"]) + pack_pose(h["grip"]) +
                        struct.pack("<4fI", h["trigger"], h["squeeze"], h["stick"][0], h["stick"][1], bits))
        return self._transact(payload)[0] == 1

    def telemetry(self):
        """What the first-person camera saw on its last vsync (MsgVRTelemetry). Angles in degrees."""
        body = self._transact(bytes([MSG_VR_TELEMETRY]))
        size = struct.unpack("<I", body[:4])[0]
        data = body[4:4 + size]
        if len(data) < 36 or data[0] < 1:
            raise PineError("unexpected telemetry reply (%d bytes)" % len(data))
        flags, reason = data[1], data[2]
        guard_fail, yaw_anchor, base_yaw, match_age, frames, vsync, prediction = struct.unpack_from("<IffIIQf", data, 4)
        return dict(version=data[0], armed=bool(flags & 0x01), look_at_active=bool(flags & 0x02),
                    pause_when_hit=bool(flags & 0x04), weapon_raised=bool(flags & 0x08), aim_moving=bool(flags & 0x10),
                    yaw_anchor_valid=bool(flags & 0x20), online_safe=bool(flags & 0x40), test_head=bool(flags & 0x80),
                    disarm_reason=DISARM_REASONS[reason] if reason < len(DISARM_REASONS) else "reason %d" % reason,
                    guard_fail_vsyncs=guard_fail, yaw_anchor=math.degrees(yaw_anchor), base_yaw_now=math.degrees(base_yaw),
                    match_age=match_age, frames_matched=frames, vsync=vsync, prediction_ms=prediction)


class FakeController:
    """Scripted VR controllers kept alive: re-sends the current state at rate_hz (the emulator drops
    test input it has not heard about for ttl_ms) and sends at once on every change. The first send
    recenters the view on the scripted head (recenter=False to skip). Shares the Pine object.

        with FakeController(p) as fc:
            fc.set_hand(1, grip=pose(pos=zone(0.22, -0.50, 0.12)), head_relative=True, squeeze=1.0)
            time.sleep(0.5)
    """

    def __init__(self, pine, head=IDENTITY_POSE, rate_hz=20.0, ttl_ms=400, recenter=True):
        self.pine = pine
        self.head = head
        self.hands = [None, None]
        self.period = 1.0 / rate_hz
        self.ttl_ms = ttl_ms
        self.error = None  # the error that stopped the re-send thread, if any
        self._recenter = recenter
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread = None

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *exc):
        self.stop()

    def _send(self):
        with self._lock:
            head, hands, recenter = self.head, tuple(self.hands), self._recenter
            self._recenter = False
        self.pine.fake_input(head=head, hands=hands, ttl_ms=self.ttl_ms, recenter=recenter)

    def _run(self):
        while not self._stop.wait(self.period):
            try:
                self._send()
            except (PineError, OSError) as e:
                self.error = e
                return

    def start(self):
        """Sends once (raising PineError if refused), then keeps re-sending in the background."""
        self._send()
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self):
        """Stops re-sending and drops the test input, so the real controllers are back at once."""
        self._stop.set()
        if self._thread:
            self._thread.join()
            self._thread = None
        try:
            self.pine.fake_input(head=None, hands=(None, None), ttl_ms=0)
        except (PineError, OSError):
            pass

    def set_head(self, head):
        with self._lock:
            self.head = head
        self._send()

    def set_hand(self, hand, **kw):
        """hand 0 = left, 1 = right; keywords as fake_hand()."""
        h = fake_hand(**kw)
        with self._lock:
            self.hands[hand] = h
        self._send()

    def clear_hand(self, hand):
        """The hand goes back to its real state (none on the null headset)."""
        with self._lock:
            self.hands[hand] = None
        self._send()


def wait_for_pine(slot=28011, seconds=60):
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            return Pine(slot)
        except OSError:
            time.sleep(1)
    raise PineError("PINE not reachable on 127.0.0.1:%d (EnablePINE on? game running?)" % slot)


if __name__ == "__main__":
    with wait_for_pine(seconds=5) as p:
        print("status:", p.status(), "| id:", p.game_id(), "| title:", p.title())
        t = time.time()
        ram = p.dump_ram()
        print("dumped %d bytes in %.2fs" % (len(ram), time.time() - t))
