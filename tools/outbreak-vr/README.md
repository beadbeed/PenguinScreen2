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

File #2 equivalents are in `bin/resources/vr-profiles/SLPM-65692.yaml` (heading offset not yet confirmed live).
