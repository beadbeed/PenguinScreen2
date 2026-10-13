"""Small PINE client for PenguinScreen2 (PCSX2 fork), used to reverse-engineer
Resident Evil Outbreak's memory for the first-person VR camera.

PINE serves when [EmuCore] EnablePINE = true and a game is running. Where it listens depends on the emulator's
platform (pcsx2/PINE.cpp):
  - Windows: TCP 127.0.0.1:<slot>, default slot 28011.
  - Linux: a Unix socket, $XDG_RUNTIME_DIR/pcsx2.sock (/tmp/pcsx2.sock when XDG_RUNTIME_DIR is unset), with
    ".<slot>" added when the slot is not 28011 (slot 28012: pcsx2.sock.28012). macOS: the same name in $TMPDIR.
This client picks the same one for the platform it runs on (on Windows that is the TCP port, as it always was). To
reach another machine's PINE, or to override:
  PINE_SOCKET=/path/to/pcsx2.sock   a Unix socket at that path
  PINE_TCP=1 (or PINE_HOST=<host>)  TCP <host>:<slot> (host 127.0.0.1 unless PINE_HOST says otherwise)
Reaching a Linux machine's socket from the PC: forward a local TCP port to it and use TCP,
  ssh -N -L 28011:/run/user/1000/pcsx2.sock user@machine
(TCP is the default on Windows).
Wire format: request  = u32 total_len (incl. these 4 bytes) + commands back to back;
             reply    = u32 total_len + u8 result (0 OK, 0xFF fail) + replies back to back.

Fork extensions for headset-free VR tests: fake_input() / FakeController drive the real VR input path
with scripted controllers (only when the emulator was started with PCSX2_VR_TEST_INPUT=1, and refused
with online-safe or the network adapter on); telemetry() reads what the first-person camera did;
menu_pointer() reads the menu pointer and point-to-select, and set_test_screen_hit() points it without
an OpenXR runtime (refused like fake_input()); pouches() reads the belt pouches.
"""
import math
import os
import re
import socket
import struct
import sys
import threading
import time

MSG_READ8, MSG_READ16, MSG_READ32, MSG_READ64 = 0x00, 0x01, 0x02, 0x03
MSG_WRITE8, MSG_WRITE16, MSG_WRITE32, MSG_WRITE64 = 0x04, 0x05, 0x06, 0x07
MSG_VERSION, MSG_SAVESTATE, MSG_LOADSTATE = 0x08, 0x09, 0x0A
MSG_TITLE, MSG_ID, MSG_UUID, MSG_GAMEVERSION, MSG_STATUS = 0x0B, 0x0C, 0x0D, 0x0E, 0x0F
MSG_VR_MEMWATCH, MSG_VR_MEMWATCH_POLL = 0xE1, 0xE2
MSG_VR_FAKE_INPUT, MSG_VR_TELEMETRY = 0xE3, 0xE4
MSG_VR_MENU_POINTER = 0xE5

MEMCHECK_READ, MEMCHECK_WRITE, MEMCHECK_WRITE_ONCHANGE = 0x01, 0x02, 0x04
EE_RAM_SIZE = 32 * 1024 * 1024
MAX_REPLY = 450000 - 64  # server's MAX_IPC_RETURN_SIZE, with headroom

# MsgVRFakeInput button bits, in VRHandState field order (bit 0 first).
FAKE_BUTTONS = ("a", "b", "x", "y", "menu", "thumbstick_click", "dpad_up", "dpad_down", "dpad_left",
                "dpad_right", "bumper", "view")
# MsgVRTelemetry disarm reasons (CameraDriver::DisarmReason).
DISARM_REASONS = ("armed", "no VM", "no profile/camera", "switched off", "VM not running", "no head pose",
                  "profile guard failed", "online, restricted (not private co-op)")
IDENTITY_POSE = ((0.0, 0.0, 0.0, 1.0), (0.0, 0.0, 0.0))


class PineError(RuntimeError):
    pass


DEFAULT_SLOT = 28011


def unix_socket_path(slot=DEFAULT_SLOT):
    """Where a Linux or macOS emulator's PINE listens for this slot (the rule in pcsx2/PINE.cpp)."""
    if sys.platform == "darwin":
        runtime_dir = os.environ.get("TMPDIR")
    else:
        runtime_dir = os.environ.get("XDG_RUNTIME_DIR")
    path = (runtime_dir + "/pcsx2.sock") if runtime_dir else "/tmp/pcsx2.sock"  # as PINE.cpp builds it
    if int(slot) != DEFAULT_SLOT:
        path += ".%d" % int(slot)
    return path


