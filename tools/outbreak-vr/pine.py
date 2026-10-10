"""Small PINE client for PenguinScreen2 (PCSX2 fork) on Windows, used to reverse-engineer
Resident Evil Outbreak's memory for the first-person VR camera.

PINE listens on TCP 127.0.0.1:<slot> (default 28011) when [EmuCore] EnablePINE = true.
Wire format: request  = u32 total_len (incl. these 4 bytes) + commands back to back;
             reply    = u32 total_len + u8 result (0 OK, 0xFF fail) + replies back to back.
"""
import socket
import struct
import time

MSG_READ8, MSG_READ16, MSG_READ32, MSG_READ64 = 0x00, 0x01, 0x02, 0x03
MSG_WRITE8, MSG_WRITE16, MSG_WRITE32, MSG_WRITE64 = 0x04, 0x05, 0x06, 0x07
MSG_VERSION, MSG_SAVESTATE, MSG_LOADSTATE = 0x08, 0x09, 0x0A
MSG_TITLE, MSG_ID, MSG_UUID, MSG_GAMEVERSION, MSG_STATUS = 0x0B, 0x0C, 0x0D, 0x0E, 0x0F
MSG_VR_MEMWATCH, MSG_VR_MEMWATCH_POLL = 0xE1, 0xE2

MEMCHECK_READ, MEMCHECK_WRITE, MEMCHECK_WRITE_ONCHANGE = 0x01, 0x02, 0x04
EE_RAM_SIZE = 32 * 1024 * 1024
MAX_REPLY = 450000 - 64  # server's MAX_IPC_RETURN_SIZE, with headroom


class PineError(RuntimeError):
    pass


class Pine:
    def __init__(self, slot=28011, host="127.0.0.1", timeout=15.0):
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
