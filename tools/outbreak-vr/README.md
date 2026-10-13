# Outbreak VR tools (Windows test PC)

Small scripts used to reverse-engineer Resident Evil Outbreak (SLPM-65428 / SLPM-65692, English patch)
and test the first-person camera. They talk to a running PenguinScreen2 through PINE
(`[EmuCore] EnablePINE = true`, slot 28011). No game files are stored here.

| File | What it does |
|---|---|
| `pine.py` | PINE client: read/write EE RAM, full 32 MB dump (~1.3 s, stalls the game briefly: offline only), savestates, MemWatch (fork's write/read watch, reports the writer PC and GPRs), scripted VR controllers (`fake_input`, `FakeController`; needs `PCSX2_VR_TEST_INPUT=1`, refused while online-safe or the network adapter is on), first-person camera telemetry (`telemetry`), the menu pointer (`menu_pointer`, `set_test_screen_hit`) and the belt pouches (`pouches`). |
| `vrtest.py` | Headset-free first-person test runner for File #1 and File #2 (the running serial picks the addresses from one per-game table; anything else is refused): drives the VR input path with scripted controllers and checks memory and telemetry (arm, smooth turn, holster, point-to-aim, walk-and-shoot and aim pitch with their online-safe variants, belt, body-follow, savestate yaw, the sprint latch, ad-lib flicks). PASS/FAIL/SKIP per scenario, exit code, optional JUnit XML and HTML report with Headset Window captures. Refuses to run with the network adapter on. `--slot auto` loads each game's usual test saves (File #1 slot 4, `pitch` slot 3 since it fails from slot 4, a test-save quirk; File #2 slot 1 Training Ground, `pitch` slot 2 with the .45 equipped). On File #2 walk-and-shoot waits out the raise step (~20 units forward as the weapon comes up). `--check-profile` needs no emulator: it loads both shipped profiles (PyYAML) and checks every address vrtest uses against them. |
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
| `menutest.py` | Headset-free menu pointer tests for File #1's item screen (offline only): pointer hit maths, steering to each cell, fast click, the pause after a confirm, the command submenu (nothing pressed while its flag is up, no steering after it closes until the dot moves), overrides, closing mid-steer, the map and pause after an item-screen visit (`map-after-items`: point-to-select must stay off and press nothing; pause goes in as Select through `drive.py`), the menu tablet, and the cell rects checked against captures; and the belt pouches on File #1 or File #2 (`pouches`, `pouch-open`, `pouch-submenu`: the reach ticks against the item bytes, the equipped box's against the special item's, each squeeze bringing the cursor to its cell within 1 s, opening the item screen from a pouch, a squeeze refused with a command submenu open, never a confirm). `--slot` loads a savestate first, `--report` saves captures, `--test-uv` points with PINE test hits instead of aiming (works with no OpenXR runtime). |
| `grabtest.py` | Headset-free shake-to-break-free tests for File #1 (offline only, `PCSX2_VR_TEST_INPUT=1`): `idle-shake` (no grab: shaking moves nothing and presses nothing), `baseline` (a grab with no input, about 10 s) and `shake` (shaking the scripted right controller from the load: the grab must end well before the baseline, and nothing more is pressed once it has). Loads `--grab-slot` (default 5, a zombie grabs Kevin) and `--calm-slot` (default 6). |

HUD cards on the null headset: `PCSX2_VR_HUD_TEST=1` shows a test toast every 3 s (it cycles through the
whole font), `PCSX2_VR_FAKE_HANDS` also shows the wrist card on the fake left hand at any angle (NO DATA
outside first person), and `PCSX2_VR_HUD_SELFTEST` runs the HUD self-test once (`(VR) HUD self-test:` in
the emulog). The toast is view-locked 0.65 m ahead and 12 deg below the line of sight (24 deg wide), nearer than the
first-person near field brings the world so nothing seen through it pops in front of it, and it goes in under the
hands (over the screen, the laser and the menu pointer), so a hand passing in front of it hides it instead of the text
drawing over a nearer hand. Every toast is logged as `(VR) HUD toast: "..."`. The 5x7 font has A-Z (lower case is drawn as
capitals), 0-9 and `. , % : - / + ! ? ( ) ' &`, so every item name in `pcsx2/VR/ItemData.cpp` draws in full ("Kevin's
45 Auto", "S&W model 500"; the HUD self-test checks each one). The wrist card's ammo line names the equipped weapon
(its item id from `feedback.weapon`, through ItemData) in place of AMMO: `45 AUTO 7` for Kevin's 45 Auto (an owner's
name is dropped), the name alone for a weapon without rounds (`SURVIVAL KNIFE`), cut to 19 characters with any bracket
closed so it never draws below scale 2 (`GRENADE L. (EXTI) 6`), and still `AMMO 7` for an id the table does not have.
The card is drawn again when the weapon changes, not only its rounds.

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
`PCSX2_VR_HANDS_SELFTEST=1` checks the placement maths, the weapon shapes, the left hand's grip on a fore-end and the
laser by kind at VR start (`(VR) Hands self-test: N checks passed`).

Weapons in hand (both Files, profile `feedback.weapon`): while the weapon is raised in first person the right hand
holds what the local character has equipped, by the item's kind (`VR/ItemData`, Outbreak-Research's item ids): a
handgun, the stun gun, nail gun or ampoule shooter as the pistol (recoil, slide, muzzle flash, as before); a long gun
as a rifle or shotgun (a longer barrel on the pistol's line, a fore-end, a stock back to the wrist; a pump shotgun, the
scoped hunting rifle, a black assault rifle or submachine gun with its magazine, or a fat-barrelled launcher), with a
heavier recoil (4 deg up, 2.5 cm back) and a bigger flash; a melee weapon along the pointing direction at its own
length (knives about 25 cm; pipes, the curved and bent pipes and the wrench 45; the axe, hammer and stun rod 50 with a
head; the scrub brush 40; the poles, square timber, spear and crutches a metre); something to throw in the closed hand
(a molotov by its neck, the throwable stick, the concrete piece); a spray can in the closed hand. With a long gun the
left hand closes round the fore-end once its grip is within 25 cm of it, and opens again past 28 cm. The laser sight
shows only for guns, and a long gun's starts past its own muzzle. A bare-handed raise shows the hand on the
controller; an equipped item that is no weapon shows bare hands too; while the weapon reader cannot tell (no
`feedback.weapon`, or a record that fails its checks) the pistol and laser show from the stance alone, as before.
Every hand's quad now lies on the plane through the middle of the hand (so the hand keeps its true depth in stereo; a
weapon reaching past it is drawn onto that plane) and is fitted to the hand and what it holds as seen from the eye; a
long gun's or a melee weapon's may grow to 1.3 m (the image stays 256 x 256, so it is coarser), and one reaching past
the eye (a pole over the shoulder) keeps the hand and as much of it as fits. An image is drawn again when what the
hand holds, or the left hand's grip, changes. Haptics by kind: a long gun's shot kicks for 80 ms at 1.4 before Haptic
Strength (so it stays stronger than a handgun's 1.0 for 40 ms when the setting is turned down), and the left hand on
its fore-end gets 0.6; a throw 0.35 for 25 ms, a spray 0.25 for 60 ms; a melee weapon nothing (a drop in its count is
no shot); and a melee weapon, something to throw or a spray can never clicks when pulled empty. Logs: each weapon
change `(VR) Hands: the right hand shows a long gun for Shotgun (item 11), with the laser sight.` (or `... the pistol
from the stance alone ...`), and per shot `(VR) Feedback: shot (long gun), ammo 7 -> 6, right hand 1.00 for 80 ms, the
left hand on the fore-end too.`. Test aid on the null headset: `PCSX2_VR_FAKE_WEAPON=<item id>` with
`PCSX2_VR_FAKE_HANDS=gun` puts that item in the fake right hand (logged once as `(VR) Fake hands: the gun is item ...`;
a long gun also holds the fake left hand just under its fore-end), e.g. 11 the shotgun, 106 the long pole, 114 a
molotov, 28 the pesticide spray.

Comfort blink (VR setting `ComfortBlink`, on by default): a view-locked black quad 0.3 m ahead that goes black
over 40 ms, holds 60 ms and clears over 80 ms, on each snap turn and each change between the first-person and
the world screen. The screen change itself is made once the blink is black (`(VR) HUD blink (world screen).`
then `(VR) Screen: world-locked.` about 40 ms later); snap turns log `(VR) HUD blink (snap turn).`.
`PCSX2_VR_BLINK_TEST=1` blinks every 2 s (with the setting on) for `hmdshot.py` captures on the null headset.

Laser sight (VR setting `LaserSight`, on by default): while a gun is raised in first person (never a melee weapon,
something to throw or a spray can), a 4 mm red beam from 2 cm past the impostor gun's muzzle (the pistol's, or a long
gun's further out) straight along the right hand's aim, fading out over 6 m. It has no depth, so it does not stop at
walls or enemies. One quad in the base space turned about the beam to face the head, drawn under the hands and over
the screen; it fades in over 0.15 s with the weapon and goes at once when it is lowered, and a frame looking straight
down the beam goes without it. The first one in a session logs `(VR) HUD: laser sight shown (weapon raised in first
person).`. `PCSX2_VR_FAKE_HANDS=gun` shows it on the fake right hand at any time (with the setting on, and while
`PCSX2_VR_FAKE_WEAPON`, if set, names a gun).

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

Belt pouches (profile `pouches:`, both Files; only while first person is armed, so never on obsrv): a 2 x 2 chest rig
laid out like the item grid (cells 0 and 1 at the upper chest, 2 and 3 below them standing out from the ribs, and the
equipped box out from the belt buckle), hung from a neck pivot under the head and turned to the forward you last recentered to, so
looking down at a pouch does not move it. An open hand reaching into one ticks once, lightly, for an empty cell and
twice, firmer, when the local record's item byte (File #1 +0xC84 + k, File #2 +0xC94 + k) is not 0. The equipped box
shows the special item (Kevin's .45, equipped or not), so its pouch reads the special item's byte (File #1 +0xC88,
File #2 +0xC98). The same reach also shows a 1.5 s toast naming the item: the byte is a record index into the item
table `feedback.weapon` names (File #1 0x3967D0, File #2 0x397B40, 60-byte records), the record must be present, name
its own index and belong to the local slot + 1 (the weapon reader's checks, `GameFeedback::ReadItemRecord`), and
ItemData gives the name with a gun's rounds (`Kevin's 45 Auto 7`), the count of ammo (`45 Auto Magazine x7`) or of a
recovery item holding more than one, the name alone otherwise (`Green Herb`), and `Item <id>` for an id it does not
have. Once per reach: back in the pouch it left less than 0.5 s ago (a hand on the edge) a hand ticks but is not named
again; an empty pouch, a closed grip passing through and any reach while a menu is up (the item screen names its items
itself) name nothing. A record failing the checks is logged once per pouch and byte (`(VR) Pouches: pouch K's item
byte names record R, which fails the item checks (...)`); each name shows as a `(VR) HUD toast:` line. A squeeze opens
the item screen through the belt zone if it is closed (stop, then Start) and asks the menu pointer for the cell
(`MenuPointer::RequestCell`: the same closed-loop D-pad steering once the screen has been up 300 ms, refused while an
item's command submenu is open (the cursor struct + 0x20) or after a confirm, never a confirm); with the screen up a
squeeze just moves the cursor. The zones come first: a hand within a hip zone's reach (radius + 3 cm) that the zone
takes is the zone's, even inside a pouch; elsewhere a hand in a pouch is the pouch's alone (the zones do not see it
there) and its grip is not a menu pointer override. Whether the item screen is up comes from the menu pointer's own
decision (`MenuPointer::ItemScreenLive`), so a squeeze on the map or pause is refused, and a trigger pulled while a
pouch's request waits for the screen never reaches the game. Logs: `(VR) Pouches: N pouch(es) on the seat ...` once per game, `(VR) Pouches: pouch K
(cell C, holds an item|empty) squeezed with the left|right hand; ...` per squeeze, and the menu pointer's `a pouch asked
for cell C; steering there from cell F.`, `... refused (why).` and `... lapsed ...`. PINE `MsgVRMenuPointer` op 2 reads
the pouches (`pine.pouches()`: each pouch's centre in the XR base space, the hand in each, the counters).
`PCSX2_VR_SELFTEST` runs `(VR) BeltPouches self-test:` once, and with it `(VR) Item names self-test:` (ItemData
lookups, the labels, the record checks on a scripted item table, both profiles' item tables).

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

Shake to break free (profile key `breakout`, both Files): measured offline on File #1 (J's Bar, savestate 5, where a
zombie grabs Kevin about 4.7 s after the load), a grab (local record stance +0x08 == 5) lasts about 10 s with no input;
mashing Circle or Cross (about 8 Hz) or waggling the left stick left and right (about 6 Hz) halves it, all of it
together breaks free in about 3.3 s, and Square does nothing. So CameraDriver publishes "local character grabbed" every
vsync (`SpatialControls::SetGrabbed`): every `breakout.grabbed` check holds on the local record (`LocalRecordAny`, so it
holds while `releaseWhen` has handed the camera back: stance 5 and the status byte in normal play, +0xBA0 below 0x18 on
File #1, +0xBB8 below 0x08 on File #2), no `lookAt.pauseWhen` menu is up, the VM runs in VR (not split-screen; the
head-tracked camera may be off), and the network mode is not restricted (never on obsrv; private co-op is fine). A
publish older than 250 ms reads as not grabbed (pause, stall). While grabbed, the Gamepad
(`SpatialControls::ApplyGrabBreakout`, `VR/GrabBreakout.cpp`) watches both grips: a hand is shaking while it moves
faster than `shakeSpeed` (1.0 m/s; the runtime's grip velocity, else the grip position's change, as PINE test input
sends) or reverses direction `reversals` (3) times within `reversalWindowMs` (300) above `reversalSpeed` (0.25 m/s), and
for `holdMs` (250) after. While a hand shakes, the left stick's X goes +1/-1 (`stickHz` 6 full cycles a second) and
Circle (ButtonA) and Cross (ButtonB) take turns (`pressHz` 8 presses a second, `pressMs` 60 each, Circle first), ORed
in: no Circle press while A or the trigger is held, no Cross while B is, and a sideways push of the player's own (past
half) keeps the stick. Each press buzzes the shaking hand (`buzz` 0.35, `buzzMs` 20, times Haptic Strength). Everything
stops on the poll the grab ends, and nothing is pressed in menu mode. Logs: `(VR) CameraDriver: local character grabbed;
shaking a controller presses to break free (...)` and `(VR) CameraDriver: no longer grabbed (after N s).`; the first
grab of a run shows the toast `Shake to break free!` (logged as a HUD toast) when the controllers are live.
`PCSX2_VR_SELFTEST` covers it in the Gamepad self-test (`breakout: ...` checks: nothing without the grab or without a
shake, the alternation and rates, the press length, the buzz, held buttons kept, the stop on the grab's end, the
reversal and runtime-velocity paths). `grabtest.py` checks it in the game, headset-free.

First-person stereo depth (VR settings `NearDepthMetres`, default 0 = the profile's `stereo.firstPerson.nearest`,
below 0 off, 0.45-1.5 m overrides; `RollParallax`, on by default; ini only): true stereo depth now reaches in front of
the screen plane down to `nearest` (1.0 m in both profiles, as you see it, at any strength; it was 0.75 m, and stays
at 1.0 until it has been judged in the headset) instead of flattening everything nearer onto that plane, for
perspective-textured draws only (constant-Q, untextured and 2D/FST draws stay as before), and the parallax turns with
head roll (full up to 25 deg, fading to none at 40 deg) so a tilted head sees no vertical disparity. The game's 2D
text therefore stays on the screen plane, so a counter or shelf nearer than it shows in front of a message drawn over
it: if the game's own text looks doubled or sunk into a surface, `[VR] NearDepthMetres = -1` turns the near field off
(flat, as before wave 4). Our toast sits at 0.65 m, nearer than any `nearest` the profiles have used.
First-person auto stereo (profile key `stereo.firstPerson.auto: true`, in both profiles):
separation = the headset's IPD / the first-person screen's real width and convergence = its distance x
`unitsPerMeter`, so far content lands exactly one IPD apart whatever the aspect ratio. Each arm eases the separation
in from 0 over 30 vsyncs (0.5 s); a later change (the IPD arriving, a strength edit, an aspect change) eases to the
new values. VR `FirstPersonStereoStrength` (0.5-1.5, default 1.0, also on the VR settings page) scales it: below 1
flattens toward the screen, above 1 deepens near objects while distant ones never pass parallel.
`FirstPersonAutoStereo = false` (ini only) uses the profile's numbers, as do a headset that has not reported its eyes,
an IPD outside 45-80 mm and a screen outside the rails. `stereo.firstPerson.ndcWidthRatio` (default 1.0) divides the
auto separation if the draws' NDC is wider than the picture; the once-per-run `(VR) First-person multiview draw: NDC
spans ... (NDC-to-display width ratio 1.000)` line measures it. Each arm logs `(VR) First-person stereo: sep 0.0132,
conv 150 (1.50 m), nearest 1.00 m (floor -0.50), roll parallax on; auto from IPD 63.0 mm (runtime) on a 4.77 x 3.58 m
screen at 1.50 m, strength 1.00; easing in over 30 vsyncs` (or `; profile values (<reason>), strength 1.00; ...`),
then `(VR) First-person stereo: eased in to sep ..., conv ... after 30 vsyncs (... ms).`; a retarget ends
`retargeting over 30 vsyncs (target changed)` and a `NearDepthMetres` edit `nearest changed`.
`PCSX2_VR_FAKE_IPD_MM=<40-90>` replaces the runtime's IPD (logged as `(test)`); offline only, refused (logged once)
while the network adapter is on. `PCSX2_VR_SELFTEST=1` logs `(VR) Auto-stereo self-test: 0 failure(s).`

First-person field of view (VR settings `FirstPersonFov = Auto | Profile`, default Profile until Alex has tried the build, and `FirstPersonFovMarginDeg`,
default 6, 0-20; both on the VR settings page): with Auto the game camera's vertical FOV (`camera.fov`, 0x306380 /
0x313590) is fitted to the headset on each first-person arm, so the game draws the room out to the edge of the view
instead of the old 100 deg picture with black around it. The FOV is twice the further of the eyes' furthest up and down
reach (both eyes, from the runtime's views; the game's projection is symmetric) plus the margin, widened if the 4:3
picture would not reach the furthest side plus half the margin (a side reach counts from the head, so a canted display's
outward cant is added to its eyes' angles), never below the profile's 100 (the floor) nor above
130 (the cap; the game builds its projection as cot(fov/2) through libvu0's sincos, fine far beyond that). It eases in
from 100 over 30 vsyncs on each arm and changes while armed only once, when the headset's eyes first arrive; a setting
change applies at the next arm. Only for a profile whose flat `screen.firstPerson` height is 2 x distance x
tan(camera.fov / 2) (both Outbreak profiles): the first-person screen is then sized to the FOV each frame was rendered
with (carried with the render pose through the view-matrix match; a view that holds still for four vsyncs gets its
frame's FOV followed too, since the matrix does not change with the FOV; `PCSX2_VR_SYNCLOG=1` logs each such step), and
the same size feeds auto stereo and the log.
Objects keep their size; the same picture covers more of the view, so the centre gets softer (100 to about 114 deg is
about 23% fewer pixels per degree; the internal resolution is left alone). Each new choice logs `(VR) First-person FOV: 114.0 deg vertical, fitted to the
headset (the eyes reach up 51.0, down 54.0, sideways 52.0 deg; margin 6.0, screen aspect 1.33; camera.fov 100.0 is
the floor, 130 the cap); easing in from 100.0 over 30 vsyncs.` (or `(profile: VR FirstPersonFov = Profile)`, `(profile
until the headset reports its eyes)`), and the first-person screen line names it with the margin it gives: `(VR)
First-person screen: +-64.0 x +-57.0 deg at 1.50 m (6.16 x 4.62 m), camera FOV 114.0 deg (auto); margin vs headset: yaw
+12.0 up +6.0 down +3.0 deg; ...`. On SteamVR's null headset (+-45 deg) the fit needs 96, so the floor holds: 100 deg,
margin yaw +12.8 up +5.0 down +5.0. `PCSX2_VR_SELFTEST=1` logs `(VR) CameraDriver FOV self-test: 0 mismatch(es).`

Edge fill (VR setting `EdgeFill`, default off until Alex has tried the build, read when VR starts; needs `OpaqueBackground`): behind the first-person
screen the background projection layer shows a dim (half brightness), blurred continuation of the picture's edges instead
of black, so a head turn faster than the screen's margin shows more of the room rather than a black border. It is
head-locked, in the background's own slot (no extra layer; SteamVR is not relied on to take a projection view wider than
the eye's, which a world-locked fill would need): each eye's half holds the picture where the first-person screen sits
in that eye's view with the head where the frame was rendered from (half the IPD to the side, turned by any display
cant), its edge texels stretched beyond it. Built on the GS thread each GS frame with GPU copies and blits only (box
filter down to 32 px wide, dimmed by averaging each texel with a black one, scaled up 5x, cut into a 2 x 128 x 128
canvas, copied into its swapchain) in its own command buffer; it never waits (a fill still on the GPU or a swapchain
image not back skips that frame's update) and goes black behind menus, cutscenes and the world screen and on any
failure. Logs `(VR) Edge fill: enabled ([VR] EdgeFill): ...` at VR start, `(VR) Edge fill: blur chain for a 1536x1344
frame: 1024x896, 6 level(s) down to 32x28, dimmed and scaled up to 160x140.` and `(VR) Edge fill: on behind the
first-person screen: head-locked, 128x128 per eye, ...` the first time it shows; a failure logs `(VR) Edge fill: ...
failed (...); the background stays black for this session.` `PCSX2_VR_SELFTEST=1` logs `(VR) Edge fill self-test: N
check(s) passed, 0 failed.` The Eyes line ends `; views canted out N deg` for a headset whose views are turned outward.

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
`python stereo_metric.py D:\Games\PS2\stereo --expect-floor -0.5` (the shipped profiles' `nearest: 1.0` against the
1.5 m first-person screen gives a floor of 1 - 1.5/1.0 = -0.5 at strength 1; the sidecar's `near_floor` is the value to
expect for other settings, -1 for the old 0.75 m).

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
| Grabbed (shake to break free) | record +0x08 == 5 (action 176, status +0xBA0 stays 0x00); input block +0xF78 (+0 pressed, +8 held word) |
| Cut handlers (NOP to own the camera) | `0x58EAEC` = `0x0C163B1C`, `0x58EAFC` = `0x0C163C8C`, `0x58EB0C` = `0x0C163B60`; keep `0x58EB14` (view build) |
| Door follow-camera eye writes | `0x5A91E0`, `0x5A9340`, `0x5A9410` = `0x0C0D013C` |
| HP / max HP (feedback, wrist card) | record +0x544 / +0x546 (u16) |
| Virus (wrist card) | counter record +0xBAC (u32) over the u32 maximum for the character id u8 record +0xB98 in the table at `0x6E6C70` (GAME overlay data) |
| Bleeding (wrist card) | record +0xC5A (u16, non-zero while bleeding) |

File #2 equivalents are in `bin/resources/vr-profiles/SLPM-65692.yaml` (heading offset not yet confirmed live).
