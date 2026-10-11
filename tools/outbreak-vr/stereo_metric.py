"""Stereo metric for a first-person stereo dump pair (PCSX2_VR_STEREODUMP): measures the disparity of the
right-eye image against the left, tile by tile, and checks the near-field floor and the head-roll rotation.

Each 48x48 tile of the gradient-magnitude images gets a (dx, dy) by phase correlation (the sway_metric.shift
style, with a sub-pixel peak); tiles with too little texture or a weak peak are dropped. dx > 0 is uncrossed
(behind the 1.5 m convergence plane), dx < 0 crossed (in front of it). Reports dx percentiles in pixels and
in units of sep*width (the far-field disparity), and the median disparity angle atan2(-dy, dx) on the display
(dy and dx scaled to the frame's display aspect from the sidecar, screen_w_m / screen_h_m when it has them;
crossed tiles are folded onto the same axis).

  python stereo_metric.py <dir | stereo_NNNNNN_L.png | stereo_NNNNNN> [--expect-floor F] [--expect-roll R]
                          [--expect-ortho] [--compare REF]
  python stereo_metric.py <dir> --ease

A directory picks its newest pair. --expect-floor F (e.g. -1): the 5th percentile of dx is F*sep*width
within 15%, and the 1st percentile is never below F*sep*width - 2 px. --expect-roll R (degrees): the median
angle is within 1 deg of R.

--expect-ortho: first-person auto stereo is orthostereo. Needs a sidecar with auto true and ease 1 (else exit
2). The sidecar sep equals min(strength, 1) * IPD / screen width (divided by ndc_ratio, capped at 0.03) within
1%, and the 99th percentile of dx is at most sep*width*ndc_ratio + 1.5 px: far content never lands more than
one IPD apart. That pixel check also fails when the draws' NDC is not as wide as the picture.

--compare REF: the disparity of this pair against another dump pair REF of the same view (another strength,
IPD or aspect). Both are measured with one tile size and matched tile by tile; tiles with |dy| > 2 px or within
1 px of either run's near floor are dropped. D = a * D_ref + b is fitted by least squares (residuals over 2 px
dropped and refitted, twice; at least 20 tiles spanning 0.4 * U_ref). With U = sep*width*ndc_ratio and c = conv
from each sidecar the linear map predicts a = U*c / (U_ref*c_ref) and b = U * (1 - c/c_ref); passes within 5%
on a and 1.5 px on b.

--ease: every sidecar in the directory, in frame order. Each arm ramp (ease rising from about 0 to 1, the
separation starting from 0) must follow sep = sep_target * smoothstep(ease) within 1e-4 * sep_target (plus the
sidecar's 6-decimal rounding), rise monotonically and never pass sep_target; the time from the last dump with
ease <= 0.05 to the first with ease >= 0.999 is 350-700 ms; and the image's p95 dx / (sep_target*width) follows
k * smoothstep(ease) within 0.12 k, k from the ramp's settled dumps. Exit 2 if no ramp was captured.

Exit code 0 = pass (or nothing to check), 1 = a check failed, 2 = bad input.
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


def tile_for(base_tile, unit, floor_mult, quiet=False):
    """The tile size that fits disparities up to unit * max(1, |floor_mult|) (raised from base_tile)."""
    tile = max(16, base_tile)
    need = int(math.ceil(2.2 * max(abs(unit), 1.0) * max(1.0, abs(floor_mult or 1.0))))
    if need > tile:
        tile = int(math.ceil(need / 16.0) * 16)
        if not quiet:
            print("note: tile raised to %d px to fit disparities up to %.0f px" % (tile, need / 2.2))
    return tile


def load_meta(side):
    if not side:
        return {}
    with open(side, "r", encoding="utf-8") as f:
        return json.load(f)


def meta_aspect(meta):
    """Display aspect from the sidecar: the first-person screen's own size when present, else its aspect."""
    sw, sh = float(meta.get("screen_w_m", 0.0) or 0.0), float(meta.get("screen_h_m", 0.0) or 0.0)
    if sw > 0.0 and sh > 0.0:
        return sw / sh
    return float(meta.get("aspect", 4.0 / 3.0))


def smoothstep(t):
    t = min(max(float(t), 0.0), 1.0)
    return t * t * (3.0 - 2.0 * t)


def print_auto_fields(meta):
    if "auto" not in meta:
        return
    print("  auto %s, IPD %.1f mm, screen %.2f x %.2f m at %.2f m, strength %.2f, sep_target %.6f, ease %.3f, "
          "ndc_ratio %.3f, t_ms %s" % (
              "true" if meta.get("auto") else "false", float(meta.get("ipd_mm", 0.0)), float(meta.get("screen_w_m", 0.0)),
              float(meta.get("screen_h_m", 0.0)), float(meta.get("screen_d_m", 0.0)), float(meta.get("strength", 1.0)),
              float(meta.get("sep_target", 0.0)), float(meta.get("ease", 1.0)), float(meta.get("ndc_ratio", 1.0)),
              meta.get("t_ms", "?")))


