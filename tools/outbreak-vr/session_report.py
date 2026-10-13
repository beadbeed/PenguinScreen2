#!/usr/bin/env python3
"""Plain-English report on a PenguinScreen2 VR headset session, from its logs.

  python session_report.py [EMULOG] [--session [DIR]] [--steamvr [DIR]] [--html FILE] [options]

EMULOG   the emulator log (default D:\\Games\\PS2\\PenguinScreen2\\logs\\emulog.txt, or the emulog.txt inside --session).
--session [DIR]   the session recorder folder (state.csv with the character's state ~4 times a second). Without DIR the
                  newest folder under D:\\Games\\PS2\\sessions.
--steamvr [DIR]   SteamVR's logs folder (default D:\\programfile\\steam\\logs, read automatically when it exists; --no-steamvr
                  skips it): its session states, dashboard and standby events and stream trouble are lined up with the emulog.
--html FILE       also write one self-contained page (inline SVG charts, no external libraries).
--t0 "YYYY-MM-DD HH:MM:SS[.f]"   wall-clock time of emulog t = 0, when it cannot be worked out from the SteamVR log.
--app NAME        the emulator's name in SteamVR's xrclient_<name>.txt (default pcsx2).
--end-ms MS --end-secs S   plateau rule: xrEndFrame mean over MS (default 3) for longer than S seconds (default 10).
-v                per-minute frame table in full, every toast, every SteamVR event.
--selftest        check the line parsers against built-in sample lines and exit.

What it reads (every line is optional, older builds and other games just give a shorter report):
  emulog   "(VR) Frames:" (submitted/s, new GS frames/s, xrEndFrame, queue-lock wait), "Session state:" (focus), Eyes (IPD, FOV),
           controller profiles, CameraDriver ARMED/DISARMED (with the failing guard in builds that name it), HUD blink,
           Screen/Stereo scene switches, MenuPointer (item screen, map/pause), Feedback (shots, hurts, reloads, heartbeat),
           grabs, toasts, warnings and errors.
  state.csv   t,serial,status,menu,overlay,stance,action,hp,hp_max,heading,x,y,z,cam_yaw,cam_pitch (tools/outbreak-vr/session_recorder.py).
  SteamVR  xrclient_<app>.txt (session states: the clock), vrserver.txt (Steam-button dashboard requests, standby, stream
           resets), vrclient_vrcompositor.txt (dashboard shown/hidden), driver_vrlink.txt (link throttle), vrcompositor.txt
           (compositor totals and stalls).

Clock: the emulog has seconds since start only. t = 0 is fitted to wall time by matching the app's "Session state: A -> B"
lines against SteamVR's "Transitioning XrSession from A to B" lines (research/2026-10-12-steamvr-endframe-research.md did
this by hand: median of the differences, good to about 0.1 s). The recorder's time base is fitted to the emulog's by lining
up its scenario-status changes with CameraDriver ARMED/DISARMED (File #1 and #2 arm on status 2 + overlay 3).

Python 3, standard library only.
"""
from __future__ import annotations

import argparse
import bisect
import csv
import datetime as dt
import glob
import html as htmllib
import math
import os
import re
import statistics
import sys
from dataclasses import dataclass, field

DEFAULT_EMULOG = r"D:\Games\PS2\PenguinScreen2\logs\emulog.txt"
DEFAULT_SESSIONS = r"D:\Games\PS2\sessions"
DEFAULT_STEAMVR = r"D:\programfile\steam\logs"

NTSC_HZ = 59.94
PAL_HZ = 50.0

# Thresholds (the plateau rule is the one asked for: xrEndFrame mean over 3 ms for more than 10 s).
TH = dict(
    end_ms=3.0,          # xrEndFrame mean above this is "slow"
    end_secs=10.0,       # ... for longer than this is a plateau
    spike_ms=8.0,        # a 5 s window whose slowest xrEndFrame is above this, outside a plateau, is a spike
    parked_ms=40.0,      # xrEndFrame mean above this: the runtime parks the app (10 Hz)
    gs_frac=0.95,        # new GS frames/s under this share of 59.94 (50 for PAL) is a GS slowdown
    sub_frac=0.95,       # submitted/s under this share of the display rate is a submission shortfall
    lock_ms=100.0,       # queue-lock wait total over this many ms per second is heavy contention
    flicker_per_min=8,   # a minute with this many screen switches is listed (menus explain most of them; see the flicker check)
    bounce_secs=3.0,     # a screen switch undone within this is a bounce
    short_arm_secs=3.0,  # first person armed for less than this is flapping
)

# What the recorder's scenario status values mean (File #1/#2 profiles and research/2026-10-12-ui-flags-research.md).
STATUS_NAMES = {2: "in game", 3: "door / room transition", 4: "cutscene"}
# Guard addresses of the shipped profiles, so a DISARMED line reads as English.
ADDR_NAMES = {
    0x540004: "game overlay id (3 = GAME)", 0x543004: "game overlay id (3 = GAME)",
    0x48BF60: "scenario status (2 = in game)", 0x4912A0: "scenario status (2 = in game)",
    0x3AEF75: "room camera mode (0 = normal; a scripted shot flips it)",
    0x3B31F5: "room camera mode (0 = normal; a scripted shot flips it)",
}
STANCE_NAMES = {1: "normal", 2: "aiming", 3: "hurt", 4: "dead/idle", 5: "grabbed"}


# --------------------------------------------------------------------------------------------------------------------
# small helpers
# --------------------------------------------------------------------------------------------------------------------

def fmt_dur(s: float) -> str:
    s = float(s)
    if s < 10:
        return f"{s:.1f} s"
    if s < 60:
        return f"{s:.0f} s"
    m, sec = divmod(int(round(s)), 60)
    if m < 60:
        return f"{m}m{sec:02d}s"
    h, m = divmod(m, 60)
    return f"{h}h{m:02d}m"


def ascii_text(s: str) -> str:
    """The text report is ASCII so any console (Windows PowerShell 5, a log viewer) shows it as written."""
    s = (str(s).replace("\u2014", "-").replace("\u2013", "-").replace("\u2018", "'").replace("\u2019", "'")
         .replace("\u201c", '"').replace("\u201d", '"').replace("\u2192", "->").replace("\u2026", "..."))
    return s.encode("ascii", "replace").decode("ascii")


def clip(s: str, n: int) -> str:
    s = " ".join(str(s).split())
    return s if len(s) <= n else s[: n - 3].rstrip() + "..."


def wmean(vals, weights):
    tw = sum(weights)
    return sum(v * w for v, w in zip(vals, weights)) / tw if tw > 0 else 0.0


class Clock:
    """emulog t (seconds) <-> wall time, once fitted."""

    def __init__(self, t0: dt.datetime | None = None, how: str = "", spread: float = 0.0):
        self.t0 = t0
        self.how = how
        self.spread = spread

    @property
    def ok(self) -> bool:
        return self.t0 is not None

    def wall(self, t: float) -> dt.datetime | None:
        return self.t0 + dt.timedelta(seconds=t) if self.t0 else None

    def hms(self, t: float) -> str:
        w = self.wall(t)
        return w.strftime("%H:%M:%S") if w else ""

    def at(self, t: float) -> str:
        """'63.3 s (18:44:45)' or '63.3 s'."""
        h = self.hms(t)
        return f"{t:.1f} s ({h})" if h else f"{t:.1f} s"

    def from_wall(self, w: dt.datetime) -> float:
        return (w - self.t0).total_seconds()


# --------------------------------------------------------------------------------------------------------------------
# emulog parsing
# --------------------------------------------------------------------------------------------------------------------

LINE_RE = re.compile(r"^\[\s*(\d+(?:\.\d+)?)\]\s?(.*?)\s*$")
MORE_RE = re.compile(r"\((\d+) more not logged\)")

FRAME_FIELDS = {
    "sub": r"submitted ([\d.]+)/s", "hz": r"display ([\d.]+) Hz", "gs": r"new GS frames ([\d.]+)/s",
    "resent": r"re-sent ([\d.]+)/s", "stale": r"stale (\d+)", "norender": r"shouldRender=false (\d+)",
    "wb_mean": r"wait->begin mean ([\d.]+)", "wb_max": r"wait->begin mean [\d.]+ max ([\d.]+)",
    "end_mean": r"xrEndFrame mean ([\d.]+)", "end_max": r"xrEndFrame mean [\d.]+ max ([\d.]+)",
    "lock_max_us": r"queue-lock wait max (\d+) us", "lock_ms_s": r"queue-lock wait max \d+ us total ([\d.]+) ms/s",
    "skipped": r"copies skipped (\d+)", "timedout": r"timed out (\d+)", "deferred": r"publishes deferred (\d+)",
    "layers": r"layers (\d+)", "mode": r"mode (.+?)\s*$",
}
FRAME_RE = {k: re.compile(v) for k, v in FRAME_FIELDS.items()}

STATE_RE = re.compile(r"Session state: (\w+) -> (\w+)(?: \((\d+) ms after session creation, (\d+) ms in (\w+)\))?")
EYES_RE = re.compile(r"Eyes: IPD ([\d.]+) mm")
KNOWN_DISARM = [
    "VR/HeadCamera switch off", "online, not private co-op", "VM not Running (paused)",
    "no valid head pose published", "profile guard failed",
]
GUARD_RE = re.compile(r"guard (\d+) \(0x([0-9A-Fa-f]+), (\d+) byte\(s\)\) reads 0x([0-9A-Fa-f]+), wants (not )?0x([0-9A-Fa-f]+)")
RELEASE_RE = re.compile(r"released \(lookAt\.releaseWhen (\d+): record \+0x([0-9A-Fa-f]+) reads 0x([0-9A-Fa-f]+)\)")

PROBLEM_RE = re.compile(
    r"(?i)\b(warning|error|errors|fail|failed|failure|fatal|exception|crash|unresolved|rejected|refused|lost|stuck|"
    r"invalid|cannot|could not|not accepted|no longer matches|did not answer|does not look like|informational only|"
    r"comfortable range|sitting out|unsafe|not bound|not initialized|shutdown skipped|no controller reported|"
    r"only the \w+ controller reported|touch bindings|ms old)\b")
ERROR_RE = re.compile(r"(?i)\b(error|errors|fail|failed|failure|fatal|exception|crash|lost)\b")
IGNORE_RE = re.compile(
    r"(CameraDriver: DISARMED|Frames: submitted|HUD toast:|Hands: placements|self-test: passed|self-test: 0 |"
    r"tearing down|is NOT supported|not offered by the runtime|Feedback: (shot|hurt|heartbeat|dry fire|reload|game rumble)|"
    r"\b0 (failed|failures?|failure\(s\)|mismatch(es)?)\b)")


@dataclass
class Ev:
    t: float
    kind: str
    d: dict = field(default_factory=dict)
    msg: str = ""


@dataclass
class Frame:
    t: float            # the window's end (the line's time)
    t0: float           # the window's start
    dur: float
    sub: float = 0.0
    hz: float = 0.0
    gs: float = 0.0
    resent: float = 0.0
    stale: int = 0
    norender: int = 0
    wb_mean: float = 0.0
    wb_max: float = 0.0
    end_mean: float | None = None
    end_max: float | None = None
    lock_max_us: int | None = None
    lock_ms_s: float | None = None
    skipped: int = 0
    timedout: int = 0
    deferred: int = 0
    layers: int = 0
    mode: str = ""

    @property
    def frames(self) -> float:
        return self.sub * self.dur


class Log:
    def __init__(self, path: str):
        self.path = path
        self.n_lines = 0
        self.t_first = 0.0
        self.t_last = 0.0
        self.ev: list[Ev] = []
        self.frames: list[Frame] = []
        self.problems: dict[str, dict] = {}
        self.info: dict = {}
        self.mtime = None

    def of(self, *kinds):
        return [e for e in self.ev if e.kind in kinds]


def _num(m, g=1, cast=float):
    try:
        return cast(m.group(g))
    except (TypeError, ValueError, AttributeError):
        return None


def parse_frames_line(t: float, m: str, prev_t: float | None) -> Frame:
    vals = {}
    for k, rx in FRAME_RE.items():
        mm = rx.search(m)
        if mm:
            vals[k] = mm.group(1)
    fr = Frame(t=t, t0=0.0, dur=5.0)
    for k, v in vals.items():
        if k == "mode":
            fr.mode = v
        elif k in ("stale", "norender", "skipped", "timedout", "deferred", "layers", "lock_max_us"):
            setattr(fr, k, int(float(v)))
        else:
            setattr(fr, k, float(v))
    # The window runs from the previous line; a gap longer than a few windows is a break (the first line after it is 5 s).
    if prev_t is not None and 0.5 <= t - prev_t <= 12.0:
        fr.t0 = prev_t
    else:
        fr.t0 = t - 5.0
    fr.dur = max(t - fr.t0, 0.1)
    return fr


def explain_disarm(reason: str, detail: str) -> str:
    """English for the failing guard of a DISARMED line (builds from 29fec71 on name it)."""
    if not detail:
        return reason
    m = GUARD_RE.search(detail)
    if m:
        addr = int(m.group(2), 16)
        got = int(m.group(4), 16)
        want = int(m.group(6), 16)
        neg = bool(m.group(5))
        name = ADDR_NAMES.get(addr, f"0x{addr:X}")
        lab = ""
        if "status" in name and got in STATUS_NAMES:
            lab = f" ({STATUS_NAMES[got]})"
        return f"{name} reads {got}{lab}, wants {'not ' if neg else ''}{want}"
    m = RELEASE_RE.search(detail)
    if m:
        off = int(m.group(2), 16)
        got = int(m.group(3), 16)
        what = {0x08: "grabbed (stance 5)", 0xBA0: "down, dead or turned", 0xBB8: "down, dead or turned"}.get(off)
        return f"released by the profile: record +0x{off:X} reads {got}" + (f" = {what}" if what else "")
    return detail


