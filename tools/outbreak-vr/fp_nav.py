"""Hold the first-person camera on while running a list of pad steps and screenshots; logs status/room."""
import json, sys, time, os
sys.path.insert(0, os.path.dirname(__file__))
import drive, padd
from pine import Pine
from fp_proto import FirstPerson
p = Pine()
fp = FirstPerson(eye_height=155, eye_forward=30, fov=80, pine=p)
fp.start()
def info():
    base = 0x476DD0 + p.read8(0x48BF7D) * 0x10E0
    return "status=%d room=%d mode=%d patched=%d frames=%d" % (p.read8(0x48BF60), p.read16(base + 0x1E2), p.read8(0x3AEF75), len(fp.patched), fp.frames)
try:
    t0 = time.time()
    while fp.frames == 0 and time.time() - t0 < 10: time.sleep(0.1)
    for step in json.loads(sys.argv[1]):
        if "shot" in step:
            time.sleep(step.get("wait", 0.4)); drive.screenshot(f"D:/Games/PS2/screens/fp-{step['shot']}.png", scale=2)
            print(step["shot"], info())
        elif "sleep" in step:
            time.sleep(step["sleep"]); print("slept", step["sleep"], info())
        else:
            padd.send(step)
finally:
    fp.stop()
