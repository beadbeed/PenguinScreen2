"""Screenshot SteamVR's null-driver 'Headset Window' (what the simulated headset shows)."""
import ctypes, ctypes.wintypes as wt, os, sys
sys.path.insert(0, os.path.dirname(__file__))
import drive
user32 = ctypes.WinDLL("user32")


def headset_window():
    h = user32.FindWindowW(None, "Headset Window")
    if not h:
        raise RuntimeError("Headset Window not found (null driver not running?)")
    return h


def shot(path, scale=2, background=True):
    return drive.screenshot(path, hwnd=headset_window(), scale=scale, background=background)


if __name__ == "__main__":
    print(shot(sys.argv[1] if len(sys.argv) > 1 else "D:/Games/PS2/screens/hmd.png"))
