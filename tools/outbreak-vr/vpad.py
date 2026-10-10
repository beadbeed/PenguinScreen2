"""Virtual Xbox 360 pad (ViGEm via vgamepad) that the emulator sees as an SDL gamepad, so a
script can play the game without focusing its window. Buttons are named in PS2 terms using
the SDL automatic mapping (Cross = A/FaceSouth, Circle = B/FaceEast, ...).
"""
import time

import vgamepad as vg

B = vg.XUSB_BUTTON
BUTTONS = {
    "cross": B.XUSB_GAMEPAD_A, "circle": B.XUSB_GAMEPAD_B,
    "square": B.XUSB_GAMEPAD_X, "triangle": B.XUSB_GAMEPAD_Y,
    "select": B.XUSB_GAMEPAD_BACK, "start": B.XUSB_GAMEPAD_START,
    "l1": B.XUSB_GAMEPAD_LEFT_SHOULDER, "r1": B.XUSB_GAMEPAD_RIGHT_SHOULDER,
    "l3": B.XUSB_GAMEPAD_LEFT_THUMB, "r3": B.XUSB_GAMEPAD_RIGHT_THUMB,
    "up": B.XUSB_GAMEPAD_DPAD_UP, "down": B.XUSB_GAMEPAD_DPAD_DOWN,
    "left": B.XUSB_GAMEPAD_DPAD_LEFT, "right": B.XUSB_GAMEPAD_DPAD_RIGHT,
}
TRIGGERS = {"l2", "r2"}


class VPad:
    def __init__(self, settle=1.5):
        self.pad = vg.VX360Gamepad()
        self.pad.reset()
        self.pad.update()
        time.sleep(settle)  # let SDL see the hot-plug

    def close(self):
        self.pad.reset()
        self.pad.update()
        del self.pad

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def _set(self, name, down):
        if name in TRIGGERS:
            v = 255 if down else 0
            (self.pad.left_trigger if name == "l2" else self.pad.right_trigger)(value=v)
        elif down:
            self.pad.press_button(button=BUTTONS[name])
        else:
            self.pad.release_button(button=BUTTONS[name])

    def press(self, name, ms=120, gap_ms=150):
        self.hold([name], ms)
        time.sleep(gap_ms / 1000)

    def hold(self, names, ms, lx=0.0, ly=0.0, rx=0.0, ry=0.0):
        """Hold buttons (list, may be empty) and/or stick deflections (-1..1; +ly = up) for ms."""
        for n in names:
            self._set(n, True)
        self.pad.left_joystick_float(x_value_float=lx, y_value_float=ly)
        self.pad.right_joystick_float(x_value_float=rx, y_value_float=ry)
        self.pad.update()
        time.sleep(ms / 1000)
        for n in names:
            self._set(n, False)
        self.pad.left_joystick_float(x_value_float=0.0, y_value_float=0.0)
        self.pad.right_joystick_float(x_value_float=0.0, y_value_float=0.0)
        self.pad.update()

    def stick(self, lx=0.0, ly=0.0, ms=500, rx=0.0, ry=0.0, buttons=()):
        self.hold(list(buttons), ms, lx=lx, ly=ly, rx=rx, ry=ry)
        time.sleep(0.1)
