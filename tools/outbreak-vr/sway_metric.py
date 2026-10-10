"""World-stability metric for the first-person screen on the null headset: while the fake head sways
(PCSX2_VR_FAKE_HEADPOSE) and the null HMD stays still, a perfectly placed frame shows the world
standing still in the Headset Window however the quad itself swings. Measures the image shift of the
centre of the left eye between Headset Window captures (phase correlation on edges, so it needs no
particular landmark) and reports how much the world moves.

  python sway_metric.py [frames] [interval_s]
"""
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
import hmdshot


def left_centre(path):
    from PIL import Image
    im = np.asarray(Image.open(path).convert("L")).astype(np.float32)
    h, w = im.shape
    eye = im[:, : w // 2]
    eh, ew = eye.shape
    c = eye[eh // 4: eh * 3 // 4, ew // 4: ew * 3 // 4]
    gx = np.abs(np.diff(c, axis=1))[:-1, :]
    gy = np.abs(np.diff(c, axis=0))[:, :-1]
    g = gx + gy
    win = np.outer(np.hanning(g.shape[0]), np.hanning(g.shape[1]))
    return (g - g.mean()) * win


def shift(a, b):
    """(dx, dy) that moves a onto b, by phase correlation, to the pixel."""
    fa, fb = np.fft.fft2(a), np.fft.fft2(b)
    r = fb * np.conj(fa)
    r /= np.abs(r) + 1e-9
    c = np.abs(np.fft.ifft2(r))
    y, x = np.unravel_index(np.argmax(c), c.shape)
    if y > c.shape[0] // 2:
        y -= c.shape[0]
    if x > c.shape[1] // 2:
        x -= c.shape[1]
    return float(x), float(y), float(c.max())


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 30
    dt = float(sys.argv[2]) if len(sys.argv) > 2 else 0.13
    frames = []
    for i in range(n):
        p = hmdshot.shot(r"D:/Games/PS2/screens/sway-%02d.png" % i, scale=1)
        frames.append(left_centre(p))
        time.sleep(dt)
    ref = frames[0]
    xs, ys, steps = [], [], []
    for i, f in enumerate(frames):
        dx, dy, _ = shift(ref, f)
        xs.append(dx)
        ys.append(dy)
        if i:
            sx, sy, _ = shift(frames[i - 1], f)
            steps.append(abs(sx) + abs(sy))
    xs, ys = np.array(xs), np.array(ys)
    print("world shift vs first frame: x std %.1f px (range %.0f), y std %.1f px (range %.0f); mean step %.1f px" % (
        xs.std(), xs.max() - xs.min(), ys.std(), ys.max() - ys.min(), float(np.mean(steps))))


if __name__ == "__main__":
    main()