def parse_emulog(path: str) -> Log:
    log = Log(path)
    try:
        log.mtime = dt.datetime.fromtimestamp(os.path.getmtime(path))
    except OSError:
        pass
    prev_frames_t = None
    cur_bindings = None      # the "Controller bindings (...)" block being read
    last_t = 0.0
    seen_first = False
    info = log.info
    info.setdefault("gpu", None)
    with open(path, encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            raw = raw.rstrip("\r\n")
            lm = LINE_RE.match(raw)
            if not lm:
                continue
            log.n_lines += 1
            t = float(lm.group(1))
            if not seen_first:
                log.t_first = t
                seen_first = True
            last_t = max(last_t, t)
            msg = lm.group(2)
            vr = msg.startswith("(VR) ")
            m = msg[5:] if vr else msg
            ev = log.ev

            def add(kind, **d):
                ev.append(Ev(t, kind, d, m))

            # ---------------- general facts
            if not vr:
                if m.startswith("PenguinScreen2 ") and "version" not in info:
                    info["version"] = m.split(" ", 1)[1]
                elif m.startswith("Disc changed to "):
                    info["disc"] = m[len("Disc changed to "):].rstrip(".")
                elif m.startswith("  Serial: "):
                    info["serial"] = m.split(":", 1)[1].strip()
                elif m.startswith("UpdateVSyncRate: Mode Changed to "):
                    info["video_mode"] = m.rsplit(" ", 1)[1].rstrip(".")
                elif m.startswith("  GPU ") and "=" in m and not info.get("gpu"):
                    info["gpu"] = m.split("=", 1)[1].strip()
                elif m.startswith("  Processor ") and "=" in m:
                    info["cpu"] = m.split("=", 1)[1].strip()
                elif m.startswith("Surface refresh rate:") and "surface_hz" not in info:
                    mm = re.search(r"([\d.]+) Hz", m)
                    if mm:
                        info["surface_hz"] = float(mm.group(1))
                elif m.startswith("OSD [VRStereoScene]: "):
                    mm = re.match(r"OSD \[VRStereoScene\]: Stereo scene: ([^(]+?)\s*(?:\(|$)", m)
                    if mm:
                        add("scene", name=mm.group(1).strip())
                    elif m.startswith("OSD [VRStereoScene]: Stereo: first person"):
                        add("scene", name="first person")
            else:
                if m.startswith("Frames:"):
                    fr = parse_frames_line(t, m, prev_frames_t)
                    prev_frames_t = t
                    log.frames.append(fr)
                elif m.startswith("Session state:"):
                    mm = STATE_RE.search(m)
                    if mm:
                        add("state", frm=mm.group(1), to=mm.group(2),
                            ms_after=_num(mm, 3, int), ms_in=_num(mm, 4, int))
                elif m.startswith("Eyes:"):
                    d = {"ipd": _num(EYES_RE.search(m))}
                    mm = re.search(r"left fov L/R/U/D ([-\d./]+)", m)
                    if mm:
                        d["left"] = [float(x) for x in mm.group(1).split("/")]
                    mm = re.search(r"right ([-\d./]+);", m)
                    if mm:
                        d["right"] = [float(x) for x in mm.group(1).split("/")]
                    mm = re.search(r"union H \+-([\d.]+) V \+([\d.]+)/(-?[\d.]+)", m)
                    if mm:
                        d["union_h"] = float(mm.group(1))
                        d["union_up"] = float(mm.group(2))
                        d["union_down"] = float(mm.group(3))
                    mm = re.search(r"display ([\d.]+) Hz", m)
                    if mm:
                        d["hz"] = float(mm.group(1))
                    add("eyes", **d)
                elif m.startswith("OpenXR runtime:"):
                    info["runtime"] = m.split(":", 1)[1].strip()
                elif m.startswith("HMD:"):
                    info["hmd"] = m.split(":", 1)[1].strip()
                elif m.startswith("Runtime supports Vulkan"):
                    info["vulkan"] = m[len("Runtime supports Vulkan"):].strip()
                elif m.startswith("Headset eye images: recommended"):
                    mm = re.search(r"(\d+)x(\d+) per eye", m)
                    if mm:
                        info["eye_images"] = (int(mm.group(1)), int(mm.group(2)))
                elif m.startswith("Frame submission:"):
                    info["submission"] = m.split(":", 1)[1].strip()
                elif m.startswith("xrCreateInstance failed"):
                    info["no_vr"] = m
                    add("no_vr")
                elif m.startswith("XR session created"):
                    add("xr_created")
                elif m.startswith("Session began"):
                    add("xr_began")
                elif m.startswith("XR session destroyed"):
                    add("xr_destroyed")
                elif m.startswith("GS device destroyed"):
                    add("vr_teardown")
                elif m.startswith("Session/runtime lost"):
                    add("runtime_lost")
                elif m.startswith("Steam Frame controller interaction:"):
                    info["frame_ctrl"] = m.split(":", 1)[1].strip().rstrip(".")
                elif m.startswith("Suggested ") and "controller bindings for" in m:
                    mm = re.match(r"Suggested (\d+) controller bindings for (\S+)", m)
                    if mm:
                        info.setdefault("suggested", []).append((mm.group(2), int(mm.group(1))))
                elif m.startswith("Controller bindings ("):
                    mm = re.match(r"Controller bindings \((.*?)\):", m)
                    cur_bindings = {"t": t, "why": mm.group(1) if mm else "", "left": None, "right": None}
                    info.setdefault("bindings", []).append(cur_bindings)
                elif m.startswith("Left hand profile:") and cur_bindings is not None:
                    cur_bindings["left"] = m.split(":", 1)[1].strip()
                elif m.startswith("Right hand profile:") and cur_bindings is not None:
                    cur_bindings["right"] = m.split(":", 1)[1].strip()
                elif m.startswith("Interaction profile changed"):
                    add("profile_changed")
                elif m.startswith(("Left controller:", "Right controller:")):
                    add("controller", side=m.split()[0].lower(), what=m.split(":", 1)[1].strip())
                elif m.startswith("Both controllers live"):
                    add("controller", side="both", what="live")
                elif m.startswith("The session started unfocused"):
                    add("started_unfocused")
                elif m.startswith("CameraDriver: ARMED"):
                    mm = re.search(r"CRC ([0-9A-Fa-f]+)", m)
                    add("armed", crc=mm.group(1) if mm else "")
                elif m.startswith("CameraDriver: DISARMED"):
                    mm = re.search(r"DISARMED \(CRC ([0-9A-Fa-f]+)\)\s*[-\u2014\u2013]+\s*(.*?)\.?\s*$", m)
                    crc, body = (mm.group(1), mm.group(2)) if mm else ("", m.split("DISARMED", 1)[1])
                    reason, detail = body, ""
                    for kr in KNOWN_DISARM:
                        if body.startswith(kr):
                            reason = kr
                            detail = body[len(kr):].lstrip(": ").strip()
                            break
                    add("disarmed", crc=crc, reason=reason, detail=detail, why=explain_disarm(reason, detail))
                elif m.startswith("CameraDriver: local character grabbed"):
                    add("grab_start")
                elif m.startswith("CameraDriver: no longer grabbed"):
                    mm = re.search(r"after ([\d.]+) s", m)
                    add("grab_end", secs=_num(mm))
                elif m.startswith("CameraDriver: network mode"):
                    info["net_mode"] = m.split("network mode", 1)[1].strip().rstrip(".")
                elif m.startswith("First-person FOV:"):
                    mm = re.search(r"([\d.]+) deg vertical", m)
                    if mm:
                        info["fp_fov"] = float(mm.group(1))
                elif m.startswith("HUD blink"):
                    mm = re.match(r"HUD blink \((.*?)\)", m)
                    add("blink", reason=mm.group(1) if mm else "")
                elif m.startswith("Screen: ") and not m.startswith("Screen: tablet (menu):"):
                    s = m[len("Screen: "):].lower()
                    mode = ("first person" if s.startswith("first person") else "world" if s.startswith("world")
                            else "tablet" if s.startswith("tablet") else s.rstrip("."))
                    add("screen", mode=mode)
                elif m.startswith("Screen: tablet (menu):"):
                    add("screen", mode="tablet")
                elif m.startswith("Screen frames:"):
                    add("screen_frames", mode=m.split(":", 1)[1].strip())
                elif m.startswith("XR swapchain created"):
                    mm = re.search(r"\(eye (\d)\): (\d+)x(\d+)", m)
                    add("swapchain", eye=_num(mm, 1, int), w=_num(mm, 2, int), h=_num(mm, 3, int))
                elif m.startswith("MenuPointer: item screen open"):
                    mm = re.search(r"cursor at cell (-?\d+)", m)
                    add("item_open", cell=_num(mm, 1, int), on="point-to-select on" in m)
                elif m.startswith("MenuPointer: item screen closed"):
                    mm = re.search(r"\((\d+) steer\(s\), (\d+) confirm\(s\)", m)
                    add("item_close", steers=_num(mm, 1, int) or 0, confirms=_num(mm, 2, int) or 0)
                elif m.startswith("MenuPointer: a menu was up without"):
                    add("menu_other")
                elif m.startswith("MenuPointer: submenu open"):
                    add("submenu_open")
                elif m.startswith("MenuPointer: submenu closed"):
                    add("submenu_close")
                elif m.startswith("MenuPointer: confirm passed"):
                    add("menu_confirm")
                elif m.startswith("Pouches:") and "squeezed" in m:
                    add("pouch", nothing="nothing done" in m)
                elif m.startswith("Feedback: shot"):
                    more = sum(int(x) for x in MORE_RE.findall(m))
                    mm = re.match(r"Feedback: shot \((.+?)\), ammo (\d+) -> (\d+), (\w+) hand", m)
                    if mm:
                        add("shot", weapon=mm.group(1), a=int(mm.group(2)), b=int(mm.group(3)), hand=mm.group(4), more=more)
                    else:
                        mm = re.match(r"Feedback: shot, counter (\d+) -> (\d+), (\w+) hand", m)
                        add("shot", weapon="?", a=_num(mm, 1, int), b=_num(mm, 2, int), hand=mm.group(3) if mm else "", more=more)
                elif m.startswith("Feedback: dry fire"):
                    add("dry", more=sum(int(x) for x in MORE_RE.findall(m)))
                elif m.startswith("Feedback: reload"):
                    mm = re.search(r"ammo (\d+) -> (\d+)", m)
                    add("reload", a=_num(mm, 1, int), b=_num(mm, 2, int), more=sum(int(x) for x in MORE_RE.findall(m)))
                elif m.startswith("Feedback: hurt"):
                    mm = re.search(r"HP (\d+) -> (\d+) of (\d+)", m)
                    add("hurt", a=_num(mm, 1, int), b=_num(mm, 2, int), mx=_num(mm, 3, int),
                        more=sum(int(x) for x in MORE_RE.findall(m)))
                elif m.startswith("Feedback: heartbeat"):
                    mm = re.search(r"HP (\d+) of (\d+) \((\d+)%\)", m)
                    add("heartbeat", hp=_num(mm, 1, int), mx=_num(mm, 2, int), pct=_num(mm, 3, int))
                elif m.startswith("Feedback: game rumble on pad"):
                    mm = re.search(r"large ([\d.]+) small ([\d.]+)", m)
                    big = (_num(mm, 1) or 0) > 0 or (_num(mm, 2) or 0) > 0
                    add("rumble", on=big, more=sum(int(x) for x in MORE_RE.findall(m)))
                elif m.startswith("Hands: the right hand shows"):
                    mm = re.match(r"Hands: the right hand shows (?:an? )?(.+?) for (.+?) \(item (\d+)\)", m)
                    if mm:
                        add("weapon", wkind=mm.group(1), name=mm.group(2), item=int(mm.group(3)))
                    else:
                        add("weapon", wkind="pistol", name="(from the stance)", item=None)
                elif m.startswith("HUD toast:"):
                    mm = re.match(r'HUD toast: "(.*)" \(([\d.]+) s\)', m)
                    add("toast", text=mm.group(1) if mm else m, secs=_num(mm, 2))
                elif m.startswith("HUD: wrist card live:"):
                    mm = re.search(r"HP (\d+)/(\d+).*?virus ([\d.]+)%.*?bleeding (yes|no)", m)
                    if mm:
                        add("wrist", hp=int(mm.group(1)), mx=int(mm.group(2)), virus=float(mm.group(3)), bleeding=mm.group(4))
                elif m.startswith("Reference space recentered"):
                    add("recenter")
                elif m.startswith("Screen re-anchored"):
                    add("reanchor")
                elif m.startswith("Controller actions inactive"):
                    add("actions", on=False)
                elif m.startswith("Controller actions active"):
                    add("actions", on=True)
                elif re.search(r"(?i)\bpick(ed)? ?up\b|\bobtained\b|take it\b", m) and "Pouches:" not in m:
                    add("pickup")
                elif re.search(r"(?i)reload", m) and not m.startswith(("Spatial controls", "Feedback")):
                    add("reload_other")

            # ---------------- problems (warnings and errors), whatever the line is
            if PROBLEM_RE.search(m) and not IGNORE_RE.search(m) and not re.match(r"OSD \[\w+\]: (Loaded|Saved|Saving)", m):
                text = re.sub(r"^OSD \[[^\]]*\]:\s*", "", m)
                text = re.sub(r"^\[Unsafe Settings\]\s*", "", text)
                text = re.sub(r"^\(VR\)\s*", "", text)
                key = re.sub(r"[-+]?\d+(\.\d+)?", "#", text)[:90]
                slot = log.problems.get(key)
                if slot is None:
                    log.problems[key] = dict(t=t, last=t, count=1, text=text,
                                             sev="error" if ERROR_RE.search(m) else "warning")
                else:
                    slot["count"] += 1
                    slot["last"] = t
    log.t_last = last_t
    return log


# --------------------------------------------------------------------------------------------------------------------
# SteamVR logs
# --------------------------------------------------------------------------------------------------------------------

MONTHS = {m: i + 1 for i, m in enumerate(["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"])}
SVR_TS = re.compile(r"^\w{3} (\w{3}) (\d{1,2}) (\d{4}) (\d\d):(\d\d):(\d\d)\.(\d{3}) \[(\w+)\] - (.*)$")
XR_TRANS = re.compile(r"Transitioning XrSession from XR_SESSION_STATE_(\w+) to XR_SESSION_STATE_(\w+)")


def parse_svr_line(line: str):
    m = SVR_TS.match(line)
    if not m:
        return None
    mon = MONTHS.get(m.group(1))
    if not mon:
        return None
    try:
        when = dt.datetime(int(m.group(3)), mon, int(m.group(2)), int(m.group(4)), int(m.group(5)), int(m.group(6)),
                           int(m.group(7)) * 1000)
    except ValueError:
        return None
    return when, m.group(8), m.group(9)


def svr_files(folder: str, base: str):
    """The current log and its rotated .previous copy, whichever exist."""
    out = []
    for suffix in ("", ".previous"):
        p = os.path.join(folder, base + suffix + ".txt")
        if os.path.isfile(p):
            out.append(p)
    return out


def read_svr(path: str, lo: dt.datetime | None, hi: dt.datetime | None):
    """Yield (when, level, msg) of the lines inside [lo, hi] (all when None)."""
    try:
        if lo is not None and dt.datetime.fromtimestamp(os.path.getmtime(path)) < lo:
            return
    except OSError:
        return
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            r = parse_svr_line(line.rstrip("\r\n"))
            if r is None:
                continue
            if lo is not None and r[0] < lo:
                continue
            if hi is not None and r[0] > hi:
                continue
            yield r


def find_xr_files(folder: str, app: str):
    """xrclient_<app>.txt and its rotated .previous copy (SteamVR keeps every run of the app in one file)."""
    return sorted(set(glob.glob(os.path.join(folder, f"xrclient_{app}*.txt"))))


def _epoch(w: dt.datetime) -> float:
    return (w - dt.datetime(2000, 1, 1)).total_seconds()


def align_clock(log: Log, folder: str | None, app: str, t0_arg: str | None) -> tuple[Clock, list]:
    """Fit emulog t = 0 to wall time. Returns the clock and SteamVR's session-state transitions (for the timeline).

    Each emulog "Session state: A -> B" at t is paired with SteamVR's "Transitioning XrSession from A to B" at wall time w;
    every pair proposes t0 = w - t, and the proposal that explains most of the app's transitions (each within 0.4 s of a
    SteamVR line of the same kind) wins. t0 is then the median of the pairs' differences (the method of
    research/2026-10-12-steamvr-endframe-research.md). Runs that share a state sequence are told apart by the log's file time."""
    if t0_arg:
        for fmt in ("%Y-%m-%d %H:%M:%S.%f", "%Y-%m-%d %H:%M:%S", "%Y-%m-%dT%H:%M:%S.%f", "%Y-%m-%dT%H:%M:%S"):
            try:
                return Clock(dt.datetime.strptime(t0_arg, fmt), "given with --t0", 0.0), []
            except ValueError:
                pass
        print(f"session_report: could not read --t0 {t0_arg!r}; ignoring it", file=sys.stderr)
    emu = [(e.t, e.d["frm"], e.d["to"]) for e in log.ev if e.kind == "state"]
    xr = []
    if folder:
        for p in find_xr_files(folder, app):
            for when, _lvl, msg in read_svr(p, None, None):
                mm = XR_TRANS.search(msg)
                if mm:
                    xr.append((when, mm.group(1), mm.group(2)))
    xr.sort()
    hint = log.mtime - dt.timedelta(seconds=log.t_last) if log.mtime else None   # a finished log ends about at its file time
    if emu and xr:
        by_pair: dict[tuple, list] = {}
        for w, a, b in xr:
            by_pair.setdefault((a, b), []).append(_epoch(w))
        for lst in by_pair.values():
            lst.sort()

        def residuals(c: float):
            out = []
            for t, a, b in emu:
                lst = by_pair.get((a, b))
                if not lst:
                    continue
                want = c + t
                i = bisect.bisect_left(lst, want)
                near = [lst[j] for j in (i - 1, i) if 0 <= j < len(lst)]
                if near:
                    w = min(near, key=lambda x: abs(x - want))
                    if abs(w - want) <= 0.4:
                        out.append(w - want)
            return out

        cands = sorted({round(w - t, 2) for t, a, b in emu for w in by_pair.get((a, b), [])})
        scored = []
        for c in cands:
            r = residuals(c)
            if r:
                scored.append((len(r), c, r))
        if scored:
            top = max(s[0] for s in scored)
            groups: list[list] = []
            for s in sorted((s for s in scored if s[0] == top), key=lambda s: s[1]):
                if groups and s[1] - groups[-1][-1][1] <= 1.0:
                    groups[-1].append(s)
                else:
                    groups.append([s])
            note = ""
            if len(groups) > 1 and hint is not None:
                h = _epoch(hint)
                groups.sort(key=lambda g: abs(g[0][1] - h))
                note = f"{len(groups)} SteamVR runs share this state sequence; the log's file time picked the nearest"
            elif len(groups) > 1:
                groups.sort(key=lambda g: -g[0][1])
                note = f"{len(groups)} SteamVR runs share this state sequence; the latest was taken"
            g = groups[0]
            c = g[len(g) // 2][1]
            r = residuals(c)
            c2 = c + statistics.median(r)
            r2 = residuals(c2)
            spread = max(abs(x - statistics.median(r2)) for x in r2) if r2 else 0.0
            how = f"fitted on {len(r2)} of {len(emu)} session-state lines matched in SteamVR's xrclient log"
            if note:
                how += f" ({note})"
            return Clock(dt.datetime(2000, 1, 1) + dt.timedelta(seconds=c2), how, spread), xr
    return Clock(None), xr


@dataclass
class SvrEvent:
    t: float            # emulog seconds
    when: dt.datetime
    src: str
    kind: str
    text: str
    secs: float = 0.0


def load_steamvr(folder: str, clock: Clock, log: Log, xr_trans) -> dict:
    """Events from SteamVR's logs inside the session, in emulog time."""
    out = dict(events=[], link=[], totals=None, stalls=[], stall_count=0, resets=0, encoder_resets=0, files=[], missing=[],
               acquire_fail=0)
    if not clock.ok:
        return out
    lo = clock.wall(log.t_first) - dt.timedelta(seconds=30)
    hi = clock.wall(log.t_last) + dt.timedelta(seconds=90)
    xr_created = next((e.t for e in log.ev if e.kind == "xr_created"), None)
    t_vr = xr_created if xr_created is not None else log.t_first
    evs: list[SvrEvent] = out["events"]

    def add(when, src, kind, text, secs=0.0):
        evs.append(SvrEvent(clock.from_wall(when), when, src, kind, text, secs))

    def files(base):
        fs = svr_files(folder, base)
        if not fs:
            out["missing"].append(base + ".txt")
        else:
            out["files"] += [os.path.basename(f) for f in fs]
        return fs

    for p in files("vrserver"):
        for when, _lvl, msg in read_svr(p, lo, hi):
            t = clock.from_wall(when)
            if "EVTYPE_OPEN_DASHBOARD" in msg:
                add(when, "vrserver", "dash_request", "Steam button: dashboard requested (EVTYPE_OPEN_DASHBOARD)")
            elif re.match(r"0 - entering standby", msg):
                add(when, "vrserver", "standby", "headset entering standby")
            elif re.match(r"0 - leaving standby", msg):
                add(when, "vrserver", "standby_end", "headset leaving standby")
            elif "Starting encoder reset" in msg:
                if t >= t_vr - 5:
                    out["encoder_resets"] += 1
            elif "Reset video stream because" in msg:
                if t >= t_vr - 5:
                    out["resets"] += 1
    for p in files("vrclient_vrcompositor"):
        for when, _lvl, msg in read_svr(p, lo, hi):
            mm = re.match(r"(Add|Remove)SystemBehaviorFlag SystemBehaviorFlag_(\w+)", msg)
            if mm:
                flag = mm.group(2)
                on = mm.group(1) == "Add"
                if flag == "LaserMouse":
                    add(when, "compositor", "dash_shown" if on else "dash_hidden",
                        "dashboard shown (laser mouse on)" if on else "dashboard hidden")
                elif flag == "DriverRequestsApplicationPause":
                    add(when, "compositor", "app_pause" if on else "app_resume",
                        "driver asks the app to pause" if on else "driver lets the app run again")
    for p in files("driver_vrlink"):
        for when, _lvl, msg in read_svr(p, lo, hi):
            mm = re.match(r"THROTTLE EVENT: (\d+)", msg)
            if mm:
                out["link"].append((clock.from_wall(when), int(mm.group(1)) / 1000.0))
            elif msg.startswith("Connection inactive"):
                add(when, "vrlink", "link_inactive", "Steam Link connection inactive")
            elif msg.startswith("Connection active"):
                add(when, "vrlink", "link_active", "Steam Link connection active")
    tot_re = re.compile(r"Total\.+\s+(\d+) presents\.\s+(\d+) dropped\.\s+(\d+) reprojected")
    for p in files("vrcompositor"):
        for when, lvl, msg in read_svr(p, lo, None):
            mt = tot_re.match(msg)
            if mt and clock.from_wall(when) > t_vr:
                out["totals"] = (int(mt.group(1)), int(mt.group(2)), int(mt.group(3)), when)
            if when > hi:
                continue
            if "WaitForAcquire timed out" in msg:
                mm = re.search(r"\[(\d+) messages suppressed\]", msg)
                n = 1 + (int(mm.group(1)) if mm else 0)
                out["stall_count"] += n
                out["stalls"].append((clock.from_wall(when), n))
            elif "AcquireSync FAILED" in msg:
                out["acquire_fail"] += 1
                add(when, "compositor", "acquire_fail", "compositor AcquireSync failed (WAIT_TIMEOUT)")
    # SteamVR's own view of the session state: only transitions the app did not log itself
    app_states = [(e.t, e.d["frm"], e.d["to"]) for e in log.ev if e.kind == "state"]
    for when, a, b in xr_trans:
        if a == b or not (lo <= when <= hi):
            continue
        t = clock.from_wall(when)
        if any(f == a and to == b and abs(ts - t) < 1.5 for ts, f, to in app_states):
            continue
        add(when, "xrclient", "xr_state", f"SteamVR session state {a} -> {b}")
    # before the emulator's VR session: keep the dashboard context (it explains an unfocused start), drop the rest
    evs[:] = [e for e in evs if e.t >= t_vr - 30]
    evs.sort(key=lambda e: e.t)
    return out


# --------------------------------------------------------------------------------------------------------------------
# session recorder (state.csv)
# --------------------------------------------------------------------------------------------------------------------

def find_session_dir(arg: str | None) -> str | None:
    if not arg:
        return None
    if arg == "latest":
        if not os.path.isdir(DEFAULT_SESSIONS):
            return None
        subs = [os.path.join(DEFAULT_SESSIONS, d) for d in os.listdir(DEFAULT_SESSIONS)
                if os.path.isfile(os.path.join(DEFAULT_SESSIONS, d, "state.csv"))]
        return max(subs, key=os.path.getmtime) if subs else None
    return arg


@dataclass
class Rec:
    rows: list = field(default_factory=list)       # dicts with float t and ints
    offset: float | None = None                    # emulog t = recorder t + offset
    how: str = ""
    matched: int = 0
    total: int = 0
    path: str = ""
    stamp: dt.datetime | None = None
    ts: list = field(default_factory=list)


def load_csv(folder: str) -> Rec | None:
    p = os.path.join(folder, "state.csv")
    if not os.path.isfile(p):
        return None
    rec = Rec(path=p)
    m = re.match(r"(\d{8})-(\d{6})", os.path.basename(os.path.normpath(folder)))
    if m:
        try:
            rec.stamp = dt.datetime.strptime(m.group(1) + m.group(2), "%Y%m%d%H%M%S")
        except ValueError:
            pass
    with open(p, encoding="utf-8", errors="replace", newline="") as fh:
        rd = csv.DictReader(fh)
        for r in rd:
            try:
                row = dict(t=float(r["t"]))
                for k in ("status", "menu", "overlay", "stance", "action", "hp", "hp_max"):
                    row[k] = int(float(r.get(k) or 0))
                for k in ("heading", "x", "y", "z", "cam_yaw", "cam_pitch"):
                    row[k] = float(r.get(k) or 0.0)
                row["serial"] = r.get("serial", "")
            except (KeyError, ValueError, TypeError):
                continue   # a truncated last line
            rec.rows.append(row)
    if not rec.rows:
        return None
    rec.ts = [r["t"] for r in rec.rows]
    # one-sample glitches (the recorder briefly reads another record: hp 3000, hp_max 3000, 2560...) are median-filtered
    for key in ("hp", "hp_max"):
        vals = [r[key] for r in rec.rows]
        for i in range(1, len(vals) - 1):
            a, b, c = vals[i - 1], vals[i], vals[i + 1]
            rec.rows[i][key + "_f"] = sorted((a, b, c))[1]
        rec.rows[0][key + "_f"] = vals[0]
        rec.rows[-1][key + "_f"] = vals[-1]
    return rec


def align_csv(rec: Rec, log: Log, clock: Clock):
    """Find the offset (emulog t = csv t + offset): the log's arm/disarm changes against the recorder's status 2 + overlay 3."""
    rows = rec.rows
    pred = [(r["status"] == 2 and r["overlay"] == 3) for r in rows]
    csv_tr = []   # (t, +1 rise / -1 fall)
    for i in range(1, len(rows)):
        if pred[i] != pred[i - 1]:
            csv_tr.append((rows[i]["t"], 1 if pred[i] else -1))
    emu_tr = []
    for e in log.ev:
        if e.kind == "armed":
            emu_tr.append((e.t, 1))
        elif e.kind == "disarmed":
            emu_tr.append((e.t, -1))
    rec.total = len(emu_tr)
    best = None
    if emu_tr and csv_tr:
        cts = {1: [t for t, s in csv_tr if s == 1], -1: [t for t, s in csv_tr if s == -1]}
        cands = set()
        for te, s in emu_tr:
            for tc in cts[s]:
                d = te - tc
                if -5.0 <= d <= 120.0:
                    cands.add(round(d, 2))
        for off in sorted(cands):
            diffs = []
            for te, s in emu_tr:
                lst = cts[s]
                i = bisect.bisect_left(lst, te - off - 0.6)
                if i < len(lst) and te - off - 0.6 <= lst[i] <= te - off + 0.3:
                    diffs.append(te - lst[i])
            if diffs:
                key = (len(diffs), -statistics.pstdev(diffs) if len(diffs) > 1 else 0.0)
                if best is None or key > best[0]:
                    best = (key, diffs)
    if best and best[0][0] >= 2 and best[0][0] >= max(2, len(emu_tr) // 3):
        diffs = best[1]
        rec.offset = statistics.median(diffs)
        rec.matched = len(diffs)
        rec.how = (f"lined up on {len(diffs)} of {len(emu_tr)} arm/disarm changes against the recorder's scenario status "
                   f"(+/- 0.3 s)")
        return
    # fallback: the folder name is the recorder's start time
    if rec.stamp and clock.ok:
        off = clock.from_wall(rec.stamp) + 0.5
        # the recorder starts within a minute of the emulator; anything else is a folder from another run
        if -10.0 <= off <= 90.0:
            rec.offset = off
            rec.how = "from the folder's time stamp and the fitted wall clock (+/- 1 s)"
            return
        rec.how = (f"this folder (started {rec.stamp:%H:%M:%S}) does not belong to this log: it started "
                   f"{off:.0f} s after the emulator; recorder data is not used")
        return
    rec.how = "could not be lined up with the emulog (no arm/disarm changes to match): recorder times are not used"


# --------------------------------------------------------------------------------------------------------------------
# analysis
# --------------------------------------------------------------------------------------------------------------------

def level_class(f: Frame) -> str | None:
    if f.end_mean is None:
        return None
    if f.end_mean >= TH["parked_ms"]:
        return "parked"
    if f.end_mean > TH["end_ms"]:
        return "hold"
    return "ok"


def contiguous(a: Frame, b: Frame) -> bool:
    return b.t0 - a.t <= 1.0


def analyze_frames(log: Log, t_vr: float | None = None) -> dict:
    fr = log.frames
    out: dict = dict(windows=fr, episodes=[], spikes=[], minutes=[])
    if not fr:
        return out
    hz_list = [f.hz for f in fr if f.hz > 0]
    eyes = [e for e in log.ev if e.kind == "eyes" and e.d.get("hz")]
    hz = eyes[0].d["hz"] if eyes else (statistics.median(hz_list) if hz_list else 0.0)
    pal = log.info.get("video_mode") == "PAL"
    gs_target = PAL_HZ if pal else NTSC_HZ
    out["hz"] = hz
    out["gs_target"] = gs_target
    # the windows right after the session was created hold the game's own start-up load: counted, never flagged
    startup = {i for i, f in enumerate(fr) if t_vr is not None and f.t <= t_vr + 8.0}
    out["startup"] = sorted(startup)
    have_end = [f for f in fr if f.end_mean is not None]
    out["have_end"] = bool(have_end)
    W = [f.dur for f in fr]
    out["sub_mean"] = wmean([f.sub for f in fr], W)
    out["gs_mean"] = wmean([f.gs for f in fr], W)
    out["gs_pct"] = 100.0 * out["gs_mean"] / gs_target if gs_target else 0.0
    out["resent_mean"] = wmean([f.resent for f in fr], W)
    if have_end:
        ws = [f.sub * f.dur for f in have_end]
        out["end_mean"] = wmean([f.end_mean for f in have_end], ws)
        out["end_max"] = max(f.end_max or 0.0 for f in have_end)
        out["end_median_window"] = statistics.median([f.end_mean for f in have_end])
        locks = [f for f in have_end if f.lock_ms_s is not None]
        out["lock_mean"] = wmean([f.lock_ms_s for f in locks], [f.dur for f in locks]) if locks else None
        out["lock_max_ms"] = max((f.lock_max_us or 0) for f in locks) / 1000.0 if locks else None
    out["stale_total"] = sum(f.stale for f in fr)
    out["skipped_total"] = sum(f.skipped for f in fr)
    out["timedout_total"] = sum(f.timedout for f in fr)
    out["deferred_total"] = sum(f.deferred for f in fr)
    out["norender_total"] = sum(f.norender for f in fr)
    out["covered"] = sum(W)
    out["t_start"] = fr[0].t0
    out["t_end"] = fr[-1].t
    out["modes"] = sorted({f.mode for f in fr if f.mode})

    # ---- xrEndFrame plateaus ("hold": a display period inside xrEndFrame) and parking (10 Hz)
    base_pool = [f.end_mean for f in have_end if level_class(f) == "ok"]
    baseline = statistics.median(base_pool) if base_pool else 0.0
    out["baseline_end"] = baseline
    runs = []
    cur = None
    for i, f in enumerate(fr):
        c = level_class(f)
        if c in ("hold", "parked"):
            if cur and cur["cls"] == c and contiguous(fr[i - 1], f):
                cur["i1"] = i
            else:
                cur = dict(cls=c, i0=i, i1=i)
                runs.append(cur)
        else:
            cur = None
    in_episode = set()
    for r in runs:
        i0, i1 = r["i0"], r["i1"]
        seg = fr[i0: i1 + 1]
        start, end = seg[0].t0, seg[-1].t
        dur = end - start
        inner = seg[1:-1] if len(seg) >= 3 else seg
        level = statistics.median([f.end_mean for f in inner])
        base_b = fr[i0 - 1].end_mean if i0 > 0 and contiguous(fr[i0 - 1], fr[i0]) and level_class(fr[i0 - 1]) == "ok" else None
        base_a = fr[i1 + 1].end_mean if i1 + 1 < len(fr) and contiguous(fr[i1], fr[i1 + 1]) and level_class(fr[i1 + 1]) == "ok" else None
        # where inside the edge windows the slow frames start/stop, from the window means (the research's method)
        if base_b is not None and level - base_b > 1.0 and len(seg) >= 2:
            frac = min(1.0, max(0.0, (seg[0].end_mean - base_b) / (level - base_b)))
            start = seg[0].t - frac * seg[0].dur
        if base_a is not None and level - base_a > 1.0 and len(seg) >= 2:
            frac = min(1.0, max(0.0, (seg[-1].end_mean - base_a) / (level - base_a)))
            end = seg[-1].t0 + frac * seg[-1].dur
        dur = max(end - start, 0.0)
        if dur < TH["end_secs"]:
            continue
        for k in range(i0, i1 + 1):
            in_episode.add(k)
        ws = [f.sub * f.dur for f in seg]
        ep = dict(
            kind="parked" if r["cls"] == "parked" else "plateau", t0=start, t1=end, dur=dur, level=level,
            end_mean=wmean([f.end_mean for f in seg], ws), end_max=max(f.end_max or 0 for f in seg),
            sub=wmean([f.sub for f in seg], [f.dur for f in seg]), gs=wmean([f.gs for f in seg], [f.dur for f in seg]),
            lock=wmean([f.lock_ms_s or 0 for f in seg], [f.dur for f in seg]),
            gs_min=min(f.gs for f in seg), gs_max=max(f.gs for f in seg),
            skipped=sum(f.skipped for f in seg), deferred=sum(f.deferred for f in seg),
            windows=len(seg), open_end=(i1 == len(fr) - 1), i0=i0, i1=i1,
            base=base_b if base_b is not None else baseline,
        )
        out["episodes"].append(ep)
    # ---- spikes: slow single frames outside an episode
    spike_idx = []
    for i, f in enumerate(fr):
        if i in in_episode or i in startup or f.end_max is None:
            continue
        if f.end_max > TH["spike_ms"] or (level_class(f) == "hold"):
            spike_idx.append(i)
    out["spikes"] = [dict(t=fr[i].t, t0=fr[i].t0, mx=fr[i].end_max, mean=fr[i].end_mean, sub=fr[i].sub, gs=fr[i].gs,
                          lock=fr[i].lock_ms_s) for i in spike_idx]
    # ---- GS slowdown / submission shortfall windows, and how many sit inside an xrEndFrame episode
    gs_low = [i for i, f in enumerate(fr) if f.gs < TH["gs_frac"] * gs_target and i not in startup]
    out["gs_low"] = gs_low
    out["gs_low_inside"] = sum(1 for i in gs_low if i in in_episode)
    out["gs_low_dur"] = sum(fr[i].dur for i in gs_low)
    sub_low = [i for i, f in enumerate(fr) if hz and f.sub < TH["sub_frac"] * hz and i not in startup]
    out["sub_low"] = sub_low
    out["sub_low_inside"] = sum(1 for i in sub_low if i in in_episode)
    lock_hi = [i for i, f in enumerate(fr) if (f.lock_ms_s or 0) >= TH["lock_ms"] and i not in startup]
    out["lock_hi"] = lock_hi
    out["lock_hi_inside"] = sum(1 for i in lock_hi if i in in_episode)
    out["gs_worst"] = min(fr, key=lambda f: f.gs) if fr else None
    # what "normal" looked like outside the episodes (the first window holds the start-up load)
    norm = [f for i, f in enumerate(fr) if i not in in_episode and i not in startup]
    out["norm_windows"] = len(norm)
    if norm:
        nw = [f.dur for f in norm]
        out["gs_norm"] = wmean([f.gs for f in norm], nw)
        out["sub_norm"] = wmean([f.sub for f in norm], nw)
        ne = [f for f in norm if f.end_mean is not None]
        out["end_norm"] = wmean([f.end_mean for f in ne], [f.sub * f.dur for f in ne]) if ne else None
        nl = [f for f in norm if f.lock_ms_s is not None]
        out["lock_norm"] = wmean([f.lock_ms_s for f in nl], [f.dur for f in nl]) if nl else None
    out["in_episode"] = sorted(in_episode)
    # runs of consecutive GS-slow windows outside episodes (what to look at when the slowdown has no xrEndFrame cause)
    gruns = []
    cur = None
    for i in gs_low:
        if i in in_episode:
            cur = None
            continue
        if cur and i == cur["i1"] + 1:
            cur["i1"] = i
        else:
            cur = dict(i0=i, i1=i)
            gruns.append(cur)
    out["gs_runs_outside"] = [dict(t0=fr[g["i0"]].t0, t1=fr[g["i1"]].t, gs=min(fr[k].gs for k in range(g["i0"], g["i1"] + 1)))
                              for g in gruns]
    # ---- per minute
    mins: dict[int, list] = {}
    for i, f in enumerate(fr):
        mins.setdefault(int(f.t // 60), []).append(i)
    for m in sorted(mins):
        idx = mins[m]
        seg = [fr[i] for i in idx]
        w = [f.dur for f in seg]
        he = [f for f in seg if f.end_mean is not None]
        flags = []
        if any(i in in_episode for i in idx):
            ep = [e for e in out["episodes"] if e["i0"] <= idx[-1] and e["i1"] >= idx[0]]
            flags.append("parked" if any(e["kind"] == "parked" for e in ep) else "plateau")
        if any(i in spike_idx for i in idx):
            flags.append("spike")
        if any(i in startup for i in idx) and not flags:
            flags.append("start-up")
        if any(i in gs_low for i in idx) and "plateau" not in flags and "parked" not in flags:
            flags.append("gs-slow")
        if any(i in sub_low for i in idx) and "plateau" not in flags and "parked" not in flags:
            flags.append("sub-low")
        out["minutes"].append(dict(
            m=m, t0=max(m * 60.0, seg[0].t0), t1=min((m + 1) * 60.0, seg[-1].t), sub=wmean([f.sub for f in seg], w),
            gs=wmean([f.gs for f in seg], w),
            end_mean=wmean([f.end_mean for f in he], [f.sub * f.dur for f in he]) if he else None,
            end_max=max((f.end_max or 0.0) for f in he) if he else None,
            lock=wmean([f.lock_ms_s for f in he if f.lock_ms_s is not None], [f.dur for f in he if f.lock_ms_s is not None])
            if any(f.lock_ms_s is not None for f in he) else None,
            flags=flags, n=len(seg)))
    return out


def analyze_focus(log: Log, svr: dict, clock: Clock, t_end: float) -> dict:
    sts = log.of("state")
    out = dict(periods=[], losses=[], by_state={}, started_unfocused=bool(log.of("started_unfocused")))
    if not sts:
        return out
    for i, e in enumerate(sts):
        end = sts[i + 1].t if i + 1 < len(sts) else t_end
        out["periods"].append(dict(state=e.d["to"], t0=e.t, t1=max(end, e.t)))
    for p in out["periods"]:
        out["by_state"][p["state"]] = out["by_state"].get(p["state"], 0.0) + (p["t1"] - p["t0"])
    sv = [e for e in svr.get("events", []) if e.kind in ("dash_request", "dash_shown", "standby", "link_inactive")]
    for i, e in enumerate(sts):
        if e.d["frm"] == "FOCUSED" and e.d["to"] != "FOCUSED":
            back = next((s for s in sts[i + 1:] if s.d["to"] == "FOCUSED"), None)
            near = [s for s in sv if -2.5 <= s.t - e.t <= 1.0 and s.kind in ("dash_request", "dash_shown")]
            toast = next((x for x in log.of("toast") if abs(x.t - e.t) < 1.5 and "paused" in x.d["text"].lower()), None)
            out["losses"].append(dict(t=e.t, to=e.d["to"], back=back.t if back else None,
                                      dur=(back.t - e.t) if back else None,
                                      sv=[(s.t, s.text) for s in near], toast=bool(toast)))
    return out


def csv_cause(rec: Rec, t_emu: float, kind: str) -> str:
    """What the recorder saw just before a disarm (kind 'end') or arm (kind 'start')."""
    if rec is None or rec.offset is None or not rec.rows:
        return ""
    tc = t_emu - rec.offset
    ts = rec.ts
    hi = bisect.bisect_right(ts, tc + 0.15)
    lo = bisect.bisect_left(ts, tc - 1.6)
    seg = rec.rows[max(lo - 1, 0): hi]
    if len(seg) < 2:
        return ""
    notes = []
    for a, b in zip(seg, seg[1:]):
        if a["status"] != b["status"]:
            na = STATUS_NAMES.get(b["status"])
            notes.append(f"scenario status {a['status']} -> {b['status']}" + (f" ({na})" if na else ""))
        if a["overlay"] != b["overlay"]:
            notes.append(f"overlay {a['overlay']} -> {b['overlay']}")
        if a["stance"] != b["stance"] and 5 in (a["stance"], b["stance"]):
            notes.append("grabbed" if b["stance"] == 5 else "grab ended")
        if a["hp_f"] != b["hp_f"] and b["hp_f"] == 0 and a["hp_f"] > 0:
            notes.append("HP reached 0")
    if notes:
        return "; ".join(dict.fromkeys(notes))
    last = seg[-1]
    return f"no change seen (status {last['status']}, overlay {last['overlay']}, stance {last['stance']})"


def analyze_arm(log: Log, rec: Rec | None, t_end: float, t_vr_start: float | None) -> dict:
    periods = []
    cur = None
    for e in log.ev:
        if e.kind == "armed":
            if cur is None:
                cur = dict(start=e.t)
        elif e.kind == "disarmed":
            if cur is not None:
                cur.update(end=e.t, reason=e.d["reason"], detail=e.d["detail"], why=e.d["why"], open=False)
                periods.append(cur)
                cur = None
            else:
                periods.append(dict(start=None, end=e.t, reason=e.d["reason"], detail=e.d["detail"], why=e.d["why"], open=False))
    if cur is not None:
        cur.update(end=t_end, reason="", detail="", why="still armed at the end of the log", open=True)
        periods.append(cur)
    real = [p for p in periods if p["start"] is not None]
    for i, p in enumerate(real):
        p["dur"] = p["end"] - p["start"]
        nxt = real[i + 1] if i + 1 < len(real) else None
        p["gap"] = (nxt["start"] - p["end"]) if nxt and not p["open"] else None
        p["csv_end"] = csv_cause(rec, p["end"], "end") if not p["open"] else ""
        p["csv_start"] = csv_cause(rec, p["start"], "start")
    out = dict(periods=real)
    out["count"] = len(real)
    out["armed_total"] = sum(p["dur"] for p in real)
    out["first"] = real[0]["start"] if real else None
    if real:
        out["short"] = [p for p in real if p["dur"] < TH["short_arm_secs"]]
        out["median"] = statistics.median([p["dur"] for p in real])
        out["longest"] = max(real, key=lambda p: p["dur"])
    reasons: dict[str, int] = {}
    for p in real:
        if p["open"]:
            continue
        key = p["reason"] or "?"
        reasons[key] = reasons.get(key, 0) + 1
    out["reasons"] = reasons
    named = [p for p in real if p.get("detail")]
    out["named_guard"] = len(named)
    # what the recorder saw at each disarm, grouped
    causes: dict[str, int] = {}
    for p in real:
        c = p.get("csv_end") or ""
        if not c or p["open"]:
            continue
        key = ("scenario status left 'in game' (door / cutscene)" if "scenario status" in c else
               "grabbed" if "grabbed" in c else "HP reached 0" if "HP reached 0" in c else
               "overlay changed" if "overlay" in c else "nothing visible in the recorder")
        causes[key] = causes.get(key, 0) + 1
    out["csv_causes"] = causes
    out["vr_start"] = t_vr_start
    return out


def analyze_menus(log: Log, t_end: float, rec=None) -> dict:
    scenes = log.of("scene")
    periods = []
    in_menu = None
    for e in scenes:
        if e.d["name"] == "menu":
            if in_menu is None:
                in_menu = e.t
        elif in_menu is not None:
            periods.append(dict(t0=in_menu, t1=e.t, open=False))
            in_menu = None
    if in_menu is not None:
        periods.append(dict(t0=in_menu, t1=t_end, open=True))
    if not scenes:
        # builds without the scene OSD line: a world-locked screen that begins while first person is armed
        armed = False
        start = None
        for e in log.ev:
            if e.kind == "armed":
                armed = True
            elif e.kind == "disarmed":
                armed = False
                if start is not None:
                    periods.append(dict(t0=start, t1=e.t, open=False))
                    start = None
            elif e.kind == "screen":
                if e.d["mode"] == "world" and armed and start is None:
                    start = e.t
                elif e.d["mode"] != "world" and start is not None:
                    periods.append(dict(t0=start, t1=e.t, open=False))
                    start = None
    items_open = log.of("item_open")
    items_close = log.of("item_close")
    others = log.of("menu_other")
    subs = log.of("submenu_open")
    for p in periods:
        lo, hi = p["t0"] - 0.2, p["t1"] + 0.5
        io = [e for e in items_open if lo <= e.t <= hi]
        ic = [e for e in items_close if lo <= e.t <= hi]
        oth = [e for e in others if lo <= e.t <= hi]
        p["dur"] = p["t1"] - p["t0"]
        if io:
            p["kind"] = "item screen"
            p["steers"] = sum(e.d["steers"] for e in ic)
            p["confirms"] = sum(e.d["confirms"] for e in ic)
            p["submenus"] = len([e for e in subs if lo <= e.t <= hi])
            p["pointer"] = all(e.d.get("on") for e in io)
        elif oth:
            p["kind"] = "map / pause"
        else:
            p["kind"] = "menu (kind not logged)"
        # the recorder's menu byte (item screen, map and pause set it; take-it prompts and hand-overs do not, see
        # research/2026-10-12-ui-flags-research.md) separates a prompt from a menu when the log cannot
        if rec is not None and rec.offset is not None and rec.rows:
            a = bisect.bisect_left(rec.ts, p["t0"] - rec.offset - 0.4)
            b = bisect.bisect_right(rec.ts, p["t1"] - rec.offset + 0.4)
            seg = rec.rows[a:b]
            if seg:
                p["csv_menu"] = max(r["menu"] for r in seg)
                if p["csv_menu"] == 0 and p["kind"] != "item screen":
                    p["kind"] = "take-it prompt / game screen"
    out = dict(periods=periods)
    out["count"] = len(periods)
    out["by_kind"] = {}
    for p in periods:
        k = out["by_kind"].setdefault(p["kind"], dict(n=0, secs=0.0))
        k["n"] += 1
        k["secs"] += p["dur"]
    out["item_opens"] = len(items_open)
    out["pointer_off"] = len([e for e in items_open if not e.d.get("on")])
    out["confirms"] = len(log.of("menu_confirm"))
    out["mismatch"] = [p for p in log.problems.values() if "no longer matches" in p["text"] or "did not answer" in p["text"]]
    return out


def csv_menu_changes(rec, t0: float, t1: float):
    """How many times the recorder saw the game's menu byte change between two emulog times (None without a recorder)."""
    if rec is None or rec.offset is None or not rec.rows:
        return None
    lo = bisect.bisect_left(rec.ts, t0 - rec.offset)
    hi = bisect.bisect_right(rec.ts, t1 - rec.offset)
    seg = rec.rows[max(lo - 1, 0): hi]
    return sum(1 for a, b in zip(seg, seg[1:]) if a["menu"] != b["menu"])


def analyze_screens(log: Log, t0: float, t_end: float, menus: dict | None = None, arm: dict | None = None, rec=None) -> dict:
    sw = log.of("screen")
    bl = log.of("blink")
    out = dict(switches=len(sw), blinks=len(bl))
    dur = max(t_end - t0, 1.0)
    out["per_min_switch"] = len(sw) / (dur / 60.0)
    out["per_min_blink"] = len(bl) / (dur / 60.0)
    reasons: dict[str, int] = {}
    for e in bl:
        reasons[e.d["reason"]] = reasons.get(e.d["reason"], 0) + 1
    out["blink_reasons"] = reasons
    modes: dict[str, int] = {}
    for e in sw:
        modes[e.d["mode"]] = modes.get(e.d["mode"], 0) + 1
    out["modes"] = modes
    # time per screen mode
    spent: dict[str, float] = {}
    for i, e in enumerate(sw):
        end = sw[i + 1].t if i + 1 < len(sw) else t_end
        spent[e.d["mode"]] = spent.get(e.d["mode"], 0.0) + max(end - e.t, 0.0)
    out["spent"] = spent
    # per-minute bins from the first minute of the VR session
    nb = int(dur // 60) + 1
    bs = [0] * nb
    bb = [0] * nb
    for e in sw:
        k = int((e.t - t0) // 60)
        if 0 <= k < nb:
            bs[k] += 1
    for e in bl:
        k = int((e.t - t0) // 60)
        if 0 <= k < nb:
            bb[k] += 1
    out["bins_switch"] = bs
    out["bins_blink"] = bb
    out["bin_t0"] = t0
    # sliding 60 s windows
    ts = [e.t for e in sw]
    worst = (0, None)
    for i, a in enumerate(ts):
        n = bisect.bisect_left(ts, a + 60.0) - i
        if n > worst[0]:
            worst = (n, a)
    out["worst_switch"] = worst
    tb = [e.t for e in bl]
    worstb = (0, None)
    for i, a in enumerate(tb):
        n = bisect.bisect_left(tb, a + 60.0) - i
        if n > worstb[0]:
            worstb = (n, a)
    out["worst_blink"] = worstb
    # bounces: a mode change followed by another within a few seconds
    bounces = []
    for a, b in zip(sw, sw[1:]):
        if b.t - a.t < TH["bounce_secs"]:
            bounces.append((a.t, b.t - a.t, a.d["mode"], b.d["mode"]))
    out["bounces"] = bounces
    out["quick"] = [b for b in bounces if b[1] < 1.0]
    # a quick toggle that the recorder also saw in the game's own menu byte is a menu press, not a screen glitch
    out["quick_backed"] = 0
    out["quick_unbacked"] = []
    for b in out["quick"]:
        n = csv_menu_changes(rec, b[0] - 0.8, b[0] + b[1] + 0.6)
        if n is None:
            out["quick_unbacked"].append(b)
        elif n >= 1:
            out["quick_backed"] += 1
        else:
            out["quick_unbacked"].append(b)
    out["has_rec"] = rec is not None and rec.offset is not None
    # switches with no menu opening/closing and no arm/disarm next to them: the ones that would be flicker
    anchors = []
    for p in (menus or {}).get("periods", []):
        anchors += [p["t0"], p["t1"]]
    for e in log.ev:
        if e.kind in ("armed", "disarmed"):
            anchors.append(e.t)
    anchors.sort()
    unexplained = []
    for e in sw:
        i = bisect.bisect_left(anchors, e.t)
        near = [anchors[j] for j in (i - 1, i) if 0 <= j < len(anchors)]
        if not near or min(abs(e.t - a) for a in near) > 0.9:
            unexplained.append(e)
    out["unexplained"] = [(e.t, e.d["mode"]) for e in unexplained]
    # why the flat world screen came up: a menu, first person being taken away, or neither
    menu_t0 = sorted(p["t0"] for p in (menus or {}).get("periods", []))
    dis_t = sorted(e.t for e in log.ev if e.kind == "disarmed")

    def near_any(lst, t):
        i = bisect.bisect_left(lst, t)
        return any(0 <= j < len(lst) and abs(lst[j] - t) <= 0.9 for j in (i - 1, i))

    out["world_menu"] = out["world_disarm"] = out["world_other"] = 0
    for e in sw:
        if e.d["mode"] != "world":
            continue
        if near_any(menu_t0, e.t):
            out["world_menu"] += 1
        elif near_any(dis_t, e.t):
            out["world_disarm"] += 1
        else:
            out["world_other"] += 1
    # scene OSD changes (stereo scene menu/base/first person), the same count seen from the stereo side
    out["scene_changes"] = len(log.of("scene"))
    out["swapchains"] = len(log.of("swapchain"))
    # how long a blink-to-screen switch takes (blink then Screen:), for a hint that the blink hides the switch
    return out


def analyze_play(log: Log, rec: Rec | None) -> dict:
    out: dict = {}
    shots = log.of("shot")
    out["shots_logged"] = len(shots)
    out["shots"] = len(shots) + sum(e.d.get("more", 0) for e in shots)
    by_w: dict[str, int] = {}
    for e in shots:
        by_w[e.d["weapon"]] = by_w.get(e.d["weapon"], 0) + 1 + e.d.get("more", 0)
    out["shots_by_weapon"] = by_w
    dry = log.of("dry")
    out["dry"] = len(dry) + sum(e.d.get("more", 0) for e in dry)
    hurts = log.of("hurt")
    out["hurts"] = len(hurts) + sum(e.d.get("more", 0) for e in hurts)
    out["damage_logged"] = sum(max((e.d["a"] or 0) - (e.d["b"] or 0), 0) for e in hurts)
    out["hurt_events"] = hurts
    hb = log.of("heartbeat")
    out["heartbeats"] = len(hb)
    runs = []
    cur = None
    for e in hb:
        if cur and e.t - cur["t1"] <= 6.0:
            cur["t1"] = e.t
            cur["min"] = min(cur["min"], e.d["hp"])
            cur["n"] += 1
        else:
            cur = dict(t0=e.t, t1=e.t, min=e.d["hp"], mx=e.d["mx"], n=1)
            runs.append(cur)
    out["low_hp_runs"] = runs
    rl = log.of("reload")
    out["reloads"] = rl
    out["reload_n"] = len(rl) + sum(e.d.get("more", 0) for e in rl)
    gs, ge = log.of("grab_start"), log.of("grab_end")
    grabs = []
    for s in gs:
        end = next((e for e in ge if e.t >= s.t), None)
        grabs.append(dict(t=s.t, secs=end.d["secs"] if end else None))
    out["grabs"] = grabs
    out["weapons"] = [(e.t, e.d["name"], e.d["wkind"]) for e in log.of("weapon")]
    rumbles = log.of("rumble")
    out["rumble_on"] = len([e for e in rumbles if e.d["on"]]) + sum(e.d.get("more", 0) for e in rumbles if e.d["on"])
    toasts: dict[str, dict] = {}
    for e in log.of("toast"):
        s = toasts.setdefault(e.d["text"], dict(n=0, first=e.t, last=e.t))
        s["n"] += 1
        s["last"] = e.t
    out["toasts"] = toasts
    out["toast_list"] = [(e.t, e.d["text"]) for e in log.of("toast")]
    out["pouch"] = len(log.of("pouch"))
    out["pouch_nothing"] = len([e for e in log.of("pouch") if e.d.get("nothing")])
    out["recenters"] = len(log.of("recenter"))
    out["pickup_lines"] = log.of("pickup")
    out["reload_other"] = log.of("reload_other")
    out["wrist_start"] = log.of("wrist")[0] if log.of("wrist") else None
    # ---- the recorder's view
    if rec is not None and rec.rows:
        rows = rec.rows
        out["rec_beyond"] = 0.0
        if rec.offset is not None:
            # the recording can outlast a copied log: only the part inside the log is read here
            out["rec_beyond"] = max(rows[-1]["t"] + rec.offset - log.t_last, 0.0)
            rows = [r for r in rows if r["t"] + rec.offset <= log.t_last + 2.0] or rows
        out["rec_rows"] = len(rows)
        out["rec_span"] = rows[-1]["t"] - rows[0]["t"]
        deaths = []
        revived = []
        zero_since = None
        for i in range(1, len(rows)):
            a, b = rows[i - 1], rows[i]
            if a["hp_f"] > 0 and b["hp_f"] == 0 and b["hp_max_f"] > 0:
                zero_since = i
                deaths.append(b["t"])
            elif zero_since is not None and b["hp_f"] > 0 and a["hp_f"] == 0:
                revived.append(b["t"])
                zero_since = None
        out["rec_zero_hp"] = deaths
        out["rec_revived"] = revived
        hp = [r["hp_f"] for r in rows if r["hp_max_f"] > 0 and r["status"] == 2]
        mx = [r["hp_max_f"] for r in rows if r["hp_max_f"] > 0]
        out["rec_hp_min"] = min(hp) if hp else None
        out["rec_hp_max"] = max(mx) if mx else None
        dts = [b["t"] - a["t"] for a, b in zip(rows, rows[1:])]
        step = statistics.median(dts) if dts else 0.26
        gaps = [(a["t"], b["t"] - a["t"]) for a, b in zip(rows, rows[1:]) if b["t"] - a["t"] > max(2.0, 6 * step)]
        out["rec_gaps"] = gaps
        out["rec_step"] = step

        def runs_of(pred, min_samples=2):
            res = []
            start = None
            for i, r in enumerate(rows):
                if pred(r):
                    if start is None:
                        start = i
                elif start is not None:
                    if i - start >= min_samples:
                        res.append((rows[start]["t"], rows[i - 1]["t"] + step))
                    start = None
            if start is not None and len(rows) - start >= min_samples:
                res.append((rows[start]["t"], rows[-1]["t"] + step))
            return res

        out["rec_menu_runs"] = runs_of(lambda r: r["menu"] == 1)
        out["rec_aim_secs"] = sum(1 for r in rows if r["stance"] == 2) * step
        out["rec_grab_runs"] = runs_of(lambda r: r["stance"] == 5)
        out["rec_pickup_runs"] = runs_of(lambda r: r["action"] == 9)
        out["rec_status_secs"] = {}
        for r in rows:
            out["rec_status_secs"][r["status"]] = out["rec_status_secs"].get(r["status"], 0.0) + step
        # still stretches: no position change, no menu, status 2
        idle = []
        start = None
        last = None
        for i, r in enumerate(rows):
            pos = (round(r["x"]), round(r["y"]), round(r["z"]), round(r["heading"]))
            if start is None:
                start, last = i, pos
            elif pos != last or r["menu"] == 1 or r["status"] != 2:
                if rows[i - 1]["t"] - rows[start]["t"] >= 45.0 and rows[start]["status"] == 2:
                    idle.append((rows[start]["t"], rows[i - 1]["t"]))
                start, last = i, pos
        if start is not None and rows[-1]["t"] - rows[start]["t"] >= 45.0 and rows[start]["status"] == 2:
            idle.append((rows[start]["t"], rows[-1]["t"]))
        out["rec_idle"] = idle
    return out


# --------------------------------------------------------------------------------------------------------------------
# putting it together
# --------------------------------------------------------------------------------------------------------------------

class R:
    """Everything the renderers need."""


def safe(r: "R", name: str, fn, default, *args):
    """Run one analysis step; a line shape it does not expect costs that section, not the report."""
    try:
        return fn(*args)
    except Exception as exc:   # noqa: BLE001
        r.errors.append(f"{name}: {type(exc).__name__}: {exc}")
        return default


def empty_play() -> dict:
    return dict(shots=0, shots_logged=0, shots_by_weapon={}, dry=0, hurts=0, damage_logged=0, hurt_events=[], heartbeats=0,
                low_hp_runs=[], reloads=[], reload_n=0, grabs=[], weapons=[], rumble_on=0, toasts={}, toast_list=[], pouch=0,
                pouch_nothing=0, recenters=0, pickup_lines=[], reload_other=[], wrist_start=None)


def empty_screens() -> dict:
    return dict(switches=0, blinks=0, per_min_switch=0.0, per_min_blink=0.0, blink_reasons={}, modes={}, spent={}, bins_switch=[],
                bins_blink=[], bin_t0=0.0, worst_switch=(0, None), worst_blink=(0, None), bounces=[], quick=[], quick_backed=0,
                quick_unbacked=[], has_rec=False, unexplained=[], scene_changes=0, swapchains=0, world_menu=0, world_disarm=0,
                world_other=0)


def dur_text(d: float) -> str:
    """'15 s', '2m06s (126 s)'."""
    return fmt_dur(d) + (f" ({d:.0f} s)" if d >= 60 else "")


def mmss(t: float) -> str:
    m, s = divmod(int(round(t)), 60)
    return f"{m}:{s:02d}"


def build(emulog: str, session_dir: str | None, steam_dir: str | None, app: str, t0_arg: str | None) -> R:
    r = R()
    r.errors = []
    r.log = parse_emulog(emulog)
    log = r.log
    r.session_dir = session_dir
    r.steam_dir = steam_dir
    r.clock, xr = align_clock(log, steam_dir, app, t0_arg)
    clock = r.clock
    r.svr = load_steamvr(steam_dir, clock, log, xr) if steam_dir and clock.ok else dict(
        events=[], link=[], totals=None, stalls=[], stall_count=0, resets=0, encoder_resets=0, files=[], missing=[],
        acquire_fail=0)
    r.rec = load_csv(session_dir) if session_dir else None
    r.rec_note = ""
    if session_dir and r.rec is None:
        r.rec_note = f"{os.path.join(session_dir, 'state.csv')}: missing, empty or unreadable; not used"
    r.session_emulog = None
    if session_dir:
        cand = os.path.join(session_dir, "emulog.txt")
        try:
            if (os.path.isfile(cand) and os.path.abspath(cand) != os.path.abspath(emulog)
                    and os.path.getsize(cand) > os.path.getsize(emulog) * 1.05):
                r.session_emulog = cand
        except OSError:
            pass
    if r.rec:
        align_csv(r.rec, log, clock)
        if r.rec.offset is None:
            r.rec_unused = r.rec
            r.rec = None
    xr_created = next((e.t for e in log.ev if e.kind == "xr_created"), None)
    first_state = next((e.t for e in log.ev if e.kind == "state"), None)
    # Frames lines come from a running VR session even when the lines before them were cut off
    r.vr_started = xr_created is not None or first_state is not None or bool(log.frames)
    r.t_vr_start = (xr_created if xr_created is not None else first_state if first_state is not None
                    else log.frames[0].t0 if log.frames else None)
    end_ev = [e.t for e in log.ev if e.kind in ("xr_destroyed", "vr_teardown")]
    r.shutdown = bool(end_ev)
    r.t_vr_end = min(end_ev) if end_ev else log.t_last
    r.t_end = log.t_last
    r.F = safe(r, "frame stats", analyze_frames, dict(windows=[], episodes=[], spikes=[], minutes=[]), log, r.t_vr_start)
    r.focus = safe(r, "focus", analyze_focus, dict(periods=[], losses=[], by_state={}, started_unfocused=False),
                   log, r.svr, clock, r.t_vr_end)
    r.arm = safe(r, "first person", analyze_arm, dict(periods=[], count=0, armed_total=0.0, first=None, reasons={}, named_guard=0,
                                                       csv_causes={}, short=[]), log, r.rec, r.t_vr_end, r.t_vr_start)
    r.menus = safe(r, "menus", analyze_menus, dict(periods=[], count=0, by_kind={}, item_opens=0, pointer_off=0, confirms=0,
                                                    mismatch=[]), log, r.t_vr_end, r.rec)
    r.screens = safe(r, "screens", analyze_screens, empty_screens(), log,
                     r.t_vr_start if r.t_vr_start is not None else 0.0, r.t_vr_end, r.menus, r.arm, r.rec)
    r.play = safe(r, "gameplay", analyze_play, empty_play(), log, r.rec)
    r.findings = safe(r, "findings", make_findings, [], r)
    for e in r.errors:
        r.findings.insert(0, ("warn", f"Part of the analysis failed ({clip(e, 160)}); the other sections are unaffected."))
    return r


def eyes_text(e: Ev) -> list[str]:
    d = e.d
    out = []
    if d.get("ipd"):
        out.append(f"IPD {d['ipd']:.1f} mm")
    lr = []
    for side in ("left", "right"):
        v = d.get(side)
        if v and len(v) == 4:
            lr.append(f"{side} eye {abs(v[0]) + abs(v[1]):.1f} x {abs(v[2]) + abs(v[3]):.1f} deg "
                      f"(L/R/U/D {v[0]:g}/{v[1]:g}/{v[2]:g}/{v[3]:g})")
    out += lr
    if d.get("union_h"):
        out.append(f"both eyes: +-{d['union_h']:g} deg wide, +{d.get('union_up', 0):g}/{d.get('union_down', 0):g} deg tall")
    return out


def pretty_profile(p: str | None) -> str:
    if not p:
        return "?"
    return p.replace("/interaction_profiles/", "")


def nearest_state_event(log: Log, t: float, lo=-2.0, hi=6.0, to=None):
    for e in log.of("state"):
        if lo <= e.t - t <= hi and (to is None or e.d["to"] in to):
            return e
    return None


def make_findings(R_: R) -> list:
    """(severity, text) in plain English. Severity: bad, warn, info, ok."""
    log, clock, F, rec = R_.log, R_.clock, R_.F, R_.rec
    T = clock.at
    out = []

    def add(sev, text):
        out.append((sev, text))

    if not R_.vr_started:
        why = log.info.get("no_vr")
        if why:
            add("bad", "VR never started in this log: " + clip(why.replace("—", "-"), 160))
        else:
            add("warn", "This log has no VR session (no OpenXR session lines): a flat or fake-head run, or a log from before VR started.")
        return out
    hz = F.get("hz") or 90.0
    per = 1000.0 / hz
    # ---- xrEndFrame plateaus and parking
    for ep in F.get("episodes", []):
        near_end = [e for e in R_.svr.get("events", []) if abs(e.t - ep["t1"]) <= 2.0 and e.kind != "xr_state"]
        near_end += [e for e in log.ev if abs(e.t - ep["t1"]) <= 2.0 and e.kind in
                     ("state", "actions", "armed", "disarmed", "screen", "item_open", "item_close", "grab_start", "grab_end",
                      "recenter", "toast")]
        if ep["kind"] == "plateau":
            loss = next((l for l in R_.focus.get("losses", []) if -2.0 <= ep["t0"] - l["t"] <= 3.0), None)
            s = (f"xrEndFrame held at about {ep['level']:.1f} ms (one {per:.1f} ms display period) for {dur_text(ep['dur'])}, "
                 f"{T(ep['t0'])} to {T(ep['t1'])}; normal is {F.get('end_norm') or F.get('baseline_end', 0):.1f} ms. "
                 f"Submitted frames were {ep['sub']:.1f}/s, but new GS frames fell to {ep['gs']:.1f}/s "
                 f"(normal {F.get('gs_norm', 0):.1f}) and queue-lock waits rose to {ep['lock']:.0f} ms/s (normal {F.get('lock_norm') or 0:.0f}).")
            if loss:
                gap = ep["t0"] - loss["t"]
                s += (f" It began within {abs(gap):.1f} s of the session going FOCUSED -> {loss['to']} at {T(loss['t'])}"
                      if abs(gap) >= 0.05 else f" It began as the session went FOCUSED -> {loss['to']} at {T(loss['t'])}")
                if loss["sv"]:
                    s += " (SteamVR logged: " + "; ".join(x[1] for x in loss["sv"][:2]) + ")"
                s += "."
            else:
                s += " No focus change in the app log sits near its start."
            if not ep["open_end"]:
                logs = "either log" if R_.svr.get("files") else "the emulog (no SteamVR log was read)"
                s += (" It ended by itself" + (f"; nothing in {logs} marks the end." if not near_end else
                                                f"; near the end: {near_end[0].text if hasattr(near_end[0], 'text') else near_end[0].msg}."))
            else:
                s += " It was still going at the end of the log."
            add("bad", s)
        else:
            st = nearest_state_event(log, ep["t0"], -3.0, 8.0, ("SYNCHRONIZED", "IDLE", "STOPPING", "READY"))
            s = (f"The app was parked from {T(ep['t0'])} "
                 f"{'to the end of the log' if ep['open_end'] else 'to ' + T(ep['t1'])} ({fmt_dur(ep['dur'])}): xrEndFrame about "
                 f"{ep['level']:.0f} ms and only {ep['sub']:.1f} frames/s submitted against a {hz:.0f} Hz display, which matches the runtime "
                 f"throttling an app it counts as not visible (headset in standby or off the head).")
            if st:
                s += f" Session state {st.d['frm']} -> {st.d['to']} at {T(st.t)}."
            sv = [e for e in R_.svr.get("events", []) if ep["t0"] - 3 <= e.t <= ep["t0"] + 12 and e.kind in ("standby", "link_inactive")]
            if sv:
                s += " SteamVR: " + "; ".join(f"{e.text} at {T(e.t)}" for e in sv) + "."
            s += f" The game kept running (GS {ep['gs']:.0f}/s)."
            if rec is not None and rec.offset is not None:
                lo, hi = ep["t0"] - rec.offset, ep["t1"] - rec.offset
                z = [t for t in R_.play.get("rec_zero_hp", []) if lo <= t <= hi]
                gr = [g for g in R_.play.get("rec_grab_runs", []) if lo <= g[0] <= hi]
                idle = [i for i in R_.play.get("rec_idle", []) if i[0] <= hi and i[1] >= lo]
                bits = []
                if idle:
                    i = max(idle, key=lambda x: min(x[1], hi) - max(x[0], lo))
                    bits.append(f"the character stood still for {fmt_dur(min(i[1], hi) - max(i[0], lo))}")
                if gr:
                    bits.append(f"it was grabbed at {T(gr[0][0] + rec.offset)}")
                if z:
                    bits.append(f"its HP reached 0 at {T(z[0] + rec.offset)}")
                if bits:
                    s += " Recorder: " + ", ".join(bits) + "."
            add("bad", s)
    # ---- spikes
    sp = F.get("spikes", [])
    real_sp = sp
    if real_sp:
        w = max(real_sp, key=lambda x: x["mx"])
        add("warn", f"{len(real_sp)} short xrEndFrame spike window(s) over {TH['spike_ms']:g} ms outside the plateaus; worst "
                    f"{w['mx']:.1f} ms in the 5 s ending {T(w['t'])}. A frame that spends that long in xrEndFrame can miss its {per:.1f} ms slot.")
    # ---- GS
    if F.get("windows"):
        pct = 100.0 * F.get("gs_norm", F["gs_mean"]) / F["gs_target"]
        tail = ""
        if F["gs_low"]:
            tail = (f" {len(F['gs_low'])} of {len(F['windows'])} windows ran under {TH['gs_frac'] * F['gs_target']:.1f}/s; "
                    f"{F['gs_low_inside']} of them sit inside the plateau/parking episodes above, so the slowdown follows the xrEndFrame "
                    f"stall (the pacer holds the queue lock while it waits).")
            if F["gs_runs_outside"]:
                g = F["gs_runs_outside"][0]
                tail += f" Outside them: {len(F['gs_runs_outside'])} slow run(s), first {T(g['t0'])} ({g['gs']:.1f}/s)."
        sev = "ok" if not F["gs_low"] else ("warn" if F["gs_low_inside"] < len(F["gs_low"]) - 1 else "info")
        add(sev, f"GS speed {F['gs_mean']:.1f}/s overall ({F['gs_pct']:.1f}% of {F['gs_target']:.2f}); outside the episodes "
                 f"{F.get('gs_norm', 0):.1f}/s ({pct:.1f}%).{tail}")
        if F["timedout_total"]:
            add("bad", f"{F['timedout_total']} GS image copies timed out (the pacer did not release an image in time).")
    # ---- focus
    fo = R_.focus
    if fo.get("started_unfocused"):
        first_focus = next((p["t0"] for p in fo["periods"] if p["state"] == "FOCUSED"), None)
        add("info", "The session started unfocused (SteamVR dashboard open)"
                    + (f" and went FOCUSED {first_focus:.1f} s into the log" if first_focus is not None
                       else " and never reached FOCUSED in this log")
                    + "; controllers were paused until then.")
    if fo.get("losses"):
        L = fo["losses"]
        parts = []
        for l in L[:6]:
            back = f"back after {l['dur']:.1f} s" if l["dur"] is not None else "never came back in this log"
            parts.append(f"{T(l['t'])} -> {l['to']} ({back})")
        add("info", f"{len(L)} focus loss(es) after the start, each a FOCUSED -> VISIBLE/SYNCHRONIZED change"
                    f"{' that SteamVR logged as a Steam-button dashboard request' if all(l['sv'] for l in L) else ''}: "
                    + "; ".join(parts) + ".")
    # ---- first person
    A = R_.arm
    if A["count"]:
        vr_len = max(R_.t_vr_end - (R_.t_vr_start or 0.0), 1.0)
        s = (f"First person armed {A['count']} time(s), {fmt_dur(A['armed_total'])} in all ({100.0 * A['armed_total'] / vr_len:.0f}% of the VR "
             f"session), first at {T(A['first'])}; median stretch {fmt_dur(A['median'])}, longest {fmt_dur(A['longest']['dur'])}.")
        ended = [p for p in A["periods"] if not p["open"]]
        if ended:
            if A["named_guard"]:
                gd: dict[str, int] = {}
                for p in ended:
                    gd[p["why"]] = gd.get(p["why"], 0) + 1
                s += " It was taken away: " + "; ".join(f"{n}x {w}" for w, n in sorted(gd.items(), key=lambda kv: -kv[1])) + "."
            else:
                rs = ", ".join(f"{n}x '{k}'" for k, n in A["reasons"].items())
                s += f" It was taken away {len(ended)} time(s): {rs}; this build's DISARMED line does not say which guard failed."
                if A["csv_causes"]:
                    s += " The recorder shows what changed: " + "; ".join(
                        f"{n}x {k}" for k, n in sorted(A["csv_causes"].items(), key=lambda kv: -kv[1])) + "."
        if A["short"]:
            s += f" {len(A['short'])} stretch(es) lasted under {TH['short_arm_secs']:g} s (flapping)."
        add("warn" if A["short"] else "info", s)
    elif R_.vr_started:
        add("warn", "First person never armed in this log" + (f" (the last DISARMED says: {log.of('disarmed')[-1].d['why']})" if log.of("disarmed") else "") + ".")
    # ---- screens
    S = R_.screens
    if S["switches"] or S["blinks"]:
        busy = S["worst_switch"]
        s = (f"{S['blinks']} comfort blinks ({S['per_min_blink']:.1f}/min) and {S['switches']} screen-mode switches "
             f"({S['per_min_switch']:.1f}/min), {R_.menus['count']} menu(s) in all")
        if busy[1] is not None:
            s += f"; the busiest 60 s had {busy[0]} switches (from {T(busy[1])})"
        s += "."
        sev = "info"
        if S["unexplained"]:
            sev = "warn"
            s += (f" {len(S['unexplained'])} switch(es) had no menu open/close or arm/disarm next to them (possible flicker), "
                  f"first {T(S['unexplained'][0][0])}.")
        if S["quick"]:
            if S["has_rec"]:
                s += f" {len(S['quick'])} quick reversal(s) within 1 s; {S['quick_backed']} of them match the game's own menu byte flipping (button presses)"
                if S["quick_unbacked"]:
                    sev = "warn"
                    s += (f", {len(S['quick_unbacked'])} do not (first {T(S['quick_unbacked'][0][0])}: a screen glitch, or a prompt "
                          f"that leaves the menu byte alone)")
                s += "."
            else:
                s += (f" {len(S['quick'])} quick reversal(s) within 1 s (button presses or a glitch; pass --session to check them "
                      f"against the game's menu byte).")
        add(sev, s)
    # ---- warnings
    errs = [p for p in log.problems.values() if p["sev"] == "error"]
    warns = [p for p in log.problems.values() if p["sev"] == "warning"]
    late = [p for p in warns if p["t"] > 15]
    early = [p for p in warns if p["t"] <= 15]
    if errs:
        add("bad", f"{len(errs)} distinct error line(s): " + "; ".join(f"{clip(p['text'], 110)} (x{p['count']}, first {T(p['t'])})" for p in errs[:3]) + ".")
    for p in late[:4]:
        add("warn", f"Warning: {clip(p['text'], 170)} (x{p['count']}, first {T(p['t'])}).")
    if len(late) > 4:
        add("warn", f"{len(late) - 4} more distinct warning line(s) after start-up: see WARNINGS AND ERRORS.")
    if early:
        add("info", f"{len(early)} start-up advisory line(s) (profile stereo notes, ini settings): see WARNINGS AND ERRORS.")
    # ---- controllers
    b = log.info.get("bindings") or []
    last = b[-1] if b else None
    if last and (last.get("left") in (None, "none") or last.get("right") in (None, "none")):
        add("warn", "A hand ended with no interaction profile (left " + pretty_profile(last.get("left")) + ", right " + pretty_profile(last.get("right")) + ").")
    # ---- recorder
    if rec is not None:
        z = R_.play.get("rec_zero_hp", [])
        if z:
            add("info", f"The recorder saw the character's HP reach 0 ({len(z)}x): " + ", ".join(T(t + rec.offset) for t in z[:5])
                        + " (down or dead, the recorder does not say which).")
    # ---- SteamVR
    sv = R_.svr
    if sv.get("totals"):
        pres, drop, repro, when = sv["totals"]
        add("info", f"SteamVR compositor totals for its run (written {when:%H:%M:%S}): {pres} presents, {drop} dropped, {repro} reprojected "
                    f"({100.0 * drop / max(pres, 1):.1f}% dropped).")
    if sv.get("link"):
        worst = max(sv["link"], key=lambda x: x[1])
        add("info", f"Steam Link logged {len(sv['link'])} throttle events (worst {worst[1]:.0f} ms at {T(worst[0])}), {sv['resets']} stream resets and "
                    f"{sv['encoder_resets']} encoder resets.")
    if not R_.shutdown:
        add("info", "The log ends without shutdown lines: it is a copy of a session still running, or the emulator died.")
    # ---- things that are fine, so a clean run does not read as an empty report
    if R_.vr_started:
        if not errs:
            add("ok", "No error lines in the emulog.")
        if F.get("windows") and not F.get("timedout_total") and not F.get("episodes") and not F.get("gs_low"):
            add("ok", "No xrEndFrame plateau, no parking and no GS slowdown.")
        if last and last.get("left") not in (None, "none") and last.get("right") not in (None, "none")                 and not any("controller" in p["text"].lower() for p in log.problems.values()):
            add("ok", f"Both hands got an interaction profile ({pretty_profile(last.get('left'))}) and no controller warning was logged.")
    order = {"bad": 0, "warn": 1, "info": 2, "ok": 3}
    out.sort(key=lambda x: order[x[0]])
    return out


# --------------------------------------------------------------------------------------------------------------------
# tables shared by the text and HTML reports
# --------------------------------------------------------------------------------------------------------------------

def tbl_overall(R_: R):
    F = R_.F
    if not F.get("windows"):
        return None
    hdr = ["", "overall", "outside the episodes", "target"]
    rows = [
        ["submitted/s", f"{F['sub_mean']:.1f}", f"{F.get('sub_norm', 0):.1f}", f"{F['hz']:.1f} (display)"],
        ["new GS frames/s", f"{F['gs_mean']:.1f}", f"{F.get('gs_norm', 0):.1f}", f"{F['gs_target']:.2f}"],
    ]
    if F.get("have_end"):
        rows.append(["xrEndFrame mean ms", f"{F['end_mean']:.2f}", f"{(F.get('end_norm') or 0):.2f}", f"< {TH['end_ms']:g}"])
        rows.append(["xrEndFrame worst window max ms", f"{F['end_max']:.1f}", "", f"< {TH['spike_ms']:g}"])
        if F.get("lock_mean") is not None:
            rows.append(["queue-lock wait ms/s (all waiters)", f"{F['lock_mean']:.0f}", f"{(F.get('lock_norm') or 0):.0f}", f"< {TH['lock_ms']:g}"])
        if F.get("lock_max_ms") is not None:
            rows.append(["queue-lock longest single wait ms", f"{F['lock_max_ms']:.1f}", "", ""])
    rows.append(["re-sent/s", f"{F['resent_mean']:.1f}", "", ""])
    return hdr, rows


def tbl_episodes(R_: R, clock: Clock):
    F = R_.F
    if not F.get("episodes"):
        return None
    hdr = ["what", "from (s)", "to (s)", "clock", "length", "xrEndFrame mean", "submitted/s", "GS/s", "queue-lock ms/s"]
    rows = []
    for ep in F["episodes"]:
        rows.append([
            "app parked (10 Hz)" if ep["kind"] == "parked" else "xrEndFrame plateau",
            f"{ep['t0']:.1f}", f"{ep['t1']:.1f}" + ("+" if ep["open_end"] else ""),
            (clock.hms(ep["t0"]) + "-" + clock.hms(ep["t1"])) if clock.ok else "",
            fmt_dur(ep["dur"]), f"{ep['end_mean']:.1f} ms (max {ep['end_max']:.0f})", f"{ep['sub']:.1f}",
            f"{ep['gs']:.1f} ({ep['gs_min']:.0f}-{ep['gs_max']:.0f})", f"{ep['lock']:.0f}"])
    return hdr, rows


def tbl_spikes(R_: R, clock: Clock, limit=8):
    sp = R_.F.get("spikes", [])
    if not sp:
        return None
    hdr = ["window ending (s)", "clock", "xrEndFrame max", "mean", "submitted/s", "GS/s", "queue-lock ms/s"]
    rows = []
    for x in sorted(sp, key=lambda x: -x["mx"])[:limit]:
        rows.append([f"{x['t']:.1f}", clock.hms(x["t"]), f"{x['mx']:.1f} ms", f"{x['mean']:.1f}", f"{x['sub']:.1f}", f"{x['gs']:.1f}",
                     f"{x['lock']:.0f}" if x["lock"] is not None else ""])
    return hdr, rows


def tbl_minutes(R_: R, clock: Clock, verbose: bool):
    mins = R_.F.get("minutes", [])
    if not mins:
        return None
    hdr = ["t (m:ss)", "clock", "submitted/s", "GS/s", "xrEndFrame mean/max ms", "queue-lock ms/s", "flags"]
    rows = []
    i = 0
    while i < len(mins):
        m = mins[i]
        if not verbose and not m["flags"]:
            j = i
            while j + 1 < len(mins) and not mins[j + 1]["flags"]:
                j += 1
            seg = mins[i: j + 1]
            w = [x["t1"] - x["t0"] for x in seg]
            he = [x for x in seg if x["end_mean"] is not None]
            rows.append([
                f"{mmss(seg[0]['t0'])}-{mmss(seg[-1]['t1'])}", (clock.hms(seg[0]["t0"]) if clock.ok else ""),
                f"{wmean([x['sub'] for x in seg], w):.1f}", f"{wmean([x['gs'] for x in seg], w):.1f}",
                (f"{wmean([x['end_mean'] for x in he], [x['t1'] - x['t0'] for x in he]):.1f} / {max(x['end_max'] for x in he):.1f}" if he else "n/a"),
                (f"{wmean([x['lock'] for x in seg if x['lock'] is not None], [1] * len([x for x in seg if x['lock'] is not None])):.0f}"
                 if any(x["lock"] is not None for x in seg) else ""),
                "normal" + (f" ({len(seg)} min)" if len(seg) > 1 else "")])
            i = j + 1
            continue
        rows.append([
            f"{mmss(m['t0'])}-{mmss(m['t1'])}", clock.hms(m["t0"]) if clock.ok else "", f"{m['sub']:.1f}", f"{m['gs']:.1f}",
            (f"{m['end_mean']:.1f} / {m['end_max']:.1f}" if m["end_mean"] is not None else "n/a"),
            f"{m['lock']:.0f}" if m["lock"] is not None else "", ", ".join(m["flags"]) if m["flags"] else "normal"])
        i += 1
    return hdr, rows


def timeline_events(R_: R, verbose: bool):
    """Merged app + SteamVR events about focus, as (t, source, text)."""
    log, F = R_.log, R_.F
    ev = []
    for e in log.of("state"):
        ev.append((e.t, "app", f"session state {e.d['frm']} -> {e.d['to']}"))
    for e in log.of("started_unfocused"):
        ev.append((e.t, "app", "started unfocused: the SteamVR menu was open"))
    for e in log.of("toast"):
        if "paused" in e.d["text"].lower() or "back" in e.d["text"].lower():
            if verbose:
                ev.append((e.t, "app", f'toast "{e.d["text"]}"'))
    if verbose:
        for e in log.of("recenter"):
            ev.append((e.t, "app", "reference space recentered by the runtime"))
    for e in R_.svr.get("events", []):
        if e.kind in ("app_pause", "app_resume") and not verbose:
            continue
        ev.append((e.t, e.src, e.text))
    for ep in F.get("episodes", []):
        ev.append((ep["t0"], "analysis", ("app parked starts: " if ep["kind"] == "parked" else "xrEndFrame plateau starts: ")
                   + f"{ep['end_mean']:.1f} ms mean, {ep['sub']:.1f}/s submitted"))
        if not ep["open_end"]:
            ev.append((ep["t1"], "analysis", ("app parked ends" if ep["kind"] == "parked" else "xrEndFrame plateau ends (estimated)")))
    ev.sort(key=lambda x: x[0])
    return ev


def tbl_timeline(R_: R, clock: Clock, verbose: bool):
    ev = timeline_events(R_, verbose)
    if not ev:
        return None
    hdr = ["t (s)", "clock", "source", "event"]
    return hdr, [[f"{t:.1f}", clock.hms(t) if clock.ok else "", src, text] for t, src, text in ev]


def tbl_arm(R_: R, clock: Clock):
    A = R_.arm
    if not A["periods"]:
        return None
    hdr = ["#", "armed at (s)", "clock", "armed for", "taken away because", "recorder saw", "off for"]
    rows = []
    for i, p in enumerate(A["periods"], 1):
        rows.append([str(i), f"{p['start']:.1f}", clock.hms(p["start"]) if clock.ok else "", fmt_dur(p["dur"]),
                     ("(still armed)" if p["open"] else (p["why"] if p["detail"] else p["reason"] or "?")),
                     p["csv_end"] or "", fmt_dur(p["gap"]) if p["gap"] is not None else ""])
    return hdr, rows


def tbl_menus(R_: R, clock: Clock):
    M = R_.menus
    if not M["periods"]:
        return None
    hdr = ["#", "opened (s)", "clock", "open for", "what", "detail"]
    rows = []
    for i, p in enumerate(M["periods"], 1):
        d = ""
        if p["kind"] == "item screen":
            d = f"{p.get('confirms', 0)} confirm(s), {p.get('steers', 0)} steer(s), {p.get('submenus', 0)} submenu(s)" + \
                ("" if p.get("pointer", True) else ", point-to-select off")
        elif p["kind"] == "take-it prompt / game screen":
            d = "the recorder's menu byte stayed 0 (a pickup or hand-over prompt, not the item screen, map or pause)"
        rows.append([str(i), f"{p['t0']:.1f}", clock.hms(p["t0"]) if clock.ok else "", fmt_dur(p["dur"]), p["kind"], d])
    return hdr, rows


def tbl_play(R_: R, clock: Clock):
    P = R_.play
    rows = []
    if P["shots"] or P["dry"]:
        w = ", ".join(f"{k} {n}" for k, n in sorted(P["shots_by_weapon"].items(), key=lambda kv: -kv[1]))
        rows.append(["shots", str(P["shots"]), (w + "; " if w else "") + f"{P['dry']} dry fire(s)"])
    if P["hurts"]:
        ev = P["hurt_events"]
        first = ev[0].t if ev else 0
        rows.append(["hurts", str(P["hurts"]), f"{P['damage_logged']} HP lost in the logged ones; first at {clock.at(first)}"])
    if P["low_hp_runs"]:
        rs = P["low_hp_runs"]
        rows.append(["low HP (heartbeat)", str(len(rs)),
                     "; ".join(f"{clock.at(r['t0'])} to {r['t1']:.1f} s, lowest HP {r['min']} of {r['mx']}" for r in rs[:4])])
    if P["grabs"]:
        rows.append(["grabs", str(len(P["grabs"])), "; ".join(
            f"{clock.at(g['t'])} for {g['secs']:.1f} s" if g["secs"] is not None else f"{clock.at(g['t'])} (no release logged)"
            for g in P["grabs"][:6])])
    if P["reload_n"]:
        rows.append(["reloads / ammo gained", str(P["reload_n"]), "; ".join(
            f"{clock.at(e.t)} ammo {e.d['a']} -> {e.d['b']}" for e in P["reloads"][:4]) + " (the game counts any rise in the equipped gun's ammo)"])
    if P["weapons"]:
        rows.append(["weapon in hand", str(len(P["weapons"])), "; ".join(f"{n} ({k})" for _t, n, k in P["weapons"][:6])])
    if P["reload_other"]:
        rows.append(["reload (other log lines)", str(len(P["reload_other"])), clip(P["reload_other"][0].msg, 90)])
    prompts = [x for x in R_.menus["periods"] if x["kind"] == "take-it prompt / game screen"]
    pk = len(P["pickup_lines"]) + len(P.get("rec_pickup_runs", []))
    if pk or P.get("rec_pickup_runs") is not None or prompts:
        bits = []
        if prompts:
            bits.append(f"{len(prompts)} flat-screen take-it prompt(s)")
        if P["pickup_lines"]:
            bits.append(f"{len(P['pickup_lines'])} log line(s), e.g. '{clip(P['pickup_lines'][0].msg, 60)}'")
        if P.get("rec_pickup_runs") is not None:
            bits.append(f"{len(P['rec_pickup_runs'])} reach animation(s) in the recorder (action 9)")
        if not P["pickup_lines"]:
            bits.append("this build logs no pickup line")
        rows.append(["pickups / take-it reaches", str(len(prompts) or pk), "; ".join(bits)])
    if P["rumble_on"]:
        rows.append(["game rumble", str(P["rumble_on"]), "forwarded to the controllers"])
    if P["pouch"]:
        rows.append(["belt pouch squeezes", str(P["pouch"]), f"{P['pouch_nothing']} did nothing"])
    if P["recenters"]:
        rows.append(["recenters", str(P["recenters"]), "by the runtime"])
    return ["what", "count", "detail"], rows


def tbl_toasts(R_: R, clock: Clock, verbose: bool):
    P = R_.play
    if not P["toasts"]:
        return None
    rows = []
    if verbose:
        for t, txt in P["toast_list"]:
            rows.append([f"{t:.1f}", clock.hms(t) if clock.ok else "", txt, ""])
        return ["t (s)", "clock", "toast", ""], rows
    for txt, s in sorted(P["toasts"].items(), key=lambda kv: kv[1]["first"]):
        rows.append([txt, str(s["n"]), f"{s['first']:.1f}" + (f"-{s['last']:.1f}" if s["n"] > 1 else "")])
    return ["toast", "times", "at (s)"], rows


def tbl_warnings(R_: R, clock: Clock):
    pr = sorted(R_.log.problems.values(), key=lambda p: (0 if p["sev"] == "error" else 1, p["t"]))
    if not pr:
        return None
    return (["", "first (s)", "times", "line"],
            [[p["sev"], f"{p['t']:.1f}", str(p["count"]), clip(p["text"], 170)] for p in pr])


def tbl_gs(R_: R, clock: Clock):
    F = R_.F
    if not F.get("windows"):
        return None
    rows = [
        ["GS frames/s, whole log", f"{F['gs_mean']:.1f} = {F['gs_pct']:.1f}% of {F['gs_target']:.2f}"],
        ["GS frames/s, outside the episodes", f"{F.get('gs_norm', 0):.1f} = {100.0 * F.get('gs_norm', 0) / F['gs_target']:.1f}%"],
        ["windows under %d%% of target" % round(TH["gs_frac"] * 100),
         f"{len(F['gs_low'])} of {len(F['windows'])} ({fmt_dur(F['gs_low_dur'])}); {F['gs_low_inside']} inside the episodes"],
        ["slowest window", (f"{F['gs_worst'].gs:.1f}/s, 5 s ending {clock.at(F['gs_worst'].t)}" if F.get("gs_worst") else "")],
        ["windows with submitted/s under %d%% of display" % round(TH["sub_frac"] * 100),
         f"{len(F['sub_low'])} ({F['sub_low_inside']} inside the episodes)"],
        ["windows with queue-lock wait over %g ms/s" % TH["lock_ms"], f"{len(F['lock_hi'])} ({F['lock_hi_inside']} inside the episodes)"],
        ["stale frames (sum)", str(F["stale_total"])],
        ["GS image copies skipped (sum)", f"{F['skipped_total']} (the pacer was busy; the headset showed an older image)"],
        ["GS image copies timed out (sum)", str(F["timedout_total"])],
        ["publishes deferred (sum)", str(F["deferred_total"])],
        ["frames with shouldRender=false (sum)", str(F["norender_total"])],
    ]
    return ["", ""], rows


# --------------------------------------------------------------------------------------------------------------------
# text renderer
# --------------------------------------------------------------------------------------------------------------------

def text_table(hdr, rows, indent="  ", max_width=132) -> list:
    rows = [[ascii_text(c) for c in r] for r in rows]
    hdr = [ascii_text(h) for h in hdr]
    ncol = len(hdr)
    w = [len(h) for h in hdr]
    for r in rows:
        for i, c in enumerate(r[:ncol]):
            w[i] = max(w[i], len(c))
    # too wide: cut the widest column (longest free text first) until the table fits
    while len(indent) + sum(w) + 2 * (ncol - 1) > max_width:
        i = max(range(ncol), key=lambda k: w[k])
        if w[i] <= 14:
            break
        w[i] -= 1
    out = []
    if any(hdr):
        out.append(indent + "  ".join(h[: w[i]].ljust(w[i]) for i, h in enumerate(hdr)).rstrip())
        out.append(indent + "  ".join("-" * w[i] for i in range(ncol)).rstrip())
    for r in rows:
        cells = []
        for i in range(ncol):
            c = r[i] if i < len(r) else ""
            if len(c) > w[i]:
                c = c[: w[i] - 3] + "..."
            cells.append(c.ljust(w[i]))
        out.append(indent + "  ".join(cells).rstrip())
    return out


def wrap(text: str, first: str, rest: str | None = None, width=118) -> list[str]:
    import textwrap
    rest = " " * len(first) if rest is None else rest
    return textwrap.wrap(ascii_text(text), width=width, initial_indent=first, subsequent_indent=rest)


def render_text(R_: R, verbose=False) -> str:
    log, clock, F, rec, svr = R_.log, R_.clock, R_.F, R_.rec, R_.svr
    T = clock.at
    L: list[str] = []

    def h(title):
        L.append("")
        L.append(title)
        L.append("-" * len(title))

    L.append("PenguinScreen2 VR session report")
    L.append("=" * 32)
    cover = f"covers {log.t_first:.1f}-{log.t_last:.1f} s ({fmt_dur(log.t_last)})"
    L.append(f"Log        {log.path}  ({log.n_lines} lines, {cover}{'' if R_.shutdown else '; no shutdown lines'})")
    g = []
    if log.info.get("disc"):
        g.append(log.info["disc"])
    if log.info.get("serial"):
        g.append(log.info["serial"])
    if log.info.get("version"):
        g.append("PenguinScreen2 " + log.info["version"])
    if log.info.get("video_mode"):
        g.append(log.info["video_mode"])
    if g:
        L.append("Game       " + ", ".join(g))
    if clock.ok:
        L.append(f"Clock      t = 0 is {clock.t0:%Y-%m-%d %H:%M:%S.%f}"[:-4] + f" ({clock.how}; spread {clock.spread:.2f} s)")
    elif R_.vr_started:
        L.append("Clock      no wall clock: SteamVR's logs were not found or did not match; pass --t0 to set it")
    if rec is not None:
        L.append(f"Recorder   {rec.path}: {len(rec.rows)} rows over {fmt_dur(rec.rows[-1]['t'] - rec.rows[0]['t'])}; "
                 f"recorder time + {rec.offset:.2f} s = emulog time ({rec.how})")
        if R_.play.get("rec_beyond", 0) > 5:
            L.append(f"           the recording runs {fmt_dur(R_.play['rec_beyond'])} past the end of this log; only the part inside the log is used")
    elif getattr(R_, "rec_unused", None) is not None:
        L.append(f"Recorder   {R_.rec_unused.path}: not used ({R_.rec_unused.how})")
    elif R_.rec_note:
        L.append("Recorder   " + R_.rec_note)
    if R_.steam_dir and R_.vr_started:
        used = sorted({f.split(".")[0] for f in svr.get("files", [])})
        L.append(f"SteamVR    {R_.steam_dir}: " + (", ".join(used) if used else "no events (clock not fitted)")
                 + (f"; missing {', '.join(svr['missing'])}" if svr.get("missing") else ""))

    if R_.session_emulog:
        L.append(f"Note       the session folder's own emulog.txt is longer than this log ({os.path.getsize(R_.session_emulog) // 1024} KB vs "
                 f"{os.path.getsize(log.path) // 1024} KB): leave out the EMULOG argument to report on the whole recording")
    h("WHAT STANDS OUT")
    L.append("[!!] problem   [! ] worth a look   [i ] note   [ok] fine")
    marks = {"bad": "[!!]", "warn": "[! ]", "info": "[i ]", "ok": "[ok]"}
    for sev, text in R_.findings:
        L += wrap(text, f"{marks[sev]} ")
    if not R_.findings:
        L.append("(nothing to report)")

    # ---------------- session
    h("SESSION")
    info = log.info
    if R_.vr_started:
        vr_len = R_.t_vr_end - (R_.t_vr_start or 0)
        L.append(f"VR session   {T(R_.t_vr_start)} to {'shutdown' if R_.shutdown else 'the end of the log'} at {T(R_.t_vr_end)} = {fmt_dur(vr_len)} "
                 f"({vr_len:.0f} s); emulator runtime {fmt_dur(log.t_last)} in this log")
        if R_.focus["by_state"]:
            tot = sum(R_.focus["by_state"].values()) or 1.0
            L.append("Focus time   " + ", ".join(f"{k} {fmt_dur(v)} ({100 * v / tot:.0f}%)" for k, v in
                                                  sorted(R_.focus["by_state"].items(), key=lambda kv: -kv[1]) if v >= 0.5))
    else:
        L.append("VR session   none in this log")
    if info.get("runtime"):
        L.append(f"Runtime      {info['runtime']}" + (f"; HMD '{info['hmd']}'" if info.get("hmd") else "") +
                 (f"; Vulkan {info['vulkan']}" if info.get("vulkan") else ""))
    eyes = [e for e in log.ev if e.kind == "eyes"]
    if eyes:
        e = eyes[0]
        L.append("Eyes         " + "; ".join(eyes_text(e)))
        L.append(f"Display      {e.d.get('hz', 0):.1f} Hz" + (f"; recommended {info['eye_images'][0]}x{info['eye_images'][1]} per eye" if info.get("eye_images") else "")
                 + (f"; submission: {info['submission']}" if info.get("submission") else "")
                 + (f"; first-person camera FOV {info['fp_fov']:g} deg vertical" if info.get("fp_fov") else ""))
        if len(eyes) > 1:
            L.append(f"             (the Eyes line appears {len(eyes)} times)")
    b = info.get("bindings") or []
    if b:
        last = b[-1]
        L.append(f"Controllers  left {pretty_profile(last.get('left'))}, right {pretty_profile(last.get('right'))}"
                 + (f"; Steam Frame controller interaction {info['frame_ctrl']}" if info.get("frame_ctrl") else ""))
        seq = []
        for x in b:
            lab = f"L {pretty_profile(x.get('left'))} / R {pretty_profile(x.get('right'))}"
            if not seq or seq[-1][0] != lab:
                seq.append((lab, x["why"], x["t"]))
        steps = " -> ".join(f"{lab} ({why}, {t:.1f} s)" for lab, why, t in seq[:4])
        L += wrap("profile history: " + steps + (f"; {len(log.of('profile_changed'))} 'interaction profile changed' event(s)" if log.of("profile_changed") else ""), "             ", "               ")
    elif info.get("suggested"):
        L.append("Controllers  no 'Controller bindings' block in this log")
    if info.get("suggested"):
        L.append("             suggested bindings: " + ", ".join(f"{pretty_profile(p)} ({n})" for p, n in info["suggested"]))
    ctl = [e for e in log.ev if e.kind == "controller"]
    if ctl:
        L.append("             " + "; ".join(f"{e.d['side']} {e.d['what']} at {e.t:.1f} s" for e in ctl))
    if info.get("net_mode"):
        L.append(f"Network      {info['net_mode']}")
    if info.get("gpu") or info.get("cpu"):
        L.append(f"PC           {info.get('cpu', '')}; {info.get('gpu', '')}")

    # ---------------- frames
    h("FRAME STATS  (5 s windows from '(VR) Frames:' lines)")
    if not F.get("windows"):
        L.append("No '(VR) Frames:' lines in this log (older build, or the headset-rate pacer was off).")
    else:
        L.append(f"{len(F['windows'])} windows, {fmt_dur(F['covered'])} covered, {F['t_start']:.1f}-{F['t_end']:.1f} s. "
                 f"Rules: xrEndFrame mean over {TH['end_ms']:g} ms for over {TH['end_secs']:g} s is a plateau; a window max over {TH['spike_ms']:g} ms is a spike.")
        if not F["have_end"]:
            L.append("(this build's Frames line has no xrEndFrame or queue-lock fields: only submitted and GS rates are available)")
        t = tbl_overall(R_)
        L += text_table(*t)
        t = tbl_episodes(R_, clock)
        L.append("")
        if t:
            L.append("Episodes:")
            L += text_table(*t)
        else:
            L.append("Episodes: none (no plateau, no parking).")
        t = tbl_spikes(R_, clock)
        if t:
            L.append("")
            L.append(f"Spikes (outside the episodes; worst {min(8, len(R_.F['spikes']))} of {len(R_.F['spikes'])}):")
            L += text_table(*t)
        L.append("")
        L.append("Over time (per minute; runs of normal minutes are merged):")
        L += text_table(*tbl_minutes(R_, clock, verbose))
        strip = frame_strip(R_)
        if strip:
            L.append("")
            L += strip

    # ---------------- focus
    h("FOCUS AND STEAMVR EVENTS")
    t = tbl_timeline(R_, clock, verbose)
    if t:
        L += text_table(*t)
    else:
        L.append("No focus changes or SteamVR events.")
    if R_.steam_dir and not clock.ok:
        L.append("(SteamVR logs not lined up: no session-state lines matched. Pass --t0 'YYYY-MM-DD HH:MM:SS' to set the clock.)")
    parked = [e for e in F.get("episodes", []) if e["kind"] == "parked"]
    stalls_parked = sum(n for t, n in svr.get("stalls", []) if any(e["t0"] <= t <= e["t1"] + 1 for e in parked))
    if svr.get("link"):
        w = max(svr["link"], key=lambda x: x[1])
        L.append(f"Steam Link: {len(svr['link'])} throttle event(s), worst {w[1]:.0f} ms at {T(w[0])}; {svr['resets']} stream reset(s), "
                 f"{svr['encoder_resets']} encoder reset(s); {svr['stall_count']} compositor 'WaitForAcquire timed out' warning(s)"
                 + (f" ({stalls_parked} while the app was parked)" if stalls_parked else ""))
    if svr.get("totals"):
        pres, drop, repro, when = svr["totals"]
        L.append(f"SteamVR compositor totals (run ended {when:%H:%M:%S}): {pres} presents, {drop} dropped, {repro} reprojected")

    # ---------------- first person
    h("FIRST PERSON (ARMED / DISARMED)")
    A = R_.arm
    if not A["count"]:
        L.append("Never armed in this log.")
        dis = log.of("disarmed")
        if dis:
            L.append(f"The log's DISARMED lines: {len(dis)}, last: {dis[-1].d['why']}")
    else:
        L.append(f"Armed {A['count']} time(s), {fmt_dur(A['armed_total'])} in all; median {fmt_dur(A['median'])}; "
                 f"{'this build names the failing guard in the DISARMED line' if A['named_guard'] else 'this build only logs the reason class; the recorder column fills in what changed'}.")
        L += text_table(*tbl_arm(R_, clock))
        if A["reasons"]:
            L.append("Reasons logged: " + ", ".join(f"{n}x {k}" for k, n in A["reasons"].items()))
        if A["csv_causes"]:
            L.append("What the recorder saw at those disarms: " + ", ".join(f"{n}x {k}" for k, n in sorted(A["csv_causes"].items(), key=lambda kv: -kv[1])))

    # ---------------- menus / screens
    h("MENUS AND SCREEN SWITCHES")
    M = R_.menus
    if M["count"]:
        L.append(f"{M['count']} menu(s) opened: " + ", ".join(f"{v['n']} {k} ({fmt_dur(v['secs'])})" for k, v in M["by_kind"].items()) +
                 f". {M['confirms']} menu confirm(s) passed to the game." +
                 (f" Point-to-select was off for {M['pointer_off']} item screen(s)." if M["pointer_off"] else ""))
        if verbose or M["count"] <= 25:
            L += text_table(*tbl_menus(R_, clock))
        else:
            L.append("(use -v for the per-menu list)")
        if rec is not None and R_.play.get("rec_menu_runs") is not None:
            L.append(f"Recorder cross-check: the game's menu byte went up {len(R_.play['rec_menu_runs'])} time(s) "
                     f"({fmt_dur(sum(b - a for a, b in R_.play['rec_menu_runs']))} in all) against {M['count']} menu(s) in the log.")
        for p in M["mismatch"]:
            L.append(f"  cursor warning: {clip(p['text'], 150)} (x{p['count']}, first {T(p['t'])})")
    else:
        L.append("No menus opened (no 'Stereo scene: menu' lines and no world-screen periods while armed).")
    S = R_.screens
    L.append("")
    if S["modes"].get("world"):
        L.append(f"World screen (the flat menu screen) shown {S['modes']['world']} time(s): {S['world_menu']} for menus, "
                 f"{S['world_disarm']} because first person was taken away, {S['world_other']} other.")
    L.append(f"Comfort blinks {S['blinks']} ({S['per_min_blink']:.1f}/min): " + (", ".join(f"{n}x {k}" for k, n in S["blink_reasons"].items()) or "none") +
             f". Screen-mode switches {S['switches']} ({S['per_min_switch']:.1f}/min): " + (", ".join(f"{n}x to {k}" for k, n in S["modes"].items()) or "none") + ".")
    if S["spent"]:
        tot = sum(S["spent"].values()) or 1.0
        L.append("Time on each screen: " + ", ".join(f"{k} {fmt_dur(v)} ({100 * v / tot:.0f}%)" for k, v in sorted(S["spent"].items(), key=lambda kv: -kv[1])) +
                 f". Stereo scene changes {S['scene_changes']}, XR swapchains created {S['swapchains']}.")
    if S["worst_switch"][1] is not None:
        L.append(f"Busiest 60 s: {S['worst_switch'][0]} switches from {T(S['worst_switch'][1])}; {S['worst_blink'][0]} blinks from {T(S['worst_blink'][1])}"
                 f" (flicker flag at {TH['flicker_per_min']} per 60 s is only meaningful when no menu explains them).")
    L.append(f"Flicker check: {len(S['unexplained'])} switch(es) with no menu or arm change next to them"
             + (": " + ", ".join(f"{t:.1f} s ({m})" for t, m in S["unexplained"][:6]) if S["unexplained"] else "")
             + f"; {len(S['quick'])} quick reversal(s) within 1 s"
             + ((f", {S['quick_backed']} of them matched by the game's own menu byte in the recorder" if S["has_rec"] else "") if S["quick"] else "")
             + (": " + ", ".join(f"{b[0]:.1f} s {b[2]}->{b[3]} after {b[1]:.2f} s" for b in S["quick"][:6]) if S["quick"] else "") + ".")
    bins = S["bins_switch"]
    if len(bins) > 1:
        flagged = [(i, n) for i, n in enumerate(bins) if n >= TH["flicker_per_min"]]
        if flagged:
            L.append(f"Minutes (from the VR session start) at or over {TH['flicker_per_min']} switches: " +
                     ", ".join(f"{mmss(S['bin_t0'] + i * 60)} {n} switches/{S['bins_blink'][i]} blinks" for i, n in flagged[:8]))
        L.append("Switches per minute: " + " ".join(str(n) for n in bins))

    # ---------------- gameplay
    h("GAMEPLAY EVENTS")
    P = R_.play
    tp = tbl_play(R_, clock)
    if tp[1]:
        L += text_table(*tp)
    else:
        L.append("Nothing logged (no shots, hurts, grabs or pickups).")
    tt = tbl_toasts(R_, clock, verbose)
    if tt:
        L.append("")
        L.append("HUD toasts shown:")
        L += text_table(*tt)

    # ---------------- warnings
    h("WARNINGS AND ERRORS")
    tw = tbl_warnings(R_, clock)
    if tw:
        L += text_table(*tw)
    else:
        L.append("None.")

    # ---------------- GS
    h("GS SPEED AND DROPS")
    tg = tbl_gs(R_, clock)
    if tg:
        L += text_table(*tg)
    else:
        L.append("No Frames lines, so no GS speed.")

    # ---------------- recorder
    if rec is not None:
        h("RECORDER (state.csv)")
        L.append(f"{len(rec.rows)} samples, one every {P.get('rec_step', 0):.2f} s; {len(P.get('rec_gaps', []))} gap(s) over 2 s"
                 + (": " + ", ".join(f"{g[0] + rec.offset:.0f} s (+{g[1]:.1f} s)" for g in P["rec_gaps"][:5]) if P.get("rec_gaps") else "") + ".")
        st = P.get("rec_status_secs", {})
        tot = sum(st.values()) or 1.0
        L.append("Scenario status time: " + ", ".join(f"{k}{' (' + STATUS_NAMES[k] + ')' if k in STATUS_NAMES else ''} {fmt_dur(v)}"
                                                       for k, v in sorted(st.items(), key=lambda kv: -kv[1])[:6]))
        L.append(f"HP lowest while in game {P.get('rec_hp_min')} of {P.get('rec_hp_max')}; HP 0 at "
                 + (", ".join(T(t + rec.offset) for t in P["rec_zero_hp"]) if P.get("rec_zero_hp") else "never")
                 + (f"; back up at {', '.join(T(t + rec.offset) for t in P['rec_revived'])}" if P.get("rec_revived") else "") + ".")
        L.append(f"Menu byte up {len(P.get('rec_menu_runs', []))} time(s), {fmt_dur(sum(b - a for a, b in P.get('rec_menu_runs', [])))} in all; "
                 f"aiming {fmt_dur(P.get('rec_aim_secs', 0))}; grabbed {len(P.get('rec_grab_runs', []))} time(s); "
                 f"{len(P.get('rec_pickup_runs', []))} pickup reach(es).")
        if P.get("rec_idle"):
            L.append("Stood still (45 s or more, in game): " + ", ".join(
                f"{T(a + rec.offset)} for {fmt_dur(b - a)}" for a, b in P["rec_idle"][:6]))

    if not R_.vr_started:
        L = prune_sections(L, ("FRAME STATS", "FOCUS AND STEAMVR", "MENUS AND SCREEN", "GS SPEED"))
        if not A["count"]:
            L = prune_sections(L, ("FIRST PERSON",))
        if not tp[1]:
            L = prune_sections(L, ("GAMEPLAY",))
    L.append("")
    L.append("Thresholds: " + ", ".join(f"{k}={v}" for k, v in TH.items()))
    L = rewrap(L)
    return "\n".join(ascii_text(x) for x in L) + "\n"


def rewrap(L: list, width: int = 132) -> list:
    """Wrap long prose lines (not tables, which start with an indent) with a hanging indent under their label."""
    import textwrap
    out = []
    for ln in L:
        if len(ln) <= width or ln.startswith(" ") or "  -----" in ln:
            out.append(ln)
            continue
        km = re.match(r"([A-Za-z][A-Za-z ]*?)\s{2,}(?=\S)", ln)
        hang = km.end() if km else 4
        first, body = (ln[:hang], ln[hang:]) if km else ("", ln)
        out += textwrap.wrap(ascii_text(body), width=width, initial_indent=first, subsequent_indent=" " * hang,
                             break_long_words=False, break_on_hyphens=False) or [ln]
    return out


def prune_sections(L: list, titles) -> list:
    """Drop whole sections (heading, rule, body) whose heading starts with one of titles."""
    heads = [i for i in range(len(L) - 1) if L[i] and L[i + 1] and set(L[i + 1]) == {"-"}]
    out = []
    keep_from = 0
    for k, i in enumerate(heads):
        end = (heads[k + 1] - 1) if k + 1 < len(heads) else len(L)   # the blank line before the next heading belongs to it
        if any(L[i].startswith(t) for t in titles):
            out += L[keep_from: i - 1]
            keep_from = end
    out += L[keep_from:]
    return out


def frame_strip(R_: R) -> list[str]:
    """One character per 30 s: xrEndFrame mean (. under 3 ms, + 3-20, # over 20 ms) and GS speed (. >= 95%, - 80-95, v below)."""
    fr = R_.F.get("windows", [])
    if not fr:
        return []
    t0 = int(R_.F["t_start"] // 30) * 30
    n = int((R_.F["t_end"] - t0) // 30) + 1
    a, b = [], []
    for k in range(n):
        seg = [f for f in fr if t0 + 30 * k <= f.t - 0.01 < t0 + 30 * (k + 1)]
        if not seg:
            a.append(" ")
            b.append(" ")
            continue
        em = [f.end_mean for f in seg if f.end_mean is not None]
        if em:
            m = wmean(em, [f.sub for f in seg if f.end_mean is not None])
            a.append("." if m <= TH["end_ms"] else "+" if m < 20 else "#")
        else:
            a.append("?")
        g = wmean([f.gs for f in seg], [f.dur for f in seg]) / R_.F["gs_target"]
        b.append("." if g >= TH["gs_frac"] else "-" if g >= 0.8 else "v")
    pad = " " * 2
    return [f"One character per 30 s from {mmss(t0)}:", f"{pad}xrEndFrame  [{''.join(a)}]  . under {TH['end_ms']:g} ms  + slow  # parked",
            f"{pad}GS speed    [{''.join(b)}]  . ok  - under {TH['gs_frac']:.0%}  v under 80%"]


# --------------------------------------------------------------------------------------------------------------------
# HTML renderer (one self-contained page, inline SVG, no external libraries)
# --------------------------------------------------------------------------------------------------------------------

CSS = """
:root{--bg:#ffffff;--fg:#1c2127;--muted:#5b6672;--card:#f4f6f8;--line:#d5dbe1;--grid:#e6eaee;
--ok:#2f855a;--warn:#b7791f;--bad:#c53030;--info:#2b6cb0;
--c-sub:#2b6cb0;--c-gs:#2f855a;--c-end:#c05621;--c-endmax:#e8b890;--c-lock:#805ad5;--c-hp:#c53030;
--s-focused:#38a169;--s-visible:#d69e2e;--s-sync:#e53e3e;--s-other:#8795a3;
--m-item:#3182ce;--m-map:#805ad5;--m-other:#8795a3;--armed:#38a169;--band-plateau:rgba(214,158,46,.22);--band-parked:rgba(229,62,62,.18)}
@media (prefers-color-scheme: dark){:root:not([data-theme="light"]){--bg:#13171c;--fg:#e4e8ec;--muted:#98a4b0;--card:#1b2128;--line:#2e3742;--grid:#252d36;
--ok:#68d391;--warn:#ecc94b;--bad:#fc8181;--info:#63b3ed;
--c-sub:#63b3ed;--c-gs:#68d391;--c-end:#f6ad55;--c-endmax:#8a6a4a;--c-lock:#b794f4;--c-hp:#fc8181;
--s-focused:#48bb78;--s-visible:#ecc94b;--s-sync:#fc8181;--s-other:#718096;
--m-item:#63b3ed;--m-map:#b794f4;--m-other:#718096;--armed:#48bb78;--band-plateau:rgba(236,201,75,.18);--band-parked:rgba(252,129,129,.16)}}
:root[data-theme="dark"]{--bg:#13171c;--fg:#e4e8ec;--muted:#98a4b0;--card:#1b2128;--line:#2e3742;--grid:#252d36;
--ok:#68d391;--warn:#ecc94b;--bad:#fc8181;--info:#63b3ed;
--c-sub:#63b3ed;--c-gs:#68d391;--c-end:#f6ad55;--c-endmax:#8a6a4a;--c-lock:#b794f4;--c-hp:#fc8181;
--s-focused:#48bb78;--s-visible:#ecc94b;--s-sync:#fc8181;--s-other:#718096;
--m-item:#63b3ed;--m-map:#b794f4;--m-other:#718096;--armed:#48bb78;--band-plateau:rgba(236,201,75,.18);--band-parked:rgba(252,129,129,.16)}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.5 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
main{max-width:1060px;margin:0 auto;padding:20px 16px 48px}
h1{font-size:1.5rem;margin:0 0 4px}
h2{font-size:1.1rem;margin:30px 0 8px;padding-top:12px;border-top:1px solid var(--line)}
p,li{margin:.3em 0}
.sub{color:var(--muted);margin:0 0 14px}
dl.meta{display:grid;grid-template-columns:max-content 1fr;gap:2px 14px;margin:8px 0 0;font-size:.9rem}
dl.meta dt{color:var(--muted)}dl.meta dd{margin:0;overflow-wrap:anywhere}
ul.find{list-style:none;padding:0;margin:8px 0}
ul.find li{background:var(--card);border-left:4px solid var(--info);border-radius:4px;padding:8px 12px;margin:6px 0}
ul.find li.bad{border-color:var(--bad)}ul.find li.warn{border-color:var(--warn)}ul.find li.ok{border-color:var(--ok)}
ul.find b.tag{display:inline-block;min-width:2.6em;font-size:.75rem;letter-spacing:.04em;text-transform:uppercase;color:var(--muted)}
ul.find li.bad b.tag{color:var(--bad)}ul.find li.warn b.tag{color:var(--warn)}ul.find li.ok b.tag{color:var(--ok)}
.tablewrap{overflow-x:auto;margin:8px 0}
table{border-collapse:collapse;font-size:.85rem;min-width:100%}
th,td{padding:4px 10px;text-align:left;border-bottom:1px solid var(--line);vertical-align:top}
th{color:var(--muted);font-weight:600;white-space:nowrap}
td.n{font-variant-numeric:tabular-nums}
tr.flag td{background:var(--card)}
.legend{display:flex;flex-wrap:wrap;gap:4px 16px;font-size:.8rem;color:var(--muted);margin:6px 0 2px}
.legend i{display:inline-block;width:14px;height:8px;border-radius:2px;margin-right:5px;vertical-align:baseline}
.chartwrap{overflow-x:auto}
.chartwrap svg{min-width:720px}
svg{display:block;width:100%;height:auto;max-width:100%}
svg text{fill:var(--muted);font:11px system-ui,sans-serif}
svg .lane{fill:var(--card);fill-opacity:.55}
svg .grid{stroke:var(--grid);stroke-width:1}
svg .axis{stroke:var(--line);stroke-width:1}
.small{font-size:.85rem;color:var(--muted)}
pre.strip{background:var(--card);padding:8px 12px;border-radius:4px;overflow-x:auto;font-size:.8rem}
"""


def _esc(x) -> str:
    return htmllib.escape(str(x), quote=True)


def html_table(hdr, rows, flag_col=None, numeric=()) -> str:
    out = ['<div class="tablewrap"><table>']
    if any(hdr):
        out.append("<thead><tr>" + "".join(f"<th>{_esc(h)}</th>" for h in hdr) + "</tr></thead>")
    out.append("<tbody>")
    for r in rows:
        cls = ""
        if flag_col is not None and flag_col < len(r) and r[flag_col] and r[flag_col] != "normal":
            cls = ' class="flag"'
        out.append(f"<tr{cls}>" + "".join(
            f'<td{" class=n" if i in numeric else ""}>{_esc(c)}</td>' for i, c in enumerate(r)) + "</tr>")
    out.append("</tbody></table></div>")
    return "".join(out)


def _ticks(t_lo: float, t_hi: float):
    span = max(t_hi - t_lo, 1.0)
    for step in (10, 20, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200):
        if span / step <= 14:
            break
    t = math.ceil(t_lo / step) * step
    out = []
    while t <= t_hi:
        out.append(t)
        t += step
    return out


def svg_timeline(R_: R) -> tuple[str, str]:
    """The main chart: frame stats in three panels, then lanes for focus, first person, menus, SteamVR, screens and play."""
    log, F, clock, rec, P = R_.log, R_.F, R_.clock, R_.rec, R_.play
    fr = F.get("windows", [])
    t_hi = log.t_last
    t_lo = max(0.0, (R_.t_vr_start if R_.t_vr_start is not None else 0.0) - 3.0)
    if fr:
        t_lo = min(t_lo, fr[0].t0)
    W, LM, RM = 1000, 74, 14
    PW = W - LM - RM

    def X(t):
        return LM + (min(max(t, t_lo), t_hi) - t_lo) / max(t_hi - t_lo, 1e-9) * PW

    parts: list[str] = []
    y = 6
    panels = []   # (top, height) of the three frame panels, for the episode bands and grid
    lanes_top = None

    def label(text, ytxt, anchor="end", x=None):
        parts.append(f'<text x="{(LM - 6) if x is None else x}" y="{ytxt:.1f}" text-anchor="{anchor}">{_esc(text)}</text>')

    def hseries(top, h, vals, ymap, cls_color, width=1.4, fill=False, base=None):
        """A step series over the 5 s windows: vals is [(t0, t1, v)]."""
        pts = []
        for t0, t1, v in vals:
            if v is None:
                continue
            yy = ymap(v)
            pts.append((X(t0), yy))
            pts.append((X(t1), yy))
        if not pts:
            return
        d = "M" + " L".join(f"{px:.1f},{py:.1f}" for px, py in pts)
        if fill:
            bt = top + h if base is None else base
            parts.append(f'<path d="{d} L{pts[-1][0]:.1f},{bt:.1f} L{pts[0][0]:.1f},{bt:.1f}Z" fill="{cls_color}" opacity=".35"/>')
        parts.append(f'<path d="{d}" fill="none" stroke="{cls_color}" stroke-width="{width}" stroke-linejoin="round"/>')

    # ---- panel 1: xrEndFrame (log scale)
    h1 = 104
    top1 = y
    panels.append((top1, h1))
    parts.append(f'<rect class="lane" x="{LM}" y="{top1}" width="{PW}" height="{h1}" rx="3"/>')
    lo_ms, hi_ms = 0.3, 300.0

    def Y1(v):
        v = min(max(v, lo_ms), hi_ms)
        return top1 + h1 * (1 - (math.log10(v) - math.log10(lo_ms)) / (math.log10(hi_ms) - math.log10(lo_ms)))

    for g in (1, 3, 10, 30, 100):
        parts.append(f'<line class="grid" x1="{LM}" x2="{LM + PW}" y1="{Y1(g):.1f}" y2="{Y1(g):.1f}"/>')
        label(f"{g} ms", Y1(g) + 4)
    parts.append(f'<line x1="{LM}" x2="{LM + PW}" y1="{Y1(TH["end_ms"]):.1f}" y2="{Y1(TH["end_ms"]):.1f}" stroke="var(--bad)" stroke-dasharray="4 4" opacity=".7"/>')
    label("xrEndFrame", top1 + 10, "start", 2)
    y = top1 + h1 + 8
    # ---- panel 2: frames per second
    h2 = 96
    top2 = y
    panels.append((top2, h2))
    parts.append(f'<rect class="lane" x="{LM}" y="{top2}" width="{PW}" height="{h2}" rx="3"/>')
    mx2 = 100.0

    def Y2(v):
        return top2 + h2 * (1 - min(max(v, 0.0), mx2) / mx2)

    for g in (0, 30, 60, 90):
        parts.append(f'<line class="grid" x1="{LM}" x2="{LM + PW}" y1="{Y2(g):.1f}" y2="{Y2(g):.1f}"/>')
        label(f"{g}/s", Y2(g) + 4)
    if F.get("hz"):
        parts.append(f'<line x1="{LM}" x2="{LM + PW}" y1="{Y2(F["hz"]):.1f}" y2="{Y2(F["hz"]):.1f}" stroke="var(--c-sub)" stroke-dasharray="2 4" opacity=".6"/>')
    if F.get("gs_target"):
        parts.append(f'<line x1="{LM}" x2="{LM + PW}" y1="{Y2(F["gs_target"]):.1f}" y2="{Y2(F["gs_target"]):.1f}" stroke="var(--c-gs)" stroke-dasharray="2 4" opacity=".6"/>')
    label("frames/s", top2 + 10, "start", 2)
    y = top2 + h2 + 8
    # ---- panel 3: queue-lock wait
    h3 = 56
    top3 = y
    panels.append((top3, h3))
    parts.append(f'<rect class="lane" x="{LM}" y="{top3}" width="{PW}" height="{h3}" rx="3"/>')
    locks = [f.lock_ms_s for f in fr if f.lock_ms_s is not None]
    mx3 = max([200.0] + locks)

    def Y3(v):
        return top3 + h3 * (1 - min(max(v, 0.0), mx3) / mx3)

    for g in (0, round(mx3 / 2, -2) if mx3 > 300 else 100, round(mx3, -1)):
        if g <= mx3:
            parts.append(f'<line class="grid" x1="{LM}" x2="{LM + PW}" y1="{Y3(g):.1f}" y2="{Y3(g):.1f}"/>')
            label(f"{g:g}", Y3(g) + 4)
    label("lock ms/s", top3 + 10, "start", 2)
    y = top3 + h3 + 10
    lanes_top = y

    # ---- episode bands over the three panels (and the lanes below, drawn after)
    bands = []
    for ep in F.get("episodes", []):
        bands.append((ep["t0"], ep["t1"], "var(--band-parked)" if ep["kind"] == "parked" else "var(--band-plateau)",
                      ("App parked" if ep["kind"] == "parked" else "xrEndFrame plateau") + f" {fmt_dur(ep['dur'])}: {ep['end_mean']:.1f} ms, GS {ep['gs']:.1f}/s"))
    for (t0, t1, col, tip) in bands:
        for (tp, hp) in panels:
            parts.append(f'<rect x="{X(t0):.1f}" y="{tp}" width="{max(X(t1) - X(t0), 1.5):.1f}" height="{hp}" fill="{col}"><title>{_esc(tip)}</title></rect>')
    # series on top of the bands
    if fr:
        hseries(top1, h1, [(f.t0, f.t, f.end_max) for f in fr if f.end_max is not None], Y1, "var(--c-endmax)", 1.0)
        hseries(top1, h1, [(f.t0, f.t, f.end_mean) for f in fr if f.end_mean is not None], Y1, "var(--c-end)", 1.6)
        hseries(top2, h2, [(f.t0, f.t, f.sub) for f in fr], Y2, "var(--c-sub)", 1.4)
        hseries(top2, h2, [(f.t0, f.t, f.gs) for f in fr], Y2, "var(--c-gs)", 1.4)
        hseries(top3, h3, [(f.t0, f.t, f.lock_ms_s) for f in fr if f.lock_ms_s is not None], Y3, "var(--c-lock)", 1.2, fill=True)
    else:
        parts.append(f'<text x="{LM + 10}" y="{top1 + 20}">no "(VR) Frames:" lines in this log</text>')

    # ---- lanes
    lane_h = 16
    cur = [lanes_top]

    def lane(name):
        top = cur[0]
        parts.append(f'<rect class="lane" x="{LM}" y="{top}" width="{PW}" height="{lane_h}" rx="2"/>')
        label(name, top + 12)
        cur[0] += lane_h + 5
        return top

    # focus
    top = lane("focus")
    colmap = {"FOCUSED": "var(--s-focused)", "VISIBLE": "var(--s-visible)", "SYNCHRONIZED": "var(--s-sync)"}
    for p in R_.focus.get("periods", []):
        x0, x1 = X(p["t0"]), X(p["t1"])
        parts.append(f'<rect x="{x0:.1f}" y="{top + 1}" width="{max(x1 - x0, 1.5):.1f}" height="{lane_h - 2}" fill="{colmap.get(p["state"], "var(--s-other)")}" opacity=".85"><title>{_esc(p["state"])} {p["t0"]:.1f}-{p["t1"]:.1f} s ({fmt_dur(p["t1"] - p["t0"])})</title></rect>')
        if x1 - x0 > 70:
            parts.append(f'<text x="{(x0 + x1) / 2:.1f}" y="{top + 12}" text-anchor="middle" style="fill:#fff;font-weight:600">{_esc(p["state"].lower())}</text>')
    # first person
    top = lane("first person")
    for p in R_.arm["periods"]:
        x0, x1 = X(p["start"]), X(p["end"])
        why = "still armed" if p["open"] else (p["why"] if p["detail"] else p["reason"])
        tip = f"armed {p['start']:.1f}-{p['end']:.1f} s ({fmt_dur(p['dur'])}); ended: {why}" + (f"; recorder: {p['csv_end']}" if p.get("csv_end") else "")
        parts.append(f'<rect x="{x0:.1f}" y="{top + 2}" width="{max(x1 - x0, 1.5):.1f}" height="{lane_h - 4}" fill="var(--armed)" opacity=".8"><title>{_esc(tip)}</title></rect>')
        if not p["open"]:
            parts.append(f'<line x1="{x1:.1f}" x2="{x1:.1f}" y1="{top}" y2="{top + lane_h}" stroke="var(--bad)" stroke-width="1.5"/>')
    # menus
    top = lane("menus")
    for p in R_.menus["periods"]:
        x0, x1 = X(p["t0"]), X(p["t1"])
        col = "var(--m-item)" if p["kind"] == "item screen" else "var(--m-map)" if p["kind"] == "map / pause" else "var(--m-other)"
        parts.append(f'<rect x="{x0:.1f}" y="{top + 2}" width="{max(x1 - x0, 2.0):.1f}" height="{lane_h - 4}" fill="{col}"><title>{_esc(p["kind"])} {p["t0"]:.1f} s for {fmt_dur(p["dur"])}</title></rect>')
    # screen switches
    top = lane("screen")
    for e in log.of("screen"):
        col = "var(--armed)" if e.d["mode"] == "first person" else "var(--m-item)"
        parts.append(f'<line x1="{X(e.t):.1f}" x2="{X(e.t):.1f}" y1="{top + 2}" y2="{top + lane_h - 2}" stroke="{col}" stroke-width="1.2"><title>{e.t:.1f} s: screen -> {_esc(e.d["mode"])}</title></line>')
    # SteamVR
    top = lane("SteamVR")
    shown = None
    for e in R_.svr.get("events", []):
        if e.t < t_lo or e.t > t_hi:
            continue
        x = X(e.t)
        if e.kind == "dash_shown":
            shown = e.t
        elif e.kind == "dash_hidden" and shown is not None:
            parts.append(f'<rect x="{X(shown):.1f}" y="{top + 5}" width="{max(X(e.t) - X(shown), 2):.1f}" height="{lane_h - 10}" fill="var(--warn)" opacity=".7"><title>dashboard open {shown:.1f}-{e.t:.1f} s</title></rect>')
            shown = None
        elif e.kind == "dash_request":
            parts.append(f'<path d="M{x:.1f},{top + 2} l4,{lane_h - 4} l-8,0 z" fill="var(--warn)"><title>{e.t:.1f} s: {_esc(e.text)}</title></path>')
        elif e.kind in ("standby", "link_inactive"):
            parts.append(f'<path d="M{x:.1f},{top + 2} l5,{(lane_h - 4) / 2:.1f} l-5,{(lane_h - 4) / 2:.1f} l-5,-{(lane_h - 4) / 2:.1f} z" fill="var(--bad)"><title>{e.t:.1f} s: {_esc(e.text)}</title></path>')
        elif e.kind == "stream_reset":
            parts.append(f'<line x1="{x:.1f}" x2="{x:.1f}" y1="{top + 4}" y2="{top + lane_h - 4}" stroke="var(--muted)"><title>{e.t:.1f} s: {_esc(e.text)}</title></line>')
    if shown is not None:
        parts.append(f'<rect x="{X(shown):.1f}" y="{top + 5}" width="{max(X(t_hi) - X(shown), 2):.1f}" height="{lane_h - 10}" fill="var(--warn)" opacity=".7"><title>dashboard open from {shown:.1f} s</title></rect>')
    for t, ms in R_.svr.get("link", []):
        if t_lo <= t <= t_hi and ms >= 20:
            parts.append(f'<circle cx="{X(t):.1f}" cy="{top + lane_h / 2:.1f}" r="2.2" fill="var(--c-lock)"><title>{t:.1f} s: Steam Link throttle {ms:.0f} ms</title></circle>')
    # play
    top = lane("play")
    for e in log.of("shot"):
        parts.append(f'<line x1="{X(e.t):.1f}" x2="{X(e.t):.1f}" y1="{top + 5}" y2="{top + lane_h - 3}" stroke="var(--fg)" opacity=".55"><title>{e.t:.1f} s: shot ({_esc(e.d["weapon"])}) ammo {e.d["a"]} -> {e.d["b"]}</title></line>')
    for e in log.of("hurt"):
        parts.append(f'<line x1="{X(e.t):.1f}" x2="{X(e.t):.1f}" y1="{top + 1}" y2="{top + lane_h - 1}" stroke="var(--bad)" stroke-width="1.6"><title>{e.t:.1f} s: hurt HP {e.d["a"]} -> {e.d["b"]}</title></line>')
    for g in P["grabs"]:
        if g["secs"]:
            parts.append(f'<rect x="{X(g["t"]):.1f}" y="{top + 1}" width="{max(X(g["t"] + g["secs"]) - X(g["t"]), 2):.1f}" height="4" fill="var(--warn)"><title>{g["t"]:.1f} s: grabbed for {g["secs"]:.1f} s</title></rect>')
    for e in log.of("reload"):
        parts.append(f'<circle cx="{X(e.t):.1f}" cy="{top + lane_h - 4}" r="2.4" fill="var(--c-sub)"><title>{e.t:.1f} s: ammo {e.d["a"]} -> {e.d["b"]}</title></circle>')
    # recorder HP
    if rec is not None and rec.rows:
        top = cur[0]
        hh = 44
        parts.append(f'<rect class="lane" x="{LM}" y="{top}" width="{PW}" height="{hh}" rx="2"/>')
        label("HP %", top + 12)
        pts = []
        for r_ in rec.rows:
            tt = r_["t"] + rec.offset
            if tt < t_lo or tt > t_hi or r_["hp_max_f"] <= 0:
                continue
            pts.append((X(tt), top + hh - 3 - (hh - 6) * min(r_["hp_f"] / r_["hp_max_f"], 1.0)))
        if pts:
            d = "M" + " L".join(f"{a:.1f},{b:.1f}" for a, b in pts)
            parts.append(f'<path d="{d}" fill="none" stroke="var(--c-hp)" stroke-width="1.3"/>')
        for tz in P.get("rec_zero_hp", []):
            parts.append(f'<circle cx="{X(tz + rec.offset):.1f}" cy="{top + hh - 3}" r="3" fill="var(--bad)"><title>HP reached 0 at {tz + rec.offset:.1f} s</title></circle>')
        cur[0] += hh + 5
    # ---- time axis
    bottom = cur[0]
    for t in _ticks(t_lo, t_hi):
        x = X(t)
        parts.insert(0, f'<line class="grid" x1="{x:.1f}" x2="{x:.1f}" y1="0" y2="{bottom}"/>')
        txt = mmss(t)
        parts.append(f'<text x="{x:.1f}" y="{bottom + 12}" text-anchor="middle">{txt}</text>')
        if clock.ok:
            parts.append(f'<text x="{x:.1f}" y="{bottom + 24}" text-anchor="middle" style="opacity:.75">{clock.hms(t)[:5]}</text>')
    height = bottom + (30 if clock.ok else 18)
    svg = (f'<svg viewBox="0 0 {W} {height}" role="img" aria-label="Frame statistics and events over time">'
           + "".join(parts) + "</svg>")
    legend = ('<div class="legend">'
              '<span><i style="background:var(--c-end)"></i>xrEndFrame mean (log scale)</span>'
              '<span><i style="background:var(--c-endmax)"></i>window max</span>'
              '<span><i style="background:var(--c-sub)"></i>submitted/s</span>'
              '<span><i style="background:var(--c-gs)"></i>new GS frames/s</span>'
              '<span><i style="background:var(--c-lock)"></i>queue-lock wait</span>'
              '<span><i style="background:var(--band-plateau);border:1px solid var(--warn)"></i>plateau</span>'
              '<span><i style="background:var(--band-parked);border:1px solid var(--bad)"></i>app parked</span>'
              '<span><i style="background:var(--s-focused)"></i>focused</span>'
              '<span><i style="background:var(--s-visible)"></i>visible (dashboard)</span>'
              '<span><i style="background:var(--s-sync)"></i>synchronized (standby)</span>'
              '<span><i style="background:var(--armed)"></i>first person armed</span>'
              '<span><i style="background:var(--m-item)"></i>item screen</span>'
              '<span><i style="background:var(--m-map)"></i>map/pause</span>'
              '</div>')
    return svg, legend


def svg_switch_bars(R_: R) -> str:
    S = R_.screens
    bs, bb = S["bins_switch"], S["bins_blink"]
    if len(bs) < 2 and sum(bs) == 0:
        return ""
    W, H, LM, RM, TM, BM = 1000, 130, 40, 14, 10, 22
    PW, PH = W - LM - RM, H - TM - BM
    n = len(bs)
    mx = max(max(bs), max(bb), TH["flicker_per_min"], 4)
    bw = PW / n
    parts = [f'<rect class="lane" x="{LM}" y="{TM}" width="{PW}" height="{PH}" rx="3"/>']
    for g in range(0, int(mx) + 1, max(1, int(mx // 4))):
        yy = TM + PH * (1 - g / mx)
        parts.append(f'<line class="grid" x1="{LM}" x2="{LM + PW}" y1="{yy:.1f}" y2="{yy:.1f}"/><text x="{LM - 5}" y="{yy + 4:.1f}" text-anchor="end">{g}</text>')
    yt = TM + PH * (1 - TH["flicker_per_min"] / mx)
    parts.append(f'<line x1="{LM}" x2="{LM + PW}" y1="{yt:.1f}" y2="{yt:.1f}" stroke="var(--bad)" stroke-dasharray="4 4" opacity=".7"/>')
    for i, (a, b) in enumerate(zip(bs, bb)):
        x = LM + i * bw
        t = S["bin_t0"] + i * 60
        h = PH * a / mx
        parts.append(f'<rect x="{x + 1:.1f}" y="{TM + PH - h:.1f}" width="{max(bw - 2, 1):.1f}" height="{h:.1f}" fill="var(--m-item)"><title>{mmss(t)}: {a} screen switches, {b} blinks</title></rect>')
        if b:
            parts.append(f'<circle cx="{x + bw / 2:.1f}" cy="{TM + PH * (1 - b / mx):.1f}" r="2.5" fill="var(--warn)"><title>{mmss(t)}: {b} blinks</title></circle>')
        if n <= 40 or i % max(1, n // 20) == 0:
            parts.append(f'<text x="{x + bw / 2:.1f}" y="{H - 6}" text-anchor="middle">{mmss(t)}</text>')
    return f'<svg viewBox="0 0 {W} {H}" role="img" aria-label="Screen switches and blinks per minute">' + "".join(parts) + "</svg>"


def render_html(R_: R, verbose=False) -> str:
    log, clock, F, rec, svr = R_.log, R_.clock, R_.F, R_.rec, R_.svr
    T = clock.at
    o: list[str] = []
    title = "VR session report" + (f" {clock.t0:%Y-%m-%d %H:%M}" if clock.ok else "")
    o.append("<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
             "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
             f"<title>{_esc(title)}</title><style>{CSS}</style></head><body><main>")
    o.append("<h1>PenguinScreen2 VR session report</h1>")
    g = [x for x in (log.info.get("disc"), log.info.get("version") and "PenguinScreen2 " + log.info["version"], log.info.get("video_mode")) if x]
    o.append(f'<p class="sub">{_esc(", ".join(g))}</p>')
    meta = [("Log", f"{log.path} ({log.n_lines} lines, {fmt_dur(log.t_last)}" + ("" if R_.shutdown else "; no shutdown lines") + ")")]
    if clock.ok:
        meta.append(("Clock", f"t = 0 is {clock.t0:%Y-%m-%d %H:%M:%S.%f}"[:-4] + f" ({clock.how}; spread {clock.spread:.2f} s)"))
    if rec is not None:
        meta.append(("Recorder", f"{rec.path}: {len(rec.rows)} rows; recorder time + {rec.offset:.2f} s = emulog time ({rec.how})"))
    if R_.steam_dir:
        used = sorted({f.split(".")[0] for f in svr.get("files", [])})
        meta.append(("SteamVR", f"{R_.steam_dir}: " + (", ".join(used) if used else "no events")))
    if R_.session_emulog:
        meta.append(("Note", "the session folder's own emulog.txt is longer than this log; leave out the EMULOG argument to use it"))
    o.append('<dl class="meta">' + "".join(f"<dt>{_esc(k)}</dt><dd>{_esc(v)}</dd>" for k, v in meta) + "</dl>")

    o.append("<h2>What stands out</h2><ul class=\"find\">")
    tags = {"bad": "problem", "warn": "check", "info": "note", "ok": "ok"}
    for sev, text in R_.findings:
        o.append(f'<li class="{sev}"><b class="tag">{tags[sev]}</b> {_esc(text)}</li>')
    o.append("</ul>")

    o.append("<h2>Timeline</h2>")
    svg, legend = svg_timeline(R_)
    o.append('<div class="chartwrap">' + svg + "</div>" + legend)
    o.append('<p class="small">Hover a bar or marker for its details. Triangles in the SteamVR lane are Steam-button dashboard requests, diamonds are '
             'standby / Steam Link inactive, grey ticks are stream resets, purple dots are Steam Link throttle events of 20 ms or more.</p>')

    # ---- session
    o.append("<h2>Session</h2>")
    rows = []
    info = log.info
    if R_.vr_started:
        vr_len = R_.t_vr_end - (R_.t_vr_start or 0)
        rows.append(["VR session", f"{T(R_.t_vr_start)} to {'shutdown' if R_.shutdown else 'the end of the log'} at {T(R_.t_vr_end)}: {fmt_dur(vr_len)}"])
        if R_.focus["by_state"]:
            tot = sum(R_.focus["by_state"].values()) or 1.0
            rows.append(["Focus time", ", ".join(f"{k} {fmt_dur(v)} ({100 * v / tot:.0f}%)" for k, v in
                                                 sorted(R_.focus["by_state"].items(), key=lambda kv: -kv[1]) if v >= 0.5)])
    else:
        rows.append(["VR session", "none in this log"])
    if info.get("runtime"):
        rows.append(["Runtime", info["runtime"] + (f"; HMD '{info['hmd']}'" if info.get("hmd") else "") + (f"; Vulkan {info['vulkan']}" if info.get("vulkan") else "")])
    eyes = [e for e in log.ev if e.kind == "eyes"]
    if eyes:
        rows.append(["IPD and FOV", "; ".join(eyes_text(eyes[0]))])
        rows.append(["Display", f"{eyes[0].d.get('hz', 0):.1f} Hz" + (f"; recommended {info['eye_images'][0]}x{info['eye_images'][1]} per eye" if info.get("eye_images") else "")
                     + (f"; first-person camera FOV {info['fp_fov']:g} deg vertical" if info.get("fp_fov") else "")])
    b = info.get("bindings") or []
    if b:
        rows.append(["Controller profiles", f"left {pretty_profile(b[-1].get('left'))}, right {pretty_profile(b[-1].get('right'))}"
                     + (f"; Steam Frame controller interaction {info['frame_ctrl']}" if info.get("frame_ctrl") else "")])
        seq = []
        for x in b:
            lab = f"L {pretty_profile(x.get('left'))} / R {pretty_profile(x.get('right'))}"
            if not seq or seq[-1][0] != lab:
                seq.append((lab, x["why"], x["t"]))
        rows.append(["Profile history", " -> ".join(f"{lab} ({why}, {t:.1f} s)" for lab, why, t in seq)])
    ctl = [e for e in log.ev if e.kind == "controller"]
    if ctl:
        rows.append(["Controllers live", "; ".join(f"{e.d['side']} {e.d['what']} at {e.t:.1f} s" for e in ctl)])
    if info.get("net_mode"):
        rows.append(["Network", info["net_mode"]])
    if info.get("gpu") or info.get("cpu"):
        rows.append(["PC", f"{info.get('cpu', '')}; {info.get('gpu', '')}"])
    o.append(html_table(["", ""], rows))

    # ---- frames
    o.append("<h2>Frame stats</h2>")
    if not F.get("windows"):
        o.append("<p>No <code>(VR) Frames:</code> lines in this log.</p>")
    else:
        o.append(f'<p class="small">{len(F["windows"])} windows of about 5 s, {_esc(fmt_dur(F["covered"]))} covered. A plateau is an xrEndFrame mean over '
                 f'{TH["end_ms"]:g} ms for over {TH["end_secs"]:g} s; a spike is a window max over {TH["spike_ms"]:g} ms.</p>')
        o.append(html_table(*tbl_overall(R_)))
        t = tbl_episodes(R_, clock)
        o.append("<h3>Episodes</h3>" + (html_table(*t) if t else "<p>None: no plateau, no parking.</p>"))
        t = tbl_spikes(R_, clock)
        if t:
            o.append("<h3>Spikes outside the episodes</h3>" + html_table(*t))
        o.append("<h3>Over time</h3>" + html_table(*tbl_minutes(R_, clock, verbose), flag_col=6))
        strip = frame_strip(R_)
        if strip:
            o.append('<pre class="strip">' + _esc("\n".join(strip)) + "</pre>")

    o.append("<h2>Focus and SteamVR events</h2>")
    t = tbl_timeline(R_, clock, verbose)
    o.append(html_table(*t) if t else "<p>No focus changes or SteamVR events.</p>")
    notes = []
    if svr.get("link"):
        w = max(svr["link"], key=lambda x: x[1])
        notes.append(f"Steam Link: {len(svr['link'])} throttle event(s), worst {w[1]:.0f} ms at {T(w[0])}; {svr['resets']} stream reset(s), "
                     f"{svr['encoder_resets']} encoder reset(s); {svr['stall_count']} compositor 'WaitForAcquire timed out' warning(s).")
    if svr.get("totals"):
        pres, drop, repro, when = svr["totals"]
        notes.append(f"SteamVR compositor totals (run ended {when:%H:%M:%S}): {pres} presents, {drop} dropped, {repro} reprojected.")
    for n in notes:
        o.append(f"<p class=\"small\">{_esc(n)}</p>")

    # ---- first person
    o.append("<h2>First person (armed / disarmed)</h2>")
    A = R_.arm
    if not A["count"]:
        o.append("<p>Never armed in this log.</p>")
    else:
        o.append(f'<p class="small">Armed {A["count"]} time(s), {_esc(fmt_dur(A["armed_total"]))} in all; median {_esc(fmt_dur(A["median"]))}. '
                 + ("This build names the failing guard in the DISARMED line." if A["named_guard"] else
                    "This build only logs the reason class; the recorder column shows what changed at that moment.") + "</p>")
        o.append(html_table(*tbl_arm(R_, clock)))

    # ---- menus / screens
    o.append("<h2>Menus and screen switches</h2>")
    M, S = R_.menus, R_.screens
    if M["count"]:
        o.append("<p>" + _esc(f"{M['count']} menu(s) opened: " + ", ".join(f"{v['n']} {k} ({fmt_dur(v['secs'])})" for k, v in M["by_kind"].items()) + ".") + "</p>")
        if verbose or M["count"] <= 40:
            o.append(html_table(*tbl_menus(R_, clock)))
    else:
        o.append("<p>No menus opened.</p>")
    o.append("<p>" + _esc(f"Comfort blinks {S['blinks']} ({S['per_min_blink']:.1f}/min): " + (", ".join(f"{n}x {k}" for k, n in S["blink_reasons"].items()) or "none")
                          + f". Screen-mode switches {S['switches']} ({S['per_min_switch']:.1f}/min). "
                          f"{len(S['unexplained'])} switch(es) with no menu or arm change next to them; {len(S['quick'])} quick reversal(s) within 1 s"
                          + (f", {S['quick_backed']} of them matched by the game's own menu byte in the recorder." if S["has_rec"] and S["quick"] else ".")) + "</p>")
    bars = svg_switch_bars(R_)
    if bars:
        o.append('<div class="chartwrap">' + bars + "</div>")
        o.append('<div class="legend"><span><i style="background:var(--m-item)"></i>screen switches per minute</span>'
                 '<span><i style="background:var(--warn)"></i>comfort blinks</span>'
                 f'<span><i style="background:var(--bad)"></i>flicker flag ({TH["flicker_per_min"]}/min; only meaningful when no menu explains them)</span></div>')

    # ---- gameplay
    o.append("<h2>Gameplay events</h2>")
    tp = tbl_play(R_, clock)
    o.append(html_table(*tp) if tp[1] else "<p>Nothing logged.</p>")
    tt = tbl_toasts(R_, clock, verbose)
    if tt:
        o.append("<h3>HUD toasts</h3>" + html_table(*tt))

    o.append("<h2>Warnings and errors</h2>")
    tw = tbl_warnings(R_, clock)
    o.append(html_table(*tw) if tw else "<p>None.</p>")
    o.append("<h2>GS speed and drops</h2>")
    tg = tbl_gs(R_, clock)
    o.append(html_table(*tg) if tg else "<p>No Frames lines.</p>")

    if rec is not None:
        P = R_.play
        o.append("<h2>Recorder (state.csv)</h2>")
        st = P.get("rec_status_secs", {})
        rows = [
            ["Samples", f"{P.get('rec_rows', 0)} (one every {P.get('rec_step', 0):.2f} s); {len(P.get('rec_gaps', []))} gap(s) over 2 s"],
            ["Scenario status time", ", ".join(f"{k}{' (' + STATUS_NAMES[k] + ')' if k in STATUS_NAMES else ''} {fmt_dur(v)}" for k, v in sorted(st.items(), key=lambda kv: -kv[1])[:6])],
            ["HP", f"lowest while in game {P.get('rec_hp_min')} of {P.get('rec_hp_max')}; HP 0 at " + (", ".join(T(t + rec.offset) for t in P["rec_zero_hp"]) if P.get("rec_zero_hp") else "never")],
            ["Menu byte", f"up {len(P.get('rec_menu_runs', []))} time(s), {fmt_dur(sum(b - a for a, b in P.get('rec_menu_runs', [])))} in all"],
            ["Aiming / grabbed / pickup reaches", f"{fmt_dur(P.get('rec_aim_secs', 0))} aiming; grabbed {len(P.get('rec_grab_runs', []))} time(s); {len(P.get('rec_pickup_runs', []))} reach animation(s)"],
        ]
        if P.get("rec_idle"):
            rows.append(["Stood still (45 s or more)", ", ".join(f"{T(a + rec.offset)} for {fmt_dur(b - a)}" for a, b in P["rec_idle"][:6])])
        o.append(html_table(["", ""], rows))
    o.append('<p class="small">Thresholds: ' + _esc(", ".join(f"{k}={v}" for k, v in TH.items())) + ". Generated by tools/outbreak-vr/session_report.py.</p>")
    o.append("</main></body></html>")
    return "".join(o)


# --------------------------------------------------------------------------------------------------------------------
# self-test and entry point
# --------------------------------------------------------------------------------------------------------------------

SAMPLE_LINES = """\
[   14.6210] (VR) Frames: submitted 88.4/s (display 90.0 Hz), new GS frames 47.6/s, re-sent 29.4/s, stale 0, shouldRender=false 0, wait->begin mean 0.0 max 0.0 ms, xrEndFrame mean 5.0 max 183.6 ms, queue-lock wait max 21353 us total 60.5 ms/s, copies skipped 0 timed out 0, publishes deferred 4, layers 4, mode headset rate (VR pacer)
[   19.6264] (VR) Frames: submitted 89.9/s (display 90.0 Hz), new GS frames 59.9/s, re-sent 30.4/s, stale 0, shouldRender=false 0, wait->begin mean 0.0 max 0.0 ms, xrEndFrame mean 0.8 max 1.6 ms, queue-lock wait max 1220 us total 10.6 ms/s, copies skipped 0 timed out 0, publishes deferred 0, layers 3, mode headset rate (VR pacer)
[   24.6000] (VR) Frames: submitted 90.0/s (display 90.0 Hz), new GS frames 60.0/s, re-sent 30.0/s, stale 0, shouldRender=false 0, wait->begin mean 0.0 max 0.0 ms, layers 3, mode headset rate (VR pacer)
[   63.3321] (VR) Session state: FOCUSED -> VISIBLE (54013 ms after session creation, 50533 ms in FOCUSED)
[   64.2563] (VR) Session state: VISIBLE -> FOCUSED
[   90.3074] (VR) CameraDriver: ARMED (CRC 32088394) — 0 write op(s), 0 matrix op(s), first-person lookAt.
[  165.4795] (VR) CameraDriver: DISARMED (CRC 32088394) — profile guard failed.
[  166.0000] (VR) CameraDriver: DISARMED (CRC 32088394) — profile guard failed: guard 2 (0x0048BF60, 1 byte(s)) reads 0x3, wants 0x2.
[  167.0000] (VR) CameraDriver: DISARMED (CRC 32088394) — profile guard failed: released (lookAt.releaseWhen 1: record +0x8 reads 0x5).
[  168.0000] (VR) CameraDriver: DISARMED (CRC 32088394) — no valid head pose published.
[  169.0000] (VR) CameraDriver: DISARMED (CRC 32088394) — profile guard failed: guard 3 (0x003AEF75, 1 byte(s)) reads 0x3, wants 0x0.
[  116.6667] (VR) Feedback: shot (handgun), ammo 7 -> 6, right hand 1.00 for 39 ms.
[  562.5403] (VR) Feedback: dry fire, empty gun, right hand 0.20 for 12 ms (1 more not logged).
[  361.0279] (VR) Feedback: hurt, HP 2300 -> 2080 of 2300, both hands 0.45 for 132 ms.
[  466.1963] (VR) Feedback: reload, ammo 1 -> 2, two ticks in both hands.
[   90.3177] (VR) HUD blink (first-person screen).
[   95.2386] (VR) Screen: world-locked.
[   95.2080] OSD [VRStereoScene]: Stereo scene: menu (sep 0.000, conv 55)
[  165.5131] OSD [VRStereoScene]: Stereo scene: base
[  184.9974] OSD [VRStereoScene]: Stereo: first person (sep 0.0147 auto, conv 150)
[   96.2287] (VR) MenuPointer: item screen open, cursor at cell 0 (node 720C10 index 0 x 498 y 108); point-to-select on.
[   99.9449] (VR) MenuPointer: item screen closed (0 steer(s), 2 confirm(s); 0 line(s) not logged).
[  134.7129] (VR) MenuPointer: a menu was up without the profile's liveWhen passing (the map or pause, not the item screen); point-to-select stayed off.
[  642.5732] (VR) CameraDriver: local character grabbed; shaking a controller presses to break free (left stick waggle, Circle and Cross in turn, on your own pad).
[  644.2080] (VR) CameraDriver: no longer grabbed (after 1.6 s).
[   63.2318] (VR) HUD toast: "Controllers paused" (2.0 s).
[    9.4363] (VR) Eyes: IPD 70.1 mm; left fov L/R/U/D -58.9/50.5/49.8/-59.7, right -50.4/59.0/49.6/-59.9; union H +-59.0 V +49.8/-59.9 deg; display 90.0 Hz
"""


def selftest() -> int:
    import tempfile
    path = os.path.join(tempfile.gettempdir(), "session_report_selftest.txt")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(SAMPLE_LINES)
    log = parse_emulog(path)
    try:
        os.remove(path)
    except OSError:
        pass
    fails = []

    def check(name, cond):
        if not cond:
            fails.append(name)

    fr = log.frames
    check("three Frames lines", len(fr) == 3)
    if len(fr) == 3:
        check("xrEndFrame mean/max", fr[0].end_mean == 5.0 and fr[0].end_max == 183.6)
        check("queue-lock total", fr[1].lock_ms_s == 10.6 and fr[1].lock_max_us == 1220)
        check("GS and submitted", fr[1].gs == 59.9 and fr[1].sub == 89.9 and fr[1].hz == 90.0)
        check("older Frames line without xrEndFrame", fr[2].end_mean is None and fr[2].layers == 3)
        check("window start is the previous line", abs(fr[1].t0 - 14.621) < 1e-6 and abs(fr[1].dur - 5.0054) < 1e-3)
    st = log.of("state")
    check("two session states", len(st) == 2 and st[0].d["frm"] == "FOCUSED" and st[1].d["to"] == "FOCUSED")
    dis = log.of("disarmed")
    check("five DISARMED lines", len(dis) == 5)
    if len(dis) == 5:
        check("old DISARMED has no detail", dis[0].d["reason"] == "profile guard failed" and dis[0].d["detail"] == "")
        check("guard named", "scenario status" in dis[1].d["why"] and "reads 3 (door / room transition)" in dis[1].d["why"])
        check("releaseWhen named", "grabbed" in dis[2].d["why"])
        check("head pose reason", dis[3].d["reason"] == "no valid head pose published")
        check("room camera guard", "room camera" in dis[4].d["why"])
    check("shot", log.of("shot") and log.of("shot")[0].d["weapon"] == "handgun" and log.of("shot")[0].d["b"] == 6)
    check("dry fire with more", log.of("dry") and log.of("dry")[0].d["more"] == 1)
    check("hurt", log.of("hurt") and log.of("hurt")[0].d["b"] == 2080)
    check("reload", log.of("reload") and log.of("reload")[0].d["b"] == 2)
    check("blink and screen", log.of("blink") and log.of("screen") and log.of("screen")[0].d["mode"] == "world")
    check("stereo scenes menu, base, first person", [e.d["name"] for e in log.of("scene")] == ["menu", "base", "first person"])
    check("item screen", log.of("item_open") and log.of("item_close") and log.of("item_close")[0].d["confirms"] == 2)
    check("map/pause", len(log.of("menu_other")) == 1)
    check("grab", log.of("grab_start") and log.of("grab_end") and log.of("grab_end")[0].d["secs"] == 1.6)
    check("toast", log.of("toast") and log.of("toast")[0].d["text"] == "Controllers paused")
    check("eyes", log.of("eyes") and log.of("eyes")[0].d["ipd"] == 70.1 and log.of("eyes")[0].d["union_h"] == 59.0)
    check("problems skip DISARMED", not any("DISARMED" in p["text"] for p in log.problems.values()))
    # SteamVR line
    r = parse_svr_line("Mon Oct 12 2026 18:44:44.664 [Info] - Transitioning XrSession from XR_SESSION_STATE_FOCUSED to XR_SESSION_STATE_VISIBLE")
    check("SteamVR time stamp", r and r[0] == dt.datetime(2026, 10, 12, 18, 44, 44, 664000) and XR_TRANS.search(r[2]).groups() == ("FOCUSED", "VISIBLE"))
    # an episode on synthetic windows
    log2 = Log("x")
    t = 10.0
    for mean in [0.8] * 3 + [3.5] + [10.7] * 6 + [9.7] + [0.8] * 3:
        t += 5.0
        log2.frames.append(parse_frames_line(t, f"Frames: submitted 90.0/s (display 90.0 Hz), new GS frames 59.9/s, re-sent 30.0/s, stale 0, "
                                                f"shouldRender=false 0, wait->begin mean 0.0 max 0.0 ms, xrEndFrame mean {mean} max {mean + 0.5} ms, "
                                                f"queue-lock wait max 100 us total 5.0 ms/s, copies skipped 0 timed out 0, publishes deferred 0, layers 3, mode x",
                                                t - 5.0 if t > 15 else None))
    F = analyze_frames(log2)
    check("plateau found", len(F["episodes"]) == 1 and F["episodes"][0]["kind"] == "plateau")
    if F["episodes"]:
        ep = F["episodes"][0]
        check("plateau edges inside their windows", 28.0 < ep["t0"] < 30.0 and 64.0 < ep["t1"] < 65.0)
    print("session_report selftest: " + ("ok" if not fails else "FAILED: " + ", ".join(fails)))
    return 1 if fails else 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Plain-English report on a PenguinScreen2 VR headset session.",
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog="Python 3, standard library only. See the top of this file for what each log gives.")
    ap.add_argument("emulog", nargs="?", help=f"emulator log (default {DEFAULT_EMULOG}, or the emulog.txt in --session)")
    ap.add_argument("--session", nargs="?", const="latest", metavar="DIR", help="session recorder folder (state.csv); no DIR = the newest")
    ap.add_argument("--steamvr", nargs="?", const=DEFAULT_STEAMVR, metavar="DIR",
                    help=f"SteamVR logs folder (default {DEFAULT_STEAMVR}, used automatically when it exists; --no-steamvr to skip)")
    ap.add_argument("--no-steamvr", action="store_true", help="do not read SteamVR's logs")
    ap.add_argument("--html", metavar="FILE", help="also write a single self-contained HTML page with inline SVG charts")
    ap.add_argument("--t0", metavar="WALL", help="wall-clock time of emulog t = 0 ('YYYY-MM-DD HH:MM:SS[.f]') when SteamVR's logs cannot give it")
    ap.add_argument("--app", default="pcsx2", help="the emulator's name in xrclient_<name>.txt (default pcsx2)")
    ap.add_argument("--end-ms", type=float, help=f"xrEndFrame plateau level in ms (default {TH['end_ms']:g})")
    ap.add_argument("--end-secs", type=float, help=f"xrEndFrame plateau length in s (default {TH['end_secs']:g})")
    ap.add_argument("-v", "--verbose", action="store_true", help="full per-minute table, every menu, toast and SteamVR event")
    ap.add_argument("--selftest", action="store_true", help="check the parsers against built-in sample lines and exit")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    if a.end_ms is not None:
        TH["end_ms"] = a.end_ms
    if a.end_secs is not None:
        TH["end_secs"] = a.end_secs
    session_dir = find_session_dir(a.session)
    if a.session and not (session_dir and os.path.isdir(session_dir)):
        print(f"session_report: no session folder found ({a.session})", file=sys.stderr)
        session_dir = None
    emulog = a.emulog
    if not emulog:
        cand = os.path.join(session_dir, "emulog.txt") if session_dir else None
        emulog = cand if cand and os.path.isfile(cand) else DEFAULT_EMULOG
    if not os.path.isfile(emulog):
        print(f"session_report: emulog not found: {emulog}", file=sys.stderr)
        return 2
    steam_dir = None
    if not a.no_steamvr:
        steam_dir = a.steamvr or (DEFAULT_STEAMVR if os.path.isdir(DEFAULT_STEAMVR) else None)
        if steam_dir and not os.path.isdir(steam_dir):
            print(f"session_report: SteamVR logs folder not found: {steam_dir}", file=sys.stderr)
            steam_dir = None
    try:
        rpt = build(emulog, session_dir, steam_dir, a.app, a.t0)
    except OSError as exc:
        print(f"session_report: cannot read {emulog}: {exc} (a log the emulator still has open can be copied first)", file=sys.stderr)
        return 2
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass
    sys.stdout.write(render_text(rpt, a.verbose))
    if a.html:
        page = render_html(rpt, a.verbose)
        with open(a.html, "w", encoding="utf-8") as fh:
            fh.write(page)
        print(f"\nHTML report: {os.path.abspath(a.html)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