def robust_line_fit(x, y, drop_px=2.0, rounds=2):
    """Least-squares y = a*x + b, then drop residuals over drop_px and refit, `rounds` times."""
    keep = np.ones(len(x), dtype=bool)
    a = b = 0.0
    for i in range(rounds + 1):
        if np.count_nonzero(keep) < 2:
            break
        a, b = np.polyfit(x[keep], y[keep], 1)
        if i == rounds:
            break
        keep = np.abs(y - (a * x + b)) <= drop_px
    return float(a), float(b), keep


def run_compare(args, left, right, meta, sep):
    """--compare REF. Returns (ok, bad_input)."""
    try:
        rl, rr, rside = resolve_pair(args.compare)
    except FileNotFoundError as e:
        print("stereo_metric: --compare: missing %s" % e)
        return False, True
    rmeta = load_meta(rside)
    if "conv" not in meta or "conv" not in rmeta or "sep" not in rmeta:
        print("stereo_metric: --compare needs sidecars with sep and conv for both pairs")
        return False, True
    ref_left, ref_right = load_gray(rl), load_gray(rr)
    if ref_left.shape != left.shape or ref_right.shape != right.shape:
        print("stereo_metric: --compare: the pairs differ in size (%s vs %s)" % (left.shape, ref_left.shape))
        return False, True
    h, w = left.shape
    c, c_ref = float(meta["conv"]), float(rmeta["conv"])
    sep_ref = float(rmeta["sep"])
    u = sep * w * float(meta.get("ndc_ratio", 1.0))
    u_ref = sep_ref * w * float(rmeta.get("ndc_ratio", 1.0))
    if abs(u_ref) < 1e-6 or abs(c_ref) < 1e-6:
        print("stereo_metric: --compare: the reference has no disparity (sep %.6f, conv %.4g)" % (sep_ref, c_ref))
        return False, True
    floor_now, floor_ref = float(meta.get("near_floor", 0.0)), float(rmeta.get("near_floor", 0.0))
    tile = max(tile_for(args.tile, u, floor_now, quiet=True), tile_for(args.tile, u_ref, floor_ref, quiet=True))
    rows = measure(left, right, tile, args.min_conf, args.min_energy_pct, args.max_second)
    rrows = measure(ref_left, ref_right, tile, args.min_conf, args.min_energy_pct, args.max_second)
    ref_by_pos = {(int(r[0]), int(r[1])): r for r in rrows}
    xs, ys = [], []
    for r in rows:
        q = ref_by_pos.get((int(r[0]), int(r[1])))
        if q is None:
            continue
        if abs(r[3]) > 2.0 or abs(q[3]) > 2.0:
            continue
        # On either run's floor the disparity is clamped, not linear in 1/w.
        if r[2] <= floor_now * u + 1.0 or q[2] <= floor_ref * u_ref + 1.0:
            continue
        xs.append(q[2])
        ys.append(r[2])
    print("  compare: against %s (sep %.6f, conv %.4g, U_ref %.1f px); tile %d px, %d matched tiles" % (
        os.path.basename(rl)[: -len("_L.png")], sep_ref, c_ref, u_ref, tile, len(xs)))
    if len(xs) < 20:
        print("  compare: only %d usable tiles (need 20) -> FAIL" % len(xs))
        return False, False
    x, y = np.array(xs), np.array(ys)
    a, b, keep = robust_line_fit(x, y)
    kept = int(np.count_nonzero(keep))
    span = float(x[keep].max() - x[keep].min()) if kept > 0 else 0.0
    a_pred = (u * c) / (u_ref * c_ref)
    b_pred = u * (1.0 - c / c_ref)
    enough = kept >= 20 and span >= 0.4 * abs(u_ref)
    a_ok = abs(a - a_pred) <= 0.05 * max(abs(a_pred), 1e-6)
    b_ok = abs(b - b_pred) <= 1.5
    print("  compare: D = %.4f * D_ref %+.2f px over %d tiles (D_ref span %.1f px, need %.1f); predicted %.4f, %+.2f px "
          "-> slope %s, intercept %s%s" % (
              a, b, kept, span, 0.4 * abs(u_ref), a_pred, b_pred, "ok" if a_ok else "FAIL", "ok" if b_ok else "FAIL",
              "" if enough else " (too few tiles or too narrow a span: FAIL)"))
    return enough and a_ok and b_ok, False


