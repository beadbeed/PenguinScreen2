"""Drive the PenguinScreen2 window from a script: focus it, press/hold keys (SendInput
scancodes), and screenshot it. Pad 1 must have Keyboard bindings (see README in this
folder). Windows only; standard library only.
"""
import ctypes
import ctypes.wintypes as wt
import os
import struct
import time
import zlib

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
user32.SetProcessDPIAware()

# PS2 pad button -> keyboard scancode (PCSX2's default keyboard layout, added as extra Pad1 bindings)
SC = {
    "up": 0x48, "down": 0x50, "left": 0x4B, "right": 0x4D,          # arrows (extended)
    "cross": 0x25, "circle": 0x26, "square": 0x24, "triangle": 0x17,  # K L J I
    "start": 0x1C, "select": 0x0E,                                    # Return, Backspace
    "l1": 0x10, "r1": 0x12, "l2": 0x02, "r2": 0x04, "l3": 0x03, "r3": 0x05,  # Q E 1 3 2 4
    "lup": 0x11, "ldown": 0x1F, "lleft": 0x1E, "lright": 0x20,        # W S A D
    "rup": 0x14, "rdown": 0x22, "rleft": 0x21, "rright": 0x23,        # T G F H
}
EXTENDED = {"up", "down", "left", "right"}

INPUT_KEYBOARD = 1
KEYEVENTF_EXTENDEDKEY, KEYEVENTF_KEYUP, KEYEVENTF_SCANCODE = 0x1, 0x2, 0x8


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wt.WORD), ("wScan", wt.WORD), ("dwFlags", wt.DWORD),
                ("time", wt.DWORD), ("dwExtraInfo", ctypes.c_size_t)]


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wt.LONG), ("dy", wt.LONG), ("mouseData", wt.DWORD), ("dwFlags", wt.DWORD),
                ("time", wt.DWORD), ("dwExtraInfo", ctypes.c_size_t)]


class _U(ctypes.Union):
    _fields_ = [("ki", KEYBDINPUT), ("mi", MOUSEINPUT)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wt.DWORD), ("u", _U)]


def _key(name, up):
    flags = KEYEVENTF_SCANCODE | (KEYEVENTF_KEYUP if up else 0) | (KEYEVENTF_EXTENDEDKEY if name in EXTENDED else 0)
    inp = INPUT(type=INPUT_KEYBOARD, u=_U(ki=KEYBDINPUT(0, SC[name], flags, 0, 0)))
    if user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT)) != 1:
        raise OSError(ctypes.get_last_error(), "SendInput failed")


def find_window():
    """Return the HWND of the PenguinScreen2 main window (the one with the largest area)."""
    found = []
    pid = wt.DWORD()

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(hwnd, _):
        if not user32.IsWindowVisible(hwnd):
            return True
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        h = kernel32.OpenProcess(0x1000, False, pid.value)  # PROCESS_QUERY_LIMITED_INFORMATION
        if h:
            buf = ctypes.create_unicode_buffer(512)
            n = wt.DWORD(512)
            if kernel32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n)) and buf.value.lower().endswith("pcsx2-qt.exe"):
                r = wt.RECT()
                user32.GetWindowRect(hwnd, ctypes.byref(r))
                found.append(((r.right - r.left) * (r.bottom - r.top), hwnd))
            kernel32.CloseHandle(h)
        return True

    user32.EnumWindows(cb, 0)
    if not found:
        raise RuntimeError("PenguinScreen2 window not found")
    return max(found)[1]


def focus(hwnd=None):
    hwnd = hwnd or find_window()
    if user32.GetForegroundWindow() == hwnd:
        return hwnd
    user32.ShowWindow(hwnd, 9)  # SW_RESTORE
    # Windows only lets the foreground process change focus; a synthetic ALT tap lifts that lock.
    user32.keybd_event(0x12, 0, 0, 0)
    user32.keybd_event(0x12, 0, 2, 0)
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.15)
    return hwnd


def press(name, ms=120, gap_ms=120):
    focus()
    _key(name, False)
    time.sleep(ms / 1000)
    _key(name, True)
    time.sleep(gap_ms / 1000)


def hold(names, ms):
    """Hold several buttons together for ms milliseconds."""
    focus()
    names = [names] if isinstance(names, str) else list(names)
    for n in names:
        _key(n, False)
    time.sleep(ms / 1000)
    for n in reversed(names):
        _key(n, True)
    time.sleep(0.1)


def seq(*steps):
    """steps: 'cross', ('lup', 800), (['r1','lleft'], 500), or a float = sleep seconds."""
    for s in steps:
        if isinstance(s, (int, float)):
            time.sleep(s)
        elif isinstance(s, str):
            press(s)
        else:
            hold(s[0], s[1])


def send_to_back(hwnd=None):
    """Un-minimise the window without activating it and park it behind every other window,
    so PrintWindow can capture it without covering whatever the user is doing."""
    hwnd = hwnd or find_window()
    if user32.IsIconic(hwnd):
        user32.ShowWindow(hwnd, 4)  # SW_SHOWNOACTIVATE
    user32.SetWindowPos(hwnd, 1, 0, 0, 0, 0, 0x0001 | 0x0002 | 0x0010)  # HWND_BOTTOM, NOSIZE|NOMOVE|NOACTIVATE
    time.sleep(0.3)
    return hwnd


def screenshot(path, hwnd=None, background=True, scale=1):
    """Save the window as a PNG. background=True uses PrintWindow(PW_RENDERFULLCONTENT), which
    works while the window is covered by other windows; False copies the on-screen pixels."""
    hwnd = hwnd or find_window()
    if user32.IsIconic(hwnd):
        send_to_back(hwnd)
    r = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    w, h = r.right - r.left, r.bottom - r.top
    sdc = user32.GetDC(0)
    mdc = gdi32.CreateCompatibleDC(sdc)
    bmp = gdi32.CreateCompatibleBitmap(sdc, w, h)
    gdi32.SelectObject(mdc, bmp)
    if background:
        user32.PrintWindow(hwnd, mdc, 2)  # PW_RENDERFULLCONTENT
    else:
        gdi32.BitBlt(mdc, 0, 0, w, h, sdc, r.left, r.top, 0x00CC0020)  # SRCCOPY

    class BITMAPINFOHEADER(ctypes.Structure):
        _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG), ("biPlanes", wt.WORD),
                    ("biBitCount", wt.WORD), ("biCompression", wt.DWORD), ("biSizeImage", wt.DWORD),
                    ("biXPelsPerMeter", wt.LONG), ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.DWORD),
                    ("biClrImportant", wt.DWORD)]

    bih = BITMAPINFOHEADER(ctypes.sizeof(BITMAPINFOHEADER), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(mdc, bmp, 0, h, buf, ctypes.byref(bih), 0)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mdc)
    user32.ReleaseDC(0, sdc)
    raw = bytearray()
    src = buf.raw
    ow, oh = w // scale, h // scale
    for y in range(oh):
        raw.append(0)
        row = src[y * scale * w * 4:(y * scale + 1) * w * 4]
        for x in range(ow):
            i = x * scale * 4
            raw += bytes((row[i + 2], row[i + 1], row[i]))
    w, h = ow, oh

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + \
          chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b"")
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "wb") as f:
        f.write(png)
    return path


if __name__ == "__main__":
    import sys
    print(screenshot(sys.argv[1] if len(sys.argv) > 1 else "shot.png"))
