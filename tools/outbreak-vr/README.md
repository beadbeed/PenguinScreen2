# Outbreak VR tools (Windows test PC)

Small scripts used to reverse-engineer Resident Evil Outbreak (SLPM-65428 / SLPM-65692, English patch)
and test the first-person camera. They talk to a running PenguinScreen2 through PINE
(`[EmuCore] EnablePINE = true`, slot 28011). No game files are stored here.

| File | What it does |
|---|---|
| `pine.py` | PINE client: read/write EE RAM, full 32 MB dump (~1.3 s, stalls the game briefly: offline only), savestates, MemWatch (fork's write/read watch, reports the writer PC and GPRs), scripted VR controllers (`fake_input`, `FakeController`; needs `PCSX2_VR_TEST_INPUT=1`, refused while online-safe or the network adapter is on) and first-person camera telemetry (`telemetry`). |
| `vrtest.py` | Headset-free first-person test runner for File #1: drives the VR input path with scripted controllers and checks memory and telemetry (arm, smooth turn, holster, point-to-aim, walk-and-shoot and aim pitch with their online-safe variants, belt, body-follow, savestate yaw). PASS/FAIL per scenario, exit code, optional JUnit XML and HTML report with Headset Window captures. Refuses to run with the network adapter on. Run `pitch` with `--slot 3` (it fails from slot 4, a test-save quirk). |
| `netmode.py` | Switches the emulator's network settings: `private <ip>` (our private server, Private Co-op First Person on), `obsrv` (first person off online, 3D screen), `off`, `status`. Close the emulator first. See `HANDOFF.md` for the co-op setup. |
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
| `menutest.py` | Headset-free menu pointer tests for File #1's item screen (offline only): pointer hit maths, steering to each cell, fast click, the pause after a confirm, the command submenu (nothing pressed while its flag is up, no steering after it closes until the dot moves), overrides, closing mid-steer, the map and pause after an item-screen visit (`map-after-items`: point-to-select must stay off and press nothing; pause goes in as Select through `drive.py`), the menu tablet, and the cell rects checked against captures. `--slot` loads a savestate first, `--report` saves captures, `--test-uv` points with PINE test hits instead of aiming (works with no OpenXR runtime). |

HUD cards on the null headset: `PCSX2_VR_HUD_TEST=1` shows a test toast every 3 s (it cycles through the
whole font), `PCSX2_VR_FAKE_HANDS` also shows the wrist card on the fake left hand at any angle (NO DATA
outside first person), and `PCSX2_VR_HUD_SELFTEST` runs the HUD self-test once (`(VR) HUD self-test:` in
the emulog). Every toast is logged as `(VR) HUD toast: "..."`.

Hands at display rate (VR setting `HandsDisplayRate`, on by default, ini only; needs `HeadsetRateSubmit`): the hand
impostors, the laser sight and the wrist card are still drawn on the GS thread, but the VR pacer places them every
display frame from the controller pose located for that frame, moving each image rigidly from the pose it was drawn
for, so they hold still against the world instead of stepping at the GS rate. A hand's image is drawn again only once
the view of it drifts 2 deg (or the eye's distance 3%) from the one it was drawn for, its grip or trigger moves, a
shot plays, or it is 0.5 s old; until then its last image is shown again. VR start logs `(VR) Hands: placed every
display frame on the VR pacer (...)` or `(VR) Hands: placed once per GS present (...)`, and in headset-rate mode the
Frames line is followed every 5 s by `(VR) Hands: placements, re-posed, age mean/max ms, moved since the GS pose
mean/max mm / deg, image drift max deg (N over 15), GS images new/s kept/s, mode ...`. With `PCSX2_VR_SYNCLOG=1` the
pacer also logs once a second `(VR) pacer: hands placement age per display frame mean/max ms ...; jerk hand rms/max mm,
laser rms/max mm` (the jerk is |p(n) - 2p(n-1) + p(n-2)| of each submitted quad centre over three display frames in a
row). Test aids on the null headset: `PCSX2_VR_FAKE_HANDS_SWEEP=1` (with `PCSX2_VR_FAKE_HANDS`; refused while the
network adapter is on) sweeps the fake hands +-12 cm and +-20 deg of yaw at 0.5 Hz on the display clock;
`PCSX2_VR_HANDS_RERASTER=always` draws every visible hand again every present (an image-quality A/B); and
`PCSX2_VR_HANDS_SELFTEST=1` checks the placement maths at VR start (`(VR) Hands self-test: N checks passed`).

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

Menu pointer and tablet (VR settings `MenuPointer`, on by default, and `MenuTablet`, off by default): in a menu
opened from first person (menu mode), the controller's aim casts a 3 mm beam (in the laser's slot, so not while the
laser shows) to a dot lying on the flat menu screen, and both hands are drawn. On File #1's item screen pointing at
an item cell or the equipped box steers the game's cursor there with D-pad presses (2 vsyncs pressed, at least 2
released, checked against the cursor struct at 0x720B80 after each), with a light tick per landing; the trigger stays
Circle and is held back until the cursor arrives, never pressed for you. File #2's item screen works the same (cursor
struct 0x7671F0, Kevin's four cells and the equipped box). While an item's command submenu (EQUIP/USE/PLACE/...) is
open (profile `suspendWhen`: the cursor struct + 0x20, File #1 0x720BA0, File #2 0x767210) nothing is pressed, so the
submenu's highlight never moves under you; the trigger is yours (a fresh pull confirms the submenu entry; one held back
before it opened stays back until you let go, so it never confirms an entry on its own). After any confirm steering
also waits 0.4 s, so a submenu that confirm opens has its flag up before another press could go out, and once a
submenu closes the cursor stays put until the dot moves to another cell. The item screen's cursor struct keeps
reading 1 after the screen closes and the menu-open byte is also up for the map and pause, so the profiles' `liveWhen`
(File #1 u8 0x723F94, 1 while the cursor is in your own grid, where the screen opens; File #2 0x76A604 by the same
offset, UNVERIFIED) must have read 1 since the menu opened before point-to-select runs: on the map or pause it stays
off and the log says `a menu was up without the profile's liveWhen passing (...)` as it closes. Two presses in one
opening that the cursor does not answer also turn it off until the screen opens again (`the cursor did not answer 2
presses ...`). With `HandsDisplayRate` the VR pacer places
the beam and the dot again every display frame from the aim located for it (on the menu screen quad the GS
published), so they keep up with the hands; a PINE test hit, or a frame with no aim pose, keeps the GS placement.
Menus show at zero stereo separation (both profiles' stereo scene `menu` on the menu-open byte, as `pauseWhen`), so
the menu lies on the screen's plane with the dot. `MenuTablet` moves the menu onto a 0.36 m screen 0.70 m ahead of and
0.30 m below the eyes (profile `screen.menu`). Logs: `(VR) MenuPointer: item screen open, cursor at cell N (...);
point-to-select on|off (why).`, `cell A -> B in N press(es), T ms.`, `steering to cell N aborted (why) ...`, `confirm
passed ...` (with suspendWhen: `...; steering waits 400 ms for a submenu's flag.`, or `confirm passed into the
submenu ...`) / `confirm swallowed ...`, `submenu open; nothing is pressed and the trigger passes straight through.` /
`submenu closed; steering waits for the dot to reach another cell.`, `(VR) Menu pointer (pacer): N display frame(s)
placed from their own aim, M with the GS placement, K left out (aim off the screen) in 5.0 s.`, `(VR) HUD: menu pointer
shown (...)`, `(VR) Screen: tablet (menu): ...` and `(VR) Screen transform (menu pointer): ...`. PINE
`MsgVRMenuPointer` (0xE5): op 0 reads the state (`pine.menu_pointer()`, any time; `suspended` covers the submenu and
the wait after a confirm), op 1 sets a test hit (`pine.set_test_screen_hit()`, refused like the test input).
`PCSX2_VR_SELFTEST` runs `(VR) MenuPointer self-test:` once, and `(VR) Menu pointer (pacer) self-test:` at VR start.

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

First-person stereo depth (VR settings `NearDepthMetres`, default 0 = the profile's `stereo.firstPerson.nearest`,
below 0 off, 0.45-1.5 m overrides; `RollParallax`, on by default; ini only): true stereo depth now reaches in front of
the screen plane down to `nearest` (0.75 m in both profiles, as you see it, at any strength) instead of flattening
everything nearer onto that plane, for perspective-textured draws only (constant-Q, untextured and 2D/FST draws stay
as before), and the parallax turns with head roll (full up to 25 deg, fading to none at 40 deg) so a tilted head sees
no vertical disparity. First-person auto stereo (profile key `stereo.firstPerson.auto: true`, in both profiles):
separation = the headset's IPD / the first-person screen's real width and convergence = its distance x
`unitsPerMeter`, so far content lands exactly one IPD apart whatever the aspect ratio. Each arm eases the separation
in from 0 over 30 vsyncs (0.5 s); a later change (the IPD arriving, a strength edit, an aspect change) eases to the
new values. VR `FirstPersonStereoStrength` (0.5-1.5, default 1.0, also on the VR settings page) scales it: below 1
flattens toward the screen, above 1 deepens near objects while distant ones never pass parallel.
`FirstPersonAutoStereo = false` (ini only) uses the profile's numbers, as do a headset that has not reported its eyes,
an IPD outside 45-80 mm and a screen outside the rails. `stereo.firstPerson.ndcWidthRatio` (default 1.0) divides the
auto separation if the draws' NDC is wider than the picture; the once-per-run `(VR) First-person multiview draw: NDC
spans ... (NDC-to-display width ratio 1.000)` line measures it. Each arm logs `(VR) First-person stereo: sep 0.0132,
conv 150 (1.50 m), nearest 0.75 m (floor -1.00), roll parallax on; auto from IPD 63.0 mm (runtime) on a 4.77 x 3.58 m
screen at 1.50 m, strength 1.00; easing in over 30 vsyncs` (or `; profile values (<reason>), strength 1.00; ...`),
then `(VR) First-person stereo: eased in to sep ..., conv ... after 30 vsyncs (... ms).`; a retarget ends
`retargeting over 30 vsyncs (target changed)` and a `NearDepthMetres` edit `nearest changed`.
`PCSX2_VR_FAKE_IPD_MM=<40-90>` replaces the runtime's IPD (logged as `(test)`); offline only, refused (logged once)
while the network adapter is on. `PCSX2_VR_SELFTEST=1` logs `(VR) Auto-stereo self-test: 0 failure(s).`

Stereo dump and `stereo_metric.py` (headset-free, offline only): `PCSX2_VR_STEREODUMP=<dir>[,everyN]` (default
every 30 frames) copies both eyes of the first-person multiview frame to `<dir>/stereo_<frame>_L.png` and `_R.png`
with a JSON sidecar (frame, w, h, sep, conv, near_floor, roll_deg, aspect, plus auto, ipd_mm, screen_w_m,
screen_h_m, screen_d_m, strength, sep_target, ease, ndc_ratio and t_ms), logging `(VR) Stereo dump: <path>` for
each pair; it never waits on the GPU, and is refused (logged once) while the network adapter is on.
`PCSX2_VR_FAKE_HEADROLL=<deg>` adds a constant head roll to the fake head (positive = left ear down; on its own it
gives a still head at yaw 0 / pitch 0 with that roll) and logs `(VR) CameraDriver: fake head roll ... -> euler roll
...` once. `python stereo_metric.py <dir | stereo_NNNNNN>` (numpy + PIL) measures the right eye against the left
on 48x48 tiles of the gradient images: dx percentiles in pixels and in units of sep*width (far content is about
+1, the 5th percentile shows the crossed near field) and the median disparity angle on the display.
`--expect-floor -1` checks the 5th percentile is -1 x sep*width within 15% and never 2 px beyond;
`--expect-roll 10` checks the angle is within 1 deg (at 40 deg and more there is no disparity to measure, which
passes). `--expect-ortho` (needs auto true and ease 1) checks sep = min(strength, 1) x IPD / screen width within 1%
and that the 99th percentile of dx is at most sep*width + 1.5 px, so far content is never more than one IPD apart:
`python stereo_metric.py D:\Games\PS2\stereo --expect-ortho`. `--compare REF` fits this pair's disparity against
another pair of the same view and checks the slope and intercept the two sidecars predict, e.g. strength 1.5 against
1.0: `python stereo_metric.py D:\Games\PS2\s15 --compare D:\Games\PS2\s1` (slope 1.50, intercept -10.2 px at 1536 px
wide). `--ease` checks every arm ramp in a directory (dump with `,2`): sep follows sep_target x smoothstep(ease),
rises monotonically, takes 350-700 ms and the picture follows it: `python stereo_metric.py D:\Games\PS2\ease --ease`.
Exit code 0 = pass, 1 = fail, 2 = bad input (or no ramp captured). Example: null headset, File #1 J's Bar, first
person armed, offline,
`PCSX2_VR_STEREODUMP=D:\Games\PS2\stereo,15`, walk up to the bar counter, then
`python stereo_metric.py D:\Games\PS2\stereo --expect-floor -1`.

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