def endpoint(slot=DEFAULT_SLOT, host=None, path=None):
    """("unix", path) or ("tcp", (host, slot)): what Pine() connects to. An explicit path or host wins, then PINE_SOCKET,
    then PINE_TCP / PINE_HOST, then the platform's own transport (TCP on Windows, the Unix socket elsewhere)."""
    if path:
        return "unix", path
    if host:
        return "tcp", (host, int(slot))
    if os.environ.get("PINE_SOCKET"):
        return "unix", os.environ["PINE_SOCKET"]
    if os.environ.get("PINE_HOST") or os.environ.get("PINE_TCP", "") not in ("", "0"):
        return "tcp", (os.environ.get("PINE_HOST") or "127.0.0.1", int(slot))
    if os.name == "nt" or not hasattr(socket, "AF_UNIX"):
        return "tcp", ("127.0.0.1", int(slot))
    return "unix", unix_socket_path(slot)


def describe_endpoint(ep):
    kind, where = ep
    return ("unix socket %s" % where) if kind == "unix" else ("%s:%d" % where)


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

    def __init__(self, slot=DEFAULT_SLOT, host=None, timeout=15.0, path=None):
        """slot: the PINE slot ([EmuCore] PINESlot). host: connect over TCP to host:slot. path: connect to the Unix
        socket at path. With neither, the platform's own transport (see endpoint())."""
        self.lock = threading.Lock()
        self.endpoint = endpoint(slot, host, path)
        kind, where = self.endpoint
        if kind == "unix":
            self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.sock.settimeout(timeout)
            try:
                self.sock.connect(where)
            except OSError:
                self.sock.close()
                raise
        else:
            self.sock = socket.create_connection(where, timeout=timeout)
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
        lapses after ttl_ms (the emulator caps it at 1000 ms; 0 drops it now), so keep re-sending
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

    def menu_pointer(self):
        """The menu pointer's state (MsgVRMenuPointer op 0, read-only, allowed any time): the flat screen quad it aims at
        (centre xyz and quaternion xyzw in the XR base space, half extents in metres), the pointer's hit on it (u, v as
        fractions of the game picture), point-to-select's cells (indices into the profile's menuPointer.cells, -1 none) and
        its counters."""
        body = self._transact(struct.pack("<BB", MSG_VR_MENU_POINTER, 0))
        size = struct.unpack("<I", body[:4])[0]
        data = body[4:4 + size]
        if len(data) < 84 or data[0] < 1:
            raise PineError("unexpected menu pointer reply (%d bytes)" % len(data))
        (ver, flags, flags2, hovered, current, target, presses_target, dpad) = struct.unpack_from("<BBBbbbBB", data, 0)
        u, v, distance = struct.unpack_from("<3f", data, 8)
        centre = struct.unpack_from("<3f", data, 20)
        quat = struct.unpack_from("<4f", data, 32)
        half_width, half_height = struct.unpack_from("<2f", data, 48)
        node, presses, arrivals, aborts, ticks, passed, swallowed = struct.unpack_from("<7I", data, 56)
        return dict(version=ver, transform_valid=bool(flags & 0x01), hit_valid=bool(flags & 0x02),
                    hit_right=bool(flags & 0x04), menu_mode=bool(flags & 0x08), gate=bool(flags & 0x10),
                    in_flight=bool(flags & 0x20), confirm_held=bool(flags & 0x40), suspended=bool(flags & 0x80),
                    tablet=bool(flags2 & 0x01), test_hit=bool(flags2 & 0x02), selfcheck_failed=bool(flags2 & 0x04),
                    refused_online=bool(flags2 & 0x08), hovered=hovered, current=current, target=target,
                    presses_target=presses_target, dpad=None if dpad == 0xFF else dpad, u=u, v=v, distance=distance,
                    centre=centre, quat=quat, half_width=half_width, half_height=half_height, node=node,
                    presses=presses, arrivals=arrivals, aborts=aborts, ticks=ticks, confirms_passed=passed,
                    confirms_swallowed=swallowed)

    def set_test_screen_hit(self, hand, u, v, ttl_ms=400):
        """A scripted menu-pointer hit (MsgVRMenuPointer op 1) at (u, v), fractions of the game picture, for hand 0 (left)
        or 1 (right); it lapses after ttl_ms (capped at 1000 ms), so keep re-sending. u below 0 (or ttl_ms 0) clears it.
        Lets point-to-select be tested with no OpenXR runtime. Raises PineError when refused, as fake_input() is."""
        payload = struct.pack("<BBBBHff", MSG_VR_MENU_POINTER, 1, 1, int(hand) & 0xFF, max(0, min(int(ttl_ms), 0xFFFF)),
                              float(u), float(v))
        return self._transact(payload)[0] == 1

    def pouches(self):
        """The belt pouches (MsgVRMenuPointer op 2, read-only, allowed any time): whether they run (first person armed with
        a pouches profile), the pouch each hand is in (-1 none), each pouch's centre in the XR base space (where the hands
        are, so a scripted grip pose can be put there as is), radius, menuPointer cell and item state (1 holds an item, 0
        empty, -1 not read), the menu pointer's waiting pouch request (-1 none) and the counters: reaches (ticks),
        item_ticks (of which double ticks), squeezes, opens (open presses through the open zone), requests (sent to the
        menu pointer), refused (squeezes that did nothing), and the menu pointer's mp_requests / mp_started / mp_refused /
        mp_lapsed."""
        body = self._transact(struct.pack("<BB", MSG_VR_MENU_POINTER, 2))
        size = struct.unpack("<I", body[:4])[0]
        data = body[4:4 + size]
        if len(data) < 208 or data[0] < 1:
            raise PineError("unexpected pouches reply (%d bytes)" % len(data))
        ver, flags, count, hover_l, hover_r, request = struct.unpack_from("<BBBbbb", data, 0)
        (reaches, item_ticks, squeezes, opens, requests, refused,
         mp_requests, mp_started, mp_refused, mp_lapsed) = struct.unpack_from("<10I", data, 8)
        pouches = []
        for i in range(min(count, 8)):
            x, y, z, radius, cell, item, item_byte = struct.unpack_from("<4fbbB", data, 48 + i * 20)
            pouches.append(dict(centre=(x, y, z), radius=radius, cell=cell, item=item, item_byte=item_byte))
        return dict(version=ver, active=bool(flags & 0x01), screen_up=bool(flags & 0x02), menu_mode=bool(flags & 0x04),
                    head_anchor=bool(flags & 0x08), open_pressing=bool(flags & 0x10), suspended=bool(flags & 0x20),
                    owned=(bool(flags & 0x40), bool(flags & 0x80)), hover=(hover_l, hover_r), request=request,
                    pouches=pouches, reaches=reaches, item_ticks=item_ticks, squeezes=squeezes, opens=opens,
                    requests=requests, refused=refused, mp_requests=mp_requests, mp_started=mp_started,
                    mp_refused=mp_refused, mp_lapsed=mp_lapsed)


