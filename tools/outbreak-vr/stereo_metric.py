"""Stereo metric for a first-person stereo dump pair (PCSX2_VR_STEREODUMP): measures the disparity of the
right-eye image against the left, tile by tile, and checks the near-field floor and the head-roll rotation.

Each 48x48 tile of the gradient-magnitude images gets a (dx, dy) by phase correlation (the sway_metric.shift
style, with a sub-pixel peak); tiles with too little texture or a weak peak are dropped. dx > 0 is uncrossed
(behind the 1.5 m convergence plane), dx < 0 crossed (in front of it). Reports dx percentiles in pixels and
in units of sep*width (the far-field disparity), and the median disparity angle atan2(-dy, dx) on the display
(dy and dx scaled to the frame's display aspect from the sidecar; crossed tiles are folded onto the same axis).

  python stereo_metric.py <dir | stereo_NNNNNN_L.png | stereo_NNNNNN> [--expect-floor F] [--expect-roll R]

A directory picks its newest pair. --expect-floor F (e.g. -1): the 5th percentile of dx is F*sep*width
within 15%, and the 1st percentile is never below F*sep*width - 2 px. --expect-roll R (degrees): the median
angle is within 1 deg of R. Exit code 0 = pass (or nothing to check), 1 = a check failed, 2 = bad input.
"""
import argparse
import glob
import json
import math
import os
import sys

import numpy as np


def load_gray(path):
    from PIL import Image
    return np.asarray(Image.open(path).convert("L")).astype(np.float32)


def grad_mag(im):
    gx = np.zeros_like(im)
    gy = np.zeros_like(im)
    gx[:, 1:-1] = 0.5 * (im[:, 2:] - im[:, :-2])
    gy[1:-1, :] = 0.5 * (im[2:, :] - im[:-2, :])
    return np.sqrt(gx * gx + gy * gy)


def _subpixel(c, y, x):
    """Parabolic refinement of the correlation peak at (y, x), wrapping at the edges."""
    h, w = c.shape
    l, r = c[y, (x - 1) % w], c[y, (x + 1) % w]
    u, d = c[(y - 1) % h, x], c[(y + 1) % h, x]
    p = c[y, x]
    den_x = l - 2.0 * p + r
    den_y = u - 2.0 * p + d
    ox = 0.5 * (l - r) / den_x if abs(den_x) > 1e-12 else 0.0
    oy = 0.5 * (u - d) / den_y if abs(den_y) > 1e-12 else 0.0
    return float(np.clip(ox, -0.5, 0.5)), float(np.clip(oy, -0.5, 0.5))


def shift(a, b):
    """(dx, dy, peak, second) that moves a onto b, by phase correlation, to a sub-pixel. second is the
    highest correlation more than 2 px from the peak (a repeating texture gives a second peak close to it)."""
    fa, fb = np.fft.fft2(a), np.fft.fft2(b)
    r = fb * np.conj(fa)
    r /= np.abs(r) + 1e-9
    c = np.abs(np.fft.ifft2(r))
    y, x = np.unravel_index(np.argmax(c), c.shape)
    ox, oy = _subpixel(c, y, x)
    peak = float(c[y, x])
    masked = np.roll(np.roll(c, -y + 2, axis=0), -x + 2, axis=1).copy()
    masked[:5, :5] = 0.0
    second = float(masked.max())
    if y > c.shape[0] // 2:
        y -= c.shape[0]
    if x > c.shape[1] // 2:
        x -= c.shape[1]
    return float(x) + ox, float(y) + oy, peak, second


def resolve_pair(arg):
    """(left, right, json or None) from a directory, an _L/_R png, or a stereo_NNNNNN base path."""
    if os.path.isdir(arg):
        lefts = sorted(glob.glob(os.path.join(arg, "stereo_*_L.png")), key=os.path.getmtime)
        if not lefts:
            raise FileNotFoundError("no stereo_*_L.png in %s" % arg)
        base = lefts[-1][: -len("_L.png")]
    elif arg.endswith("_L.png") or arg.endswith("_R.png"):
        base = arg[: -len("_L.png")]
    elif arg.endswith(".json"):
        base = arg[: -len(".json")]
    else:
        base = arg
    left, right, side = base + "_L.png", base + "_R.png", base + ".json"
    for p in (left, right):
        if not os.path.isfile(p):
            raise FileNotFoundError(p)
    return left, right, (side if os.path.isfile(side) else None)


