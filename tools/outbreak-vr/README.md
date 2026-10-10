# Outbreak VR tools (Windows test PC)

Small scripts used to reverse-engineer Resident Evil Outbreak (SLPM-65428 / SLPM-65692, English patch)
and test the first-person camera. They talk to a running PenguinScreen2 through PINE
(`[EmuCore] EnablePINE = true`, slot 28011). No game files are stored here.

| File | What it does |
|---|---|
| `pine.py` | PINE client: read/write EE RAM, full 32 MB dump (~1.3 s, stalls the game briefly: offline only), savestates, MemWatch (fork's write/read watch, reports the writer PC and GPRs), scripted VR controllers (`fake_input`, `FakeController`; needs `PCSX2_VR_TEST_INPUT=1`, refused while online-safe or the network adapter is on) and first-person camera telemetry (`telemetry`). |
| `vrtest.py` | Headset-free first-person test runner for File #1: drives the VR input path with scripted controllers and checks memory and telemetry (arm, smooth turn, holster, point-to-aim, walk-and-shoot and aim pitch with their online-safe variants, belt, body-follow, savestate yaw). PASS/FAIL per scenario, exit code, optional JUnit XML and HTML report with Headset Window captures. |
| `vpad.py` / `padd.py` | Virtual Xbox 360 pad through ViGEm (`pip install vgamepad`); `padd.py` keeps one plugged in and takes JSON commands on 127.0.0.1:28100, so the game can be driven without focusing its window. |
| `drive.py` | Find the emulator window, screenshot it in the background (PrintWindow), keyboard fallback. |
| `act.py` | `act.py <name> '<json cmd>' ...`: send pad commands, then screenshot. |
| `re_move.py` | Snapshot diff (idle / walk / turn) to find position and heading. |
| `mute.ps1` | Mute (or `-Unmute`) the emulator in the Windows mixer. |
| `launch_vr.ps1` / `launch_fakevr.ps1` / `restart_fake.sh` | Launch in VR through SteamVR, or headset-free with a fake swaying head (`restart_fake.sh <deg> <slot>` also loads a slot and mutes). The previous `emulog.txt` is kept as `logs/emulog-<time>.txt`. |
| `test_fakevr.py` | Headset-free checks: `armed` (patches, no spin, walks where it looks), `features` (hidden body holds, body follows view, camera holds while the item screen is open), `flat` (savestate repair). |
| `null_driver.py` | `on`/`off`: switch SteamVR to its simulated null headset (backs up and restores `steamvr.vrsettings` exactly). |
| `sync_sweep.sh` / `sway_metric.py` | On the null headset: world wobble in the Headset Window for render-pose sync variants (`time`, or a `syncFramesBack` value). With `PCSX2_VR_SYNCLOG=1` the emulog also gets, once a second, `(VR) predict:` (how far frames still turn when shown, and the horizons) and `(VR) edge: rms/max deg, H ms` (how far the first-person frame sits from the head as it is shown, the leading-edge band that `lookAt.predict` shrinks; against the fake head when `PCSX2_VR_FAKE_HEADPOSE` is set). |
| `session_recorder.py` | Started by `launch_vr.ps1` and `Launch-PenguinScreen2-SteamVR.cmd`: records game state (~4/s) and a screenshot every 5 s to `D:\Games\PS2\sessions\<time>` until the emulator closes, plus the emulog. Screenshots only when the network adapter is on. |
| `hmdshot.py`, `input_log.py`, `record_play.py` | Headset Window screenshot; pad input + screenshots while someone plays; memory snapshots + screenshots while someone plays. |

HUD cards on the null headset: `PCSX2_VR_HUD_TEST=1` shows a test toast every 3 s (it cycles through the
whole font), `PCSX2_VR_FAKE_HANDS` also shows the wrist card on the fake left hand at any angle (NO DATA
outside first person), and `PCSX2_VR_HUD_SELFTEST` runs the HUD self-test once (`(VR) HUD self-test:` in
the emulog). Every toast is logged as `(VR) HUD toast: "..."`.

Comfort blink (VR setting `ComfortBlink`, on by default): a view-locked black quad 0.3 m ahead that goes black
over 40 ms, holds 60 ms and clears over 80 ms, on each snap turn and each change between the first-person and
the world screen. The screen change itself is made once the blink is black (`(VR) HUD blink (world screen).`
then `(VR) Screen: world-locked.` about 40 ms later); snap turns log `(VR) HUD blink (snap turn).`.
`PCSX2_VR_BLINK_TEST=1` blinks every 2 s (with the setting on) for `hmdshot.py` captures on the null headset.

Laser sight (VR setting `LaserSight`, on by default): while the weapon is raised in first person, a 4 mm red
beam from 2 cm past the impostor pistol's muzzle straight along the right hand's aim, fading out over 6 m. It
has no depth, so it does not stop at walls or enemies. One quad in the base space turned about the beam to face
the head, drawn under the hands and over the screen; it fades in over 0.15 s with the weapon and goes at once
when it is lowered, and a frame looking straight down the beam goes without it. The first one in a session logs
`(VR) HUD: laser sight shown (weapon raised in first person).`. `PCSX2_VR_FAKE_HANDS=gun` shows it on the fake
right hand at any time (with the setting on).

Comfort vignette (VR settings `ComfortVignette`, off by default, and `VignetteStrength`, 0.2-1.0, default 0.6):
while first person moves the view without the head, a view-locked quad 0.5 m ahead (6 m square, about 161 deg
across) darkens the edges of the view: clear out to 45% of its ring's radius, ramping to opaque black at the
ring (about 58 deg off the line of sight) and opaque beyond it to the quad's edges. Its opacity is the strength
times the motion CameraDriver publishes each vsync (`CameraDriver::ComfortMotion()`): the larger of the smooth
stick turn's rate against `smoothTurnDegPerSec` and walking (the move stick's push past its deadzone, or the
character's own horizontal speed with 1.5 m/s and up counting fully), eased in over 0.15 s and out over 0.3 s.
Snap turns count nothing (the blink covers them). It goes at once when first person does (pause, a menu, a door,
a cutscene, a savestate load). The first one in a session logs `(VR) HUD: comfort vignette shown (...)`.
`PCSX2_VR_VIGNETTE_TEST=1` ramps it up and down every 2 s at any time (with the setting on) for `hmdshot.py`
captures on the null headset.

Rules when testing: never change emulation speed; never pause, savestate or dump RAM while online
(obsrv drops clients after ~30 s without packets and bans speed changes); do not run the virtual pad
while an anti-cheat game is open.

## File #1 addresses used by the first-person profile

From the Outbreak Tracker DLL, HeySnippy's ModernCam code sites and a disassembly of our GAME.BIN
(the English patch does not move code or bss).

| What | Address |
|---|---|
| Player records | `0x476DD0 + slot * 0x10E0`; local slot u8 `0x48BF7D` |
| Position | record +0x38/+0x3C/+0x40 (f32 x, y up, z) |
| Heading | record +0x92 (s16, 0x10000 = 360 deg, 0 faces +Z, forward = (sin h, cos h)) |
| Camera eye / target / FOV | `0x306338` / `0x306344` (f32 vec3) / `0x306380` (f32 deg) |
| Overlay id (GAME = 3) | u32 `0x540004` |
| Scenario status (2 = in game) | u8 `0x48BF60` |
| Room camera mode (0 = normal) | u8 `0x3AEF75` |
| Item screen / map / pause open | u8 `0x48BF7E` (1 while open) |
| Camera focus slot (near-cull spares it) | u8 `0x3AEF74`; near-cull radii f32 `0x6D6CF0` (class 1 = characters at +4, 80) |
| View matrix (row vectors; look = -col 2) | `0x3062E0` |
| Stance / action / stick magnitude | record +0x08 (1 normal, 2 aiming) / +0x54F (0 idle, 1 walk, 3 run, 128 aim) / +0xF98 (u16) |
| Cut handlers (NOP to own the camera) | `0x58EAEC` = `0x0C163B1C`, `0x58EAFC` = `0x0C163C8C`, `0x58EB0C` = `0x0C163B60`; keep `0x58EB14` (view build) |
| Door follow-camera eye writes | `0x5A91E0`, `0x5A9340`, `0x5A9410` = `0x0C0D013C` |
| HP / max HP (feedback, wrist card) | record +0x544 / +0x546 (u16) |
| Virus (wrist card) | counter record +0xBAC (u32) over the u32 maximum for the character id u8 record +0xB98 in the table at `0x6E6C70` (GAME overlay data) |
| Bleeding (wrist card) | record +0xC5A (u16, non-zero while bleeding) |

File #2 equivalents are in `bin/resources/vr-profiles/SLPM-65692.yaml` (heading offset not yet confirmed live).
