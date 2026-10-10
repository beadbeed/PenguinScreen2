"""Keeps one virtual pad plugged in and takes commands over 127.0.0.1:28100 (one JSON object
per line), so scripts can play without hot-plugging the pad each time.

  {"op": "press", "b": "start", "ms": 150}
  {"op": "hold", "b": ["r1"], "ms": 800, "lx": 0, "ly": 1, "rx": 0, "ry": 0}
  {"op": "quit"}

Client: send(...) below, or `python padd.py send '{"op":"press","b":"cross"}'`.
"""
import json
import socket
import sys
import time

PORT = 28100


def serve():
    from vpad import VPad
    pad = VPad()
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", PORT))
    srv.listen(4)
    print("padd: virtual pad up, listening on", PORT, flush=True)
    try:
        while True:
            conn, _ = srv.accept()
            with conn, conn.makefile("rw") as f:
                for line in f:
                    cmd = json.loads(line)
                    op = cmd.get("op")
                    if op == "quit":
                        f.write("bye\n")
                        f.flush()
                        return
                    b = cmd.get("b", [])
                    b = [b] if isinstance(b, str) else b
                    if op == "press":
                        for name in b:
                            pad.press(name, cmd.get("ms", 120), cmd.get("gap", 150))
                    elif op == "hold":
                        pad.hold(b, cmd.get("ms", 300), cmd.get("lx", 0.0), cmd.get("ly", 0.0),
                                 cmd.get("rx", 0.0), cmd.get("ry", 0.0))
                    elif op == "sleep":
                        time.sleep(cmd.get("s", 1.0))
                    f.write("ok\n")
                    f.flush()
    finally:
        pad.close()
        srv.close()


def send(*cmds, timeout=120):
    with socket.create_connection(("127.0.0.1", PORT), timeout=timeout) as s, s.makefile("rw") as f:
        for c in cmds:
            f.write(json.dumps(c) + "\n")
            f.flush()
            f.readline()


if __name__ == "__main__":
    if len(sys.argv) > 2 and sys.argv[1] == "send":
        send(*[json.loads(a) for a in sys.argv[2:]])
    else:
        serve()
