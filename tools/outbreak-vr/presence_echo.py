"""VR presence's offline loopback partner ([VR] VrPresence; research/2026-10-21-vr-presence.md).

Receives the emulator's presence packets on 127.0.0.1 and sends each one back as a fake partner: the same poses with
the slot rewritten to an AI partner's (so the emulator draws them at that partner's character), a session and sequence
of its own, and a fresh checksum. Loopback only: it binds 127.0.0.1 and talks to 127.0.0.1, never anything else.

    python -I presence_echo.py [--listen 47801] [--to 47800] [--slot 1] [--key ""] [--seconds 0]

The emulator needs [VR] VrPresence = true, VrPresencePeer = 127.0.0.1:47801 (this script's --listen),
VrPresencePort = 47800 (--to) and PCSX2_VR_PRESENCE_LOOPBACK=1 in its environment. Also importable
(presence_test.py runs Echo in a thread)."""
import argparse
import math
import random
import socket
import struct
import sys
import threading
import time

MAGIC = 0x31505256  # "VRP1"
VERSION = 1
SIZE = 156
# magic, version, flags, slot, pad, room, grip0, grip1, key, session, seq, send_ms, character, head, left, right
# (each pos, fwd, up), trigger0, trigger1, pad
BODY = struct.Struct("<IBBBBHBBIIII3f9f9f9fBBH")
assert BODY.size == SIZE - 4


def fnv1a(data):
    h = 2166136261
    for b in data:
        h ^= b
        h = (h * 16777619) & 0xFFFFFFFF
    return h


def decode(data):
    """A packet as a dict, or None (wrong size, magic, version or checksum)."""
    if len(data) != SIZE or struct.unpack_from("<I", data, SIZE - 4)[0] != fnv1a(data[:SIZE - 4]):
        return None
    f = BODY.unpack_from(data)
    if f[0] != MAGIC or f[1] != VERSION:
        return None
    p = dict(flags=f[2], slot=f[3], room=f[5], grip=(f[6], f[7]), key=f[8], session=f[9], seq=f[10], send_ms=f[11],
             character=f[12:15], trigger=(f[42], f[43]))
    for i, name in enumerate(("head", "left", "right")):
        v = f[15 + 9 * i: 24 + 9 * i]
        p[name] = dict(pos=v[0:3], fwd=v[3:6], up=v[6:9])
    return p


def encode(p):
    vals = [MAGIC, VERSION, p["flags"], p["slot"], 0, p["room"], p["grip"][0], p["grip"][1], p["key"], p["session"],
            p["seq"], p["send_ms"]] + list(p["character"])
    for name in ("head", "left", "right"):
        vals += list(p[name]["pos"]) + list(p[name]["fwd"]) + list(p[name]["up"])
    vals += [p["trigger"][0], p["trigger"][1], 0]
    body = BODY.pack(*vals)
    return body + struct.pack("<I", fnv1a(body))


def key_hash(text):
    return fnv1a(text.encode("utf-8"))


class Echo:
    """The fake partner. slot: the AI partner's slot to claim; key: overrides the key hash (None: send back the
    emulator's own). stats: counts and the last packet received."""

    def __init__(self, listen=47801, to=47800, slot=1, key=None):
        self.to = ("127.0.0.1", to)
        self.slot = slot
        self.key = key
        self.session = random.getrandbits(32)
        self.seq = 0
        self.stats = dict(received=0, bad=0, sent=0, foreign=0)
        self.last = None
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", listen))
        self.sock.settimeout(0.2)
        self._stop = threading.Event()
        self._thread = None
        self.paused = False

    def step(self):
        try:
            data, addr = self.sock.recvfrom(2048)
        except (socket.timeout, ConnectionResetError):
            return
        if addr[0] != "127.0.0.1":
            self.stats["foreign"] += 1
            return
        p = decode(data)
        if p is None:
            self.stats["bad"] += 1
            return
        self.stats["received"] += 1
        self.last = p
        if self.paused:
            return
        q = dict(p)
        q["slot"] = self.slot
        q["session"] = self.session
        self.seq = (self.seq + 1) & 0xFFFFFFFF
        q["seq"] = self.seq
        if self.key is not None:
            q["key"] = self.key
        self.sock.sendto(encode(q), self.to)
        self.stats["sent"] += 1

    def run(self, seconds=0.0):
        end = time.time() + seconds if seconds else None
        while not self._stop.is_set() and (end is None or time.time() < end):
            self.step()

    def start(self):
        self._thread = threading.Thread(target=self.run, daemon=True)
        self._thread.start()
        return self

    def stop(self):
        self._stop.set()
        if self._thread:
            self._thread.join()
        self.sock.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--listen", type=int, default=47801)
    ap.add_argument("--to", type=int, default=47800)
    ap.add_argument("--slot", type=int, default=1)
    ap.add_argument("--key", default=None, help="send this word's hash instead of the emulator's own")
    ap.add_argument("--seconds", type=float, default=0.0, help="0: until Ctrl+C")
    a = ap.parse_args()
    e = Echo(a.listen, a.to, a.slot, None if a.key is None else key_hash(a.key))
    print("echoing 127.0.0.1:%d -> 127.0.0.1:%d as slot %d" % (a.listen, a.to, a.slot))
    last = time.time()
    try:
        end = time.time() + a.seconds if a.seconds else None
        while end is None or time.time() < end:
            e.step()
            if time.time() - last > 2.0 and e.last:
                last = time.time()
                h = e.last["head"]["pos"]
                print("%s | slot %d room %d head game (%.1f, %.1f, %.1f) seq %d" % (
                    e.stats, e.last["slot"], e.last["room"], h[0], h[1], h[2], e.last["seq"]))
    except KeyboardInterrupt:
        pass
    e.stop()


if __name__ == "__main__":
    main()
