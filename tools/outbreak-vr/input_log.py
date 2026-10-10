"""Log what pad input reaches Outbreak's local character (File #1) at ~20 Hz, plus a screenshot every
2 s, to see whether VR controller input arrives continuously or drops out.

  python input_log.py <out_dir> [seconds]
"""
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
import drive
from pine import Pine


def main():
    out = sys.argv[1]
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 300
    os.makedirs(out, exist_ok=True)
    p = Pine()
    t0 = time.time()
    last_shot = -10.0
    with open(os.path.join(out, "input.csv"), "w") as f:
        f.write("t,status,stance,action,heading,held,stick_mag,stick_angle,x,z\n")
        while time.time() - t0 < secs:
            t = time.time() - t0
            try:
                base = 0x476DD0 + p.read8(0x48BF7D) * 0x10E0
                status = p.read8(0x48BF60)
                stance, action = p.read8(base + 8), p.read8(base + 0x54F)
                heading = struct.unpack("<h", struct.pack("<H", p.read16(base + 0x92)))[0] * 360 / 65536
                held = p.read32(base + 0xF80)
                mag, ang = p.read16(base + 0xF98), p.read16(base + 0xF9A)
                x = struct.unpack("<f", struct.pack("<I", p.read32(base + 0x38)))[0]
                z = struct.unpack("<f", struct.pack("<I", p.read32(base + 0x40)))[0]
                f.write("%.2f,%d,%d,%d,%.1f,0x%08X,%d,%d,%.0f,%.0f\n" % (t, status, stance, action, heading, held, mag, ang, x, z))
                f.flush()
                if t - last_shot >= 2.0:
                    drive.screenshot(os.path.join(out, "shot-%05.1f.png" % t), scale=2)
                    last_shot = t
            except Exception as e:
                print("stopped:", e)
                break
            time.sleep(0.05)


if __name__ == "__main__":
    main()
