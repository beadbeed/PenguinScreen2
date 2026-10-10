"""Switch SteamVR to its built-in null (simulated) headset for headset-free VR tests, and back.

  python null_driver.py on    # back up steamvr.vrsettings, then enable the null HMD
  python null_driver.py off   # put the backed-up file back exactly

SteamVR must not be running (it rewrites the file on exit); this script closes it first.
The null HMD needs standby and the dashboard off, or the compositor parks the app.
"""
import json
import os
import shutil
import subprocess
import sys
import time

SETTINGS = r"D:\programfile\steam\config\steamvr.vrsettings"
BACKUP = r"D:\Games\PS2\research\steamvr.vrsettings.before-null-driver"


def close_steamvr():
    for exe in ("vrmonitor.exe", "vrcompositor.exe", "vrserver.exe", "vrdashboard.exe", "vrwebhelper.exe"):
        subprocess.run(["taskkill", "/IM", exe], capture_output=True)
    for _ in range(30):
        out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq vrserver.exe"], capture_output=True, text=True).stdout
        if "vrserver.exe" not in out:
            return
        time.sleep(1)
    subprocess.run(["taskkill", "/F", "/IM", "vrserver.exe"], capture_output=True)
    time.sleep(2)


def on():
    close_steamvr()
    shutil.copy2(SETTINGS, BACKUP)
    d = json.load(open(SETTINGS, encoding="utf-8"))
    d.setdefault("steamvr", {}).update({"forcedDriver": "null", "activateMultipleDrivers": True, "requireHmd": False,
                                        "enableHomeApp": False})
    d["driver_null"] = {"enable": True, "serialNumber": "Null Serial Number", "modelNumber": "Null Model Number",
                        "windowX": 100, "windowY": 100, "windowWidth": 1280, "windowHeight": 720,
                        "renderWidth": 1512, "renderHeight": 1680, "secondsFromVsyncToPhotons": 0.01111111,
                        "displayFrequency": 90.0}
    d.setdefault("power", {}).update({"turnOffScreensTimeout": 86400, "pauseCompositorOnStandby": False})
    d.setdefault("dashboard", {}).update({"enableDashboard": False})
    json.dump(d, open(SETTINGS, "w", encoding="utf-8"), indent=3)
    print("null HMD on; original saved to", BACKUP)


def off():
    close_steamvr()
    if not os.path.exists(BACKUP):
        sys.exit("no backup at " + BACKUP)
    shutil.copy2(BACKUP, SETTINGS)
    print("steamvr.vrsettings restored from", BACKUP)


if __name__ == "__main__":
    {"on": on, "off": off}[sys.argv[1]]()
