# Outbreak VR tools (Windows test PC)

Small scripts used to reverse-engineer Resident Evil Outbreak (SLPM-65428 / SLPM-65692, English patch)
and test the first-person camera. They talk to a running PenguinScreen2 through PINE
(`[EmuCore] EnablePINE = true`, slot 28011). No game files are stored here.

| File | What it does |
|---|---|
| `pine.py` | PINE client: read/write EE RAM, full 32 MB dump (~1.3 s, stalls the game briefly: offline only), savestates, MemWatch (fork's write/read watch, reports the writer PC and GPRs). |
| `vpad.py` / `padd.py` | Virtual Xbox 360 pad through ViGEm (`pip install vgamepad`); `padd.py` keeps one plugged in and takes JSON commands on 127.0.0.1:28100, so the game can be driven without focusing its window. |
| `drive.py` | Find the emulator window, screenshot it in the background (PrintWindow), keyboard fallback. |
| `act.py` | `act.py <name> '<json cmd>' ...`: send pad commands, then screenshot. |
| `re_move.py` | Snapshot diff (idle / walk / turn) to find position and heading. |
| `mute.ps1` | Mute (or `-Unmute`) the emulator in the Windows mixer. |

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
| Cut handlers (NOP to own the camera) | `0x58EAEC` = `0x0C163B1C`, `0x58EAFC` = `0x0C163C8C`, `0x58EB0C` = `0x0C163B60`; keep `0x58EB14` (view build) |
| Door follow-camera eye writes | `0x5A91E0`, `0x5A9340`, `0x5A9410` = `0x0C0D013C` |

File #2 equivalents are in `bin/resources/vr-profiles/SLPM-65692.yaml` (heading offset not yet confirmed live).
