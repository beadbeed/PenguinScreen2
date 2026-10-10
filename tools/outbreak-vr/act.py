"""act.py <shot-name> [json-cmd ...] : send pad commands, wait, then screenshot to D:/Games/PS2/screens/re-<name>.png"""
import json, sys, time
sys.path.insert(0, __import__("os").path.dirname(__file__))
import drive, padd
name, cmds = sys.argv[1], [json.loads(a) for a in sys.argv[2:]]
if cmds:
    padd.send(*cmds)
time.sleep(float(__import__("os").environ.get("ACT_WAIT", "1.5")))
print(drive.screenshot(f"D:/Games/PS2/screens/re-{name}.png", scale=int(__import__("os").environ.get("ACT_SCALE", "2"))))