def run_expect_ortho(meta, sep, w, pv):
    """--expect-ortho. Returns (ok, bad_input)."""
    if not meta.get("auto") or float(meta.get("ease", 0.0)) < 0.999:
        print("stereo_metric: --expect-ortho needs a sidecar with auto true and ease 1 (got auto %s, ease %s)" % (
            meta.get("auto", "?"), meta.get("ease", "?")))
        return False, True
    ipd_m = float(meta.get("ipd_mm", 0.0)) / 1000.0
    screen_w = float(meta.get("screen_w_m", 0.0))
    strength = float(meta.get("strength", 1.0))
    ratio = float(meta.get("ndc_ratio", 1.0)) or 1.0
    if ipd_m <= 0.0 or screen_w <= 0.0:
        print("stereo_metric: --expect-ortho: the sidecar has no IPD or screen width")
        return False, True
    expected = min(min(strength, 1.0) * ipd_m / (screen_w * ratio), min(ipd_m / (screen_w * ratio), 0.03))
    book_ok = abs(sep - expected) <= 0.01 * expected
    far_px = sep * w * ratio
    p99 = float(pv[6])
    px_ok = p99 <= far_px + 1.5
    print("  ortho: sep %.6f vs min(strength,1) * IPD / width %.6f +-1%% -> %s" % (sep, expected, "ok" if book_ok else "FAIL"))
    print("  ortho: p99 dx %+.1f px <= %.1f + 1.5 px -> %s" % (p99, far_px, "ok" if px_ok else "FAIL"))
    print("  far-field max %.1f px = %.1f mm on the %.2f m screen (IPD %.1f mm)" % (
        p99, p99 / w * screen_w * 1000.0, screen_w, ipd_m * 1000.0))
    return book_ok and px_ok, False