class FakeController:
    """Scripted VR controllers kept alive: re-sends the current state at rate_hz (the emulator drops
    test input it has not heard about for ttl_ms) and sends at once on every change. The first send
    recenters the view on the scripted head (recenter=False to skip). Shares the Pine object.

        with FakeController(p) as fc:
            fc.set_hand(1, grip=pose(pos=zone(0.22, -0.50, 0.12)), head_relative=True, squeeze=1.0)
            time.sleep(0.5)
    """

    def __init__(self, pine, head=IDENTITY_POSE, rate_hz=20.0, ttl_ms=900, recenter=True):
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


def wait_for_pine(slot=DEFAULT_SLOT, seconds=60, host=None, path=None):
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            return Pine(slot, host=host, path=path)
        except OSError:
            time.sleep(1)
    raise PineError("PINE not reachable on %s (EnablePINE on? game running?)" %
                    describe_endpoint(endpoint(slot, host, path)))


# The emulator's ini (portable install on the test PC; PCSX2_INI overrides) and each game's network-play flag.
INI = os.environ.get("PCSX2_INI", r"D:\Games\PS2\PenguinScreen2\inis\PenguinScreen2.ini")
NETWORK_FLAG = {"SLPM-65428": 0x3065C2, "SLPM-65692": 0x3137D2}


def network_adapter_on(ini=INI):
    """True if [DEV9/Eth] EthEnable is true in the ini; None if the ini can't be read."""
    try:
        with open(ini, encoding="utf-8", errors="replace") as fh:
            section = None
            for line in fh:
                m = re.match(r"^\[(.+)\]\s*$", line)
                if m:
                    section = m.group(1)
                elif section == "DEV9/Eth" and re.match(r"^\s*EthEnable\s*=\s*true\s*$", line, re.I):
                    return True
        return False
    except OSError:
        return None


def require_offline(p=None):
    """Exit unless the emulator is offline. The test tools pause, load savestates, patch code and send test input;
    none of that may ever happen online (the server drops a client that stalls, and a state load desyncs the room)."""
    if network_adapter_on():
        sys.exit("the network adapter is on (%s): test tools never run online. Close the emulator, run "
                 "netmode.py off, and start it again." % INI)
    if p is not None:
        try:
            flag = NETWORK_FLAG.get(p.game_id())
            if flag is not None and p.read8(flag):
                sys.exit("the game is in network play: test tools never run online.")
        except PineError:
            pass


if __name__ == "__main__":
    print("PINE:", describe_endpoint(endpoint()))
    with wait_for_pine(seconds=5) as p:
        print("status:", p.status(), "| id:", p.game_id(), "| title:", p.title())
        t = time.time()
        ram = p.dump_ram()
        print("dumped %d bytes in %.2fs" % (len(ram), time.time() - t))