def measure(left, right, tile, min_conf, min_energy_pct, max_second):
    gl, gr = grad_mag(left), grad_mag(right)
    h, w = gl.shape
    win = np.outer(np.hanning(tile), np.hanning(tile)).astype(np.float32)
    energies, cells = [], []
    for y0 in range(0, h - tile + 1, tile):
        for x0 in range(0, w - tile + 1, tile):
            a = gl[y0:y0 + tile, x0:x0 + tile]
            energies.append(float(a.std()))
            cells.append((y0, x0))
    if not cells:
        return np.zeros((0, 5))
    floor_e = max(1.0, float(np.percentile(energies, min_energy_pct)))
    rows = []
    for (y0, x0), e in zip(cells, energies):
        if e < floor_e:
            continue
        a = gl[y0:y0 + tile, x0:x0 + tile]
        b = gr[y0:y0 + tile, x0:x0 + tile]
        if float(b.std()) < floor_e * 0.5:
            continue
        dx, dy, peak, second = shift((a - a.mean()) * win, (b - b.mean()) * win)
        if peak < min_conf or second > max_second * peak:
            continue
        rows.append((x0 + tile * 0.5, y0 + tile * 0.5, dx, dy, peak))
    return np.array(rows, dtype=np.float64).reshape(-1, 5)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pair", help="dump directory (newest pair), stereo_NNNNNN_L.png, or stereo_NNNNNN")
    ap.add_argument("--tile", type=int, default=48, help="tile size in pixels (default 48; raised to fit the expected disparity)")
    ap.add_argument("--min-conf", type=float, default=0.12, help="phase-correlation peak cutoff (default 0.12)")
    ap.add_argument("--max-second", type=float, default=0.7,
                    help="drop tiles whose second correlation peak exceeds this fraction of the first (default 0.7)")
    ap.add_argument("--min-energy-pct", type=float, default=40.0,
                    help="drop tiles whose gradient energy is below this percentile (default 40)")
    ap.add_argument("--sep", type=float, default=None, help="stereo separation (default: the sidecar's, else 0.0132)")
    ap.add_argument("--aspect", type=float, default=None, help="display aspect (default: the sidecar's, else 4/3)")
    ap.add_argument("--expect-floor", type=float, default=None, help="expected near floor in units of sep (e.g. -1)")
    ap.add_argument("--expect-roll", type=float, default=None, help="expected disparity angle in degrees")
    args = ap.parse_args()

    try:
        lpath, rpath, side = resolve_pair(args.pair)
    except FileNotFoundError as e:
        print("stereo_metric: missing %s" % e)
        return 2

    meta = {}
    if side:
        with open(side, "r", encoding="utf-8") as f:
            meta = json.load(f)
    left, right = load_gray(lpath), load_gray(rpath)
    if left.shape != right.shape:
        print("stereo_metric: left %s and right %s differ in size" % (left.shape, right.shape))
        return 2
    h, w = left.shape
    sep = args.sep if args.sep is not None else float(meta.get("sep", 0.0132))
    aspect = args.aspect if args.aspect is not None else float(meta.get("aspect", 4.0 / 3.0))
    unit = sep * w  # far-field disparity in pixels (right against left)

    tile = max(16, args.tile)
    need = int(math.ceil(2.2 * max(abs(unit), 1.0) * max(1.0, abs(args.expect_floor or 1.0))))
    if need > tile:
        tile = int(math.ceil(need / 16.0) * 16)
        print("note: tile raised to %d px to fit disparities up to %.0f px" % (tile, need / 2.2))

    rows = measure(left, right, tile, args.min_conf, args.min_energy_pct, args.max_second)
    print("pair: %s" % os.path.basename(lpath)[: -len("_L.png")])
    print("  frame %dx%d, sep %.4f (sep*width %.1f px), aspect %.3f, near_floor %s, roll %s deg (parallax %s)" % (
        w, h, sep, unit, aspect, meta.get("near_floor", "?"), meta.get("roll_deg", "?"), meta.get("roll_parallax", "?")))
    if len(rows) < 8:
        print("  only %d confident tiles: not enough texture to measure" % len(rows))
        return 2

    dx, dy = rows[:, 2], rows[:, 3]
    pct = [1, 5, 25, 50, 75, 95, 99]
    pv = np.percentile(dx, pct)
    print("  %d confident tiles of %d" % (len(rows), (h // tile) * (w // tile)))
    print("  dx px:       " + "  ".join("p%d %+.1f" % (p, v) for p, v in zip(pct, pv)))
    if abs(unit) > 1e-6:
        print("  dx sep*w:    " + "  ".join("p%d %+.2f" % (p, v / unit) for p, v in zip(pct, pv)))
    print("  dy px:       median %+.2f, p5 %+.2f, p95 %+.2f" % (
        float(np.median(dy)), float(np.percentile(dy, 5)), float(np.percentile(dy, 95))))

    # Disparity angle on the display: NDC x = 2 dx / w, NDC y = 2 dy / h; the display is `aspect` wide per unit
    # high, so the angle is atan2(-dy / h, dx * aspect / w). Crossed tiles are folded onto the same axis, and
    # tiles near zero disparity (on the convergence plane) have no direction, so they are left out.
    mag_min = max(3.0, 0.25 * abs(unit))
    vx = dx * aspect / w
    vy = -dy / h
    mag_px = np.hypot(dx, dy)
    sel = mag_px >= mag_min
    angle = None
    if np.count_nonzero(sel) >= 4:
        fx, fy = vx[sel].copy(), vy[sel].copy()
        flip = fx < 0
        fx[flip], fy[flip] = -fx[flip], -fy[flip]
        angles = np.degrees(np.arctan2(fy, fx))
        angle = float(np.median(angles))
        rx, ry = dx[sel].copy(), -dy[sel]
        ry = np.where(rx < 0, -ry, ry)
        rx = np.abs(rx)
        raw = float(np.median(np.degrees(np.arctan2(ry, rx))))
        print("  angle: median %+.2f deg on the display (%+.2f deg in frame pixels), %d tiles with |d| >= %.1f px" % (
            angle, raw, int(np.count_nonzero(sel)), mag_min))
    else:
        print("  angle: too few tiles with |d| >= %.1f px (disparity about 0)" % mag_min)

    ok = True
    if args.expect_floor is not None:
        target = args.expect_floor * unit
        p5, p1 = float(pv[1]), float(pv[0])
        tol = max(0.15 * abs(target), 1.0)
        near_ok = abs(p5 - target) <= tol
        never_ok = p1 >= target - 2.0
        if meta.get("roll_parallax") and abs(float(meta.get("roll_deg", 0.0))) > 1.0:
            print("  warning: the dump has head roll %.1f deg; the floor check assumes roll 0" % float(meta["roll_deg"]))
        print("  floor: p5 %+.1f px vs %+.1f +-%.1f px -> %s; p1 %+.1f px >= %+.1f px -> %s" % (
            p5, target, tol, "ok" if near_ok else "FAIL", p1, target - 2.0, "ok" if never_ok else "FAIL"))
        ok = ok and near_ok and never_ok
    if args.expect_roll is not None:
        if angle is None:
            if abs(args.expect_roll) >= 40.0:
                print("  roll: no measurable disparity, as expected at %.0f deg (faded out) -> ok" % args.expect_roll)
            else:
                print("  roll: no measurable angle -> FAIL")
                ok = False
        else:
            roll_ok = abs(angle - args.expect_roll) <= 1.0
            print("  roll: %+.2f deg vs %+.2f +-1 deg -> %s" % (angle, args.expect_roll, "ok" if roll_ok else "FAIL"))
            ok = ok and roll_ok
    if args.expect_floor is not None or args.expect_roll is not None:
        print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