def run_ease(args):
    """--ease over every sidecar in a directory."""
    if not os.path.isdir(args.pair):
        print("stereo_metric: --ease needs a dump directory")
        return 2
    dumps = []
    for side in glob.glob(os.path.join(args.pair, "stereo_*.json")):
        try:
            meta = load_meta(side)
        except (OSError, ValueError):
            continue
        if "ease" not in meta or "sep_target" not in meta or "frame" not in meta:
            continue
        dumps.append((int(meta["frame"]), side[: -len(".json")], meta))
    dumps.sort(key=lambda d: d[0])
    if not dumps:
        print("stereo_metric: --ease: no sidecars with ease in %s" % args.pair)
        return 2

    # Segments: a new one starts wherever ease falls (an arm or a retarget restarts it).
    segments, cur, last_ease = [], [], None
    for d in dumps:
        e = float(d[2]["ease"])
        if cur and last_ease is not None and e < last_ease - 1e-6:
            segments.append(cur)
            cur = []
        cur.append(d)
        last_ease = e
    if cur:
        segments.append(cur)

    def snap(e):
        k = round(e * 30.0)
        return k / 30.0 if abs(e - k / 30.0) < 1e-3 else e

    ramps = 0
    ok = True
    for seg in segments:
        eases = [float(d[2]["ease"]) for d in seg]
        if min(eases) >= 0.999 or max(eases) < 0.999:
            continue
        first = seg[0][2]
        target0 = float(first.get("sep_target", 0.0))
        # An arm ramp starts from zero separation; a retarget starts from the old value and is not checked here.
        if abs(float(first["sep"]) - target0 * smoothstep(snap(float(first["ease"])))) > max(0.02 * abs(target0), 2e-6):
            print("  ramp at frame %d: starts from sep %.6f, not from 0 (a retarget); skipped" % (seg[0][0], float(first["sep"])))
            continue
        ramps += 1
        rok = True
        target = target0
        tol = max(1e-4 * abs(target), 6e-7)
        prev_sep = -1.0
        worst = 0.0
        mono_ok = True
        over_ok = True
        for _, _, m in seg:
            s, e = float(m["sep"]), snap(float(m["ease"]))
            worst = max(worst, abs(s - float(m["sep_target"]) * smoothstep(e)))
            if s < prev_sep - 1e-7:
                mono_ok = False
            if s > float(m["sep_target"]) * 1.0001 + 6e-7:
                over_ok = False
            prev_sep = s
        curve_ok = worst <= tol
        early = [m for _, _, m in seg if float(m["ease"]) <= 0.05]
        late = [m for _, _, m in seg if float(m["ease"]) >= 0.999]
        if early and late and "t_ms" in early[-1] and "t_ms" in late[0]:
            dt = float(late[0]["t_ms"]) - float(early[-1]["t_ms"])
            time_ok = 350.0 <= dt <= 700.0
            time_txt = "%.0f ms" % dt
        else:
            time_ok = False
            time_txt = "no dump with ease <= 0.05"
        rok = curve_ok and mono_ok and over_ok and time_ok
        print("  ramp at frame %d: %d dumps, sep_target %.6f; curve worst %.2e (tol %.2e) -> %s; monotonic -> %s; "
              "never above target -> %s; %s from ease <= 0.05 to 1 (350-700) -> %s" % (
                  seg[0][0], len(seg), target, worst, tol, "ok" if curve_ok else "FAIL", "ok" if mono_ok else "FAIL",
                  "ok" if over_ok else "FAIL", time_txt, "ok" if time_ok else "FAIL"))

        # The picture follows the ease: p95 dx / (sep_target * width) = k * smoothstep(ease).
        measured = []
        for frame, base, m in seg:
            lp, rp = base + "_L.png", base + "_R.png"
            if not (os.path.isfile(lp) and os.path.isfile(rp)):
                continue
            left, right = load_gray(lp), load_gray(rp)
            if left.shape != right.shape:
                continue
            w = left.shape[1]
            unit_t = float(m["sep_target"]) * w * float(m.get("ndc_ratio", 1.0))
            if abs(unit_t) < 1e-6:
                continue
            tile = tile_for(args.tile, unit_t, float(m.get("near_floor", 0.0)), quiet=True)
            rows = measure(left, right, tile, args.min_conf, args.min_energy_pct, args.max_second)
            if len(rows) < 8:
                continue
            measured.append((frame, snap(float(m["ease"])), float(np.percentile(rows[:, 2], 95)) / unit_t))
        settled = [v for _, e, v in measured if e >= 0.999]
        if not settled:
            print("  ramp at frame %d: no measurable settled dump for k -> FAIL" % seg[0][0])
            rok = False
        else:
            k = float(np.median(settled))
            worst_img = 0.0
            for frame, e, v in measured:
                if e >= 0.999:
                    continue
                worst_img = max(worst_img, abs(v - k * smoothstep(e)))
            img_ok = worst_img <= 0.12 * abs(k)
            print("  ramp at frame %d: image p95 dx / (sep_target*w) follows %.3f * smoothstep(ease): worst %.3f "
                  "(tol %.3f) over %d dumps -> %s" % (
                      seg[0][0], k, worst_img, 0.12 * abs(k), len(measured), "ok" if img_ok else "FAIL"))
            rok = rok and img_ok
        ok = ok and rok

    if ramps == 0:
        print("stereo_metric: --ease: no arm ramp captured in %s" % args.pair)
        return 2
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


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
    ap.add_argument("--aspect", type=float, default=None,
                    help="display aspect (default: the sidecar's screen_w_m / screen_h_m, else its aspect, else 4/3)")
    ap.add_argument("--expect-floor", type=float, default=None, help="expected near floor in units of sep (e.g. -1)")
    ap.add_argument("--expect-roll", type=float, default=None, help="expected disparity angle in degrees")
    ap.add_argument("--expect-ortho", action="store_true",
                    help="auto stereo is orthostereo: sep = IPD / screen width, far content at most one IPD apart")
    ap.add_argument("--compare", metavar="REF", default=None,
                    help="fit this pair's disparity against reference pair REF and check the predicted slope/intercept")
    ap.add_argument("--ease", action="store_true", help="check the first-person ease over every dump in the directory")
    args = ap.parse_args()

    if args.ease:
        return run_ease(args)

    try:
        lpath, rpath, side = resolve_pair(args.pair)
    except FileNotFoundError as e:
        print("stereo_metric: missing %s" % e)
        return 2

    meta = load_meta(side)
    left, right = load_gray(lpath), load_gray(rpath)
    if left.shape != right.shape:
        print("stereo_metric: left %s and right %s differ in size" % (left.shape, right.shape))
        return 2
    h, w = left.shape
    sep = args.sep if args.sep is not None else float(meta.get("sep", 0.0132))
    aspect = args.aspect if args.aspect is not None else meta_aspect(meta)
    unit = sep * w  # far-field disparity in pixels (right against left)

    tile = tile_for(args.tile, unit, args.expect_floor)

    rows = measure(left, right, tile, args.min_conf, args.min_energy_pct, args.max_second)
    print("pair: %s" % os.path.basename(lpath)[: -len("_L.png")])
    print("  frame %dx%d, sep %.4f (sep*width %.1f px), aspect %.3f, near_floor %s, roll %s deg (parallax %s)" % (
        w, h, sep, unit, aspect, meta.get("near_floor", "?"), meta.get("roll_deg", "?"), meta.get("roll_parallax", "?")))
    print_auto_fields(meta)
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
    if args.expect_ortho:
        ortho_ok, bad = run_expect_ortho(meta, sep, w, pv)
        if bad:
            return 2
        ok = ok and ortho_ok
    if args.compare is not None:
        cmp_ok, bad = run_compare(args, left, right, meta, sep)
        if bad:
            return 2
        ok = ok and cmp_ok
    if args.expect_floor is not None or args.expect_roll is not None or args.expect_ortho or args.compare is not None:
        print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
