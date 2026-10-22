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
| `session_report.py` | Plain-English report on a headset session from its logs (Python 3, standard library only, no emulator needed): `python session_report.py [emulog] [--session [DIR]] [--steamvr [DIR]] [--html FILE]`. The emulog defaults to `logs\emulog.txt` and SteamVR's logs folder is read when it exists; `--session` adds the recorder's `state.csv` (no DIR = the newest folder, and with no emulog argument that folder's own emulog copy). Findings come first, then session (runtime, IPD/FOV, controller profiles, focus time), frame stats from the `(VR) Frames:` lines (submitted/s, new GS frames/s against 59.94, xrEndFrame, queue-lock wait) with plateaus (xrEndFrame mean over 3 ms for over 10 s), parking, spikes and GS slowdowns flagged and lined up with the FOCUSED/VISIBLE changes and SteamVR's dashboard, standby and Steam Link events (the emulog clock is fitted to SteamVR's session-state lines, the method of `research/2026-10-12-steamvr-endframe-research.md`), first-person ARMED/DISARMED periods with their reasons (the failing guard where the build names it, otherwise what the recorder saw change), menus (item screen, map/pause), blinks and screen switches per minute with a flicker check, shots, hurts, grabs, reloads, pickups, toasts, warnings and errors. Every input is optional and older builds give a shorter report. `--html FILE` also writes one self-contained page with inline SVG charts, `-v` lists everything, `--selftest` checks the parsers. |
| `hmdshot.py`, `input_log.py`, `record_play.py` | Headset Window screenshot; pad input + screenshots while someone plays; memory snapshots + screenshots while someone plays. |
| `menutest.py` | Headset-free menu pointer tests for File #1's item screen (offline only): pointer hit maths, steering to each cell, fast click, the pause after a confirm, the command submenu (nothing pressed while its flag is up, no steering after it closes until the dot moves), overrides, closing mid-steer, the map and pause after an item-screen visit (`map-after-items`: point-to-select must stay off and press nothing; pause goes in as Select through `drive.py`), the menu tablet, and the cell rects checked against captures; and the belt pouches on File #1 or File #2 (`pouches`, `pouch-open`, `pouch-submenu`: the reach ticks against the item bytes, the equipped box's against the special item's, each squeeze bringing the cursor to its cell within 1 s, opening the item screen from a pouch, a squeeze refused with a command submenu open, never a confirm). `--slot` loads a savestate first, `--report` saves captures, `--test-uv` points with PINE test hits instead of aiming (works with no OpenXR runtime). |
| `nearcull_test.py` | Headset-free near-cull check on a portable dev install (offline only): `before` runs the File #1 profile as committed at HEAD (from the install's own override folder, removed after), `after` the checkout's. Slot 5 faces the approaching zombie and logs, about 50 times a second, its distance by the cull's measure and whether it was drawn (`+0x198`); slot 6 `down` looks at the body. Captures and a JSON log go to `<install>/logs/nearcull/`. |
| `menupanel_test.py` | Headset-free (or `--xr`, on SteamVR's null headset) checks of `[VR] MenuPanel` on File #1, offline only, on a portable dev install it starts and stops itself: per screen (`items` and `map` from slot 6, `takeit` at the bathroom sink from slot 8, `pause` through Select as Backspace), the share of samples with first person on, the camera's and the game's view matrix's turn against a 40 deg scripted head turn, the game counter, the emulog's menu panel lines, and on the item screen the menu pointer steering cells 1, 3 and 0 (PINE test hits; with `--xr` a ray at the published screen, whose centre, size and hits are printed). For `[VR] MenuPanelRoom` it prints the ini's value, after each screen closes with the head kept turned how soon the view is at the head again (`resume`), and at the end each panel dump's room as shown against its snapshot (the dim). Writes the panel dumps, a stereo dump and the emulog into the out dir (`--dump N`: a panel dump every N panels). Nothing is confirmed: screens close with their own button or Cross. |
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

Dedicated XR queue (VR setting `DedicatedXrQueue`, off by default, ini only, read when the GS device is created): the
OpenXR session gets a Vulkan queue of its own in the GS graphics family instead of sharing the GS queue. Shared, the VR
pacer holds the queue lock through `xrEndFrame` (the runtime uses the session's queue inside it) and every GS submit and
present takes the same lock, so a runtime that blocks in `xrEndFrame` (about 10.7 ms a frame for two minutes after a
SteamVR dashboard blip in Alex's first headset session) stalls the emulation. Dedicated, the GS's own submits and
presents take no lock (only its `vkDeviceWaitIdle` does); the two queues are ordered by two timeline semaphores: each GS
submit signals a gs value, each screen copy or edge fill (the compositor reads of GS textures) waits for the GS's latest
submitted value and signals an xr value, and the GS's next submit waits for that xr value. The compositor's own calls on
the session's queue (swapchain acquire and release, the copies, the hand, HUD and lever uploads) still take the lock
against the pacer. It needs a graphics family with a queue to spare and `VK_KHR_timeline_semaphore`; without them, or if
the device or the session cannot be created that way, the session shares the GS queue as before. VR start logs
`(VR) XR queue: dedicated (family F, index I); the GS queue no longer waits on the runtime.` or `(VR) XR queue: shared
with the GS (<why>).` The `(VR) Frames:` line splits its queue-lock waits as `total X ms/s (GS thread Y, GS submits Z)`
(Y is what the emulation waited, Z the GS's own submits and presents, about 0 when dedicated) and ends with
`XR queue dedicated` or `shared`. If the GS ever has work not yet submitted when the compositor reads its frame, that
work is submitted first and `(VR) XR queue: the GS had work not yet submitted ... (N so far)` is logged (the first time
and every 600th); it should not show in normal play.

Pacer copies (VR setting `PacerCopies`, off by default, ini only, read when VR starts; needs `DedicatedXrQueue` in
effect and `HeadsetRateSubmit`): the VR pacer does every swapchain call itself, so the GS thread (and with it the
emulation) never calls into the runtime or takes the queue lock in its end-of-frame work. A runtime that blocks in
`xrEndFrame` then holds up only the pacer. The GS copies each frame (both eyes) into one of three images of the
compositor's own with a copy recorded into its OWN current command buffer (no extra submit, no lock; it goes to the GPU
with the GS's next submit, whose timeline value is known as it is recorded), builds the edge fill beside it the same way,
and draws the hands, HUD cards, lever cards and zone markers into a three-slot staging ring each (host memory), then
publishes the layers at once. The pacer, after `xrBeginFrame`, claims what it will read, acquires and waits each
swapchain image within 2 ms, copies the ring and the staging into its own swapchains in one submit on the session's
queue (waiting for the GS's latest submitted timeline value, signalling the next xr value), releases them and ends the
frame. A layer goes out only with the image it was built for, as before (the screen's pair with its frame, a hand or
zone marker with its drawing, the toast and the laser's slot with their content); otherwise with what went out with the
image its swapchain holds, or parked. The ring is safe by the claims plus the xr timeline wait every GS submit after a
pacer read carries; the staging slots by the pacer's fences. Because the frame reaches the GPU with the GS's next
submit, the headset sees it about one GS frame (16.7 ms) later than with the GS-side copy; `PCSX2_VR_PACER_COPIES_FLUSH=1`
has the GS submit right after the copy instead (an A/B for that latency). The stereo dump keeps working (it reads the
frame in the same command buffer, after the copy). The alternate-eye debug mode (`PCSX2_VR_INTERLEAVE`) goes as mono.
VR start logs `(VR) Pacer copies: on ([VR] PacerCopies): ...`, `off`, or why it stays off (`needs the dedicated XR
queue`, `needs HeadsetRateSubmit`); the pacer's swapchains log `(VR) Pacer copies: swapchain N made on the pacer (...)`
as they are made. With it on the Frames line ends `pacer copies: frames F/s age mean/max ms (GS submit not in G, image
not back I, busy B), uploads U/s`: frames copied into the eye swapchains and how long after their publish, pacer frames
whose newest frame's GS submit had not gone in yet, images the runtime had not handed back within the budget, frames
with all three copy command buffers still on the GPU, and CPU-drawn layers copied. With it on, `queue-lock wait ... GS
thread` should read 0 and `copies skipped` count only a full ring (rare).

The `(VR) Frames:` line (every 5 s, from the thread that ends the frames: the VR pacer, or the GS in legacy mode):
frames submitted per second against the display rate, new and re-sent GS frames, stale and `shouldRender=false`
frames, then `mean/max ms:` for `xrWaitFrame` and `xrBeginFrame` (each from its call to its return: SteamVR may pace in
either, or in `xrEndFrame`; legacy adds `wait->begin`, how long a waited frame sat until the GS present began it) and
`xrEndFrame` split into `queue lock` (waiting for the session's queue) and `runtime` (the call itself). `display lead vs
first min/mean/max` is each frame's `predictedDisplayTime` minus the steady clock at its `xrWaitFrame`'s return,
against the first frame's (logged once as `(VR) Frames: display lead baseline N ms`): a runtime that starts predicting
one frame further out shows as a jump of one display period (11.1 ms at 90 Hz). Then the queue-lock waits, copies and
deferrals as before, the last frame's `layers`, `layer changes` (submits whose layer array, each layer's type,
swapchain and image size in order, differs from the one before; SteamVR re-creates a layer position's texture when
what sits there changes size, about 150 times in 11 minutes in the first headset session) and `no projection`
(submits with layers but no projection layer under them; with `OpaqueBackground` on this should read 0 after the first
frame: a frame whose views cannot be located, as around a focus change, sends the background with the last views
located instead, counted as `views reused`).

Layer positions: every layer has a fixed position in the frame's layer array, in the order they are drawn: the
background, the screen's layers, the lever cards, the zone markers, the laser (or menu beam), the menu pointer's dot,
the toast, the hands, then the wrist card, the vignette and the blink. A lever card, zone marker, HUD card or hand that
is not shown this frame but whose swapchain has been released once stays in its position parked: its own image, 1 cm
across, 100 m behind the head in the VIEW space, so no eye sees it (valid OpenXR, nothing zero-sized or transparent for
a runtime to special-case). SteamVR shows our quads as overlays by array index and re-creates a position's texture
when what sits there changes size, and the volatile cards (laser, dot, toast, zone markers) must stay under the hands
for their depth, so leaving one out moved every layer after it; parked, a toggle changes nothing in the array. A layer
whose swapchain was never released stays out (it joins once, the first time it shows), and parked layers only take
capacity the shown ones leave (15 plus the background). In first person that is 14 layers every frame, `layers 14
(parked N)` on the Frames line, and `layer changes` should stay at 0 in steady play (a screen size change, the edge
fill turning on or off and a layer's first show still count one). The HUD cards' images are now 256 x 256 like the
hands' and the zone markers' (the laser and beam, blink and dot stretched by whole factors, the wrist card padded and
its quad grown to match; the toast keeps 1024 x 160), so a position that does change occupant mostly keeps its size.
VR start logs `(VR) Layers: each layer keeps its position ...`; `PCSX2_VR_NO_PARKED_LAYERS=1` leaves a layer that is
not shown out again (logged), for an A/B of what the parked overlays cost SteamVR (`xrEndFrame` runtime mean and max).

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

Game weapon models (VR setting `GameWeaponModels`, on by default, ini only, read every frame): both hands are the CC0
hand model (`VR/HandMesh`: its pistol pose round a gun or the controller, the index curling in with the trigger, its fist
round anything else and round a long gun's fore-end), and the right hand's weapon, the holstered one and the slung long
gun are the game's own model of the item (`VR/GameModels`, decoded from the disc at VR start, memory only) at its real
size, drawn with `VR/MeshRaster` (2 x 2 samples) into the same impostor images. The weapon sits where the game itself
attaches it to the character's hand: the profile's `feedback.weapon.hold` (File #1: the local record's u16 `+0xCA8` is
the weapon type, an entry of the 0x44-byte table at `0x6E5944` gives the model's origin in the hand bone's frame and its
turn), mapped onto the hand model's wrist; a gun's barrel is then turned onto the controller's aim, so melee weapons
now run through the fist along the handle, as the game holds them. In the holster a melee weapon hangs shaft down from
the belt. A weapon whose model did not decode or whose entry is not known keeps the placeholder look, logged once
(`(VR) Hands: item 3 (Magnum Handgun) keeps the placeholder shape: ...`); `false` is today's look throughout. The
equipped weapon's entry is logged when it changes (`(VR) Weapon hold: the equipped item 400 (Kevin's 45 Auto) is weapon
type 7 (the item table says 7); its entry: grip (...), turn (...) deg, with a muzzle.`), and in a session the 5 s
`(VR) Hands:` line adds `mesh images N/s mean X max Y ms` (GS thread). Test aids: `PCSX2_VR_DUMP_HANDS=<folder>` draws
every weapon of the scenario's item table once in game (a session or the headset-free scripted input), in the hand from
the eye and the side, in the holster or sling and from 1.8 m, into PNG files there and logs each image's time (the
images show the game's models: they stay on the PC, never in the repo); `PCSX2_VR_MESH_SAMPLES=1` or `4` changes the
sample grid. The finding and costs: `research/2026-10-14-weapon-models-in-hand.md`.

Zone markers (VR setting `ZoneMarkers`, on by default, checkbox under Laser Sight; read live): while first person
drives the camera with no menu up, the body zones show where a squeeze works. The equipped weapon (`feedback.weapon`,
an ItemData weapon kind; nothing for bare hands, a non-weapon item or an id the table lacks) hangs in the right-hip
holster zone while it is lowered, drawn by HandModel as the hand holds it but without the glove: a handgun, the stun
gun, nail gun or ampoule shooter, or a melee weapon muzzle or blade down and 15 deg back so the grip stands up toward
the hand (`HandModel::HolsterPose`), something to throw or a spray can upright on the belt. A long gun (shotgun,
rifle, SMG, launcher) is slung over the right shoulder in the backpack zone instead, barrel up behind the shoulder
(15 deg back, 10 deg out: `HandModel::SlingPose`; it sits beside and behind the head, so you mostly see it looking up),
and nothing hangs at the hip then. It goes the moment the weapon is raised (it is in your hand) and comes back when it
is lowered; it is drawn 1.45x brighter while the right hand is in that zone's reach. With a long gun equipped a
right-grip squeeze over the shoulder raises it (R1, at once and held while squeezed, no stop first: the profiles'
`PressedLongGun` bind on the backpack zone); with anything else equipped it opens the item screen (Start) as before,
and the hip holster presses R1 either way. The route is fixed as the squeeze begins, so changing weapon mid-squeeze
never turns a held R1 into Start. A small dim bag (6 cm) marks the left-hip belt zone and faint rings (half each
pouch's ball, so the shipped rig's rows, which line up along the line of sight from the eyes, stay apart) mark the five
pouches where BeltPouches places them; a ring or the bag brightens, with a faint fill, while its hand is in reach (the
pouch the hand is in, per the pouch telemetry; the zone's own StepZone verdict for the bag and the weapon). A zone
marker is placed from the input snapshot's head exactly as StepZone places that zone (HeadAnchor, then PlaceControl:
the head's position, turned with its yaw), and the zone must be armed (its `when` guards); the pouches come from the
last pouch poll (seat frame: the neck and the recentered forward). While the head looks ahead (tipped down less than
25 deg) no bag or ring within 20 deg of the line of sight is drawn, so nothing covers the middle of the game; looking
down at your body they all show. Layers: the markers have no room of their own in the compositor's 15 (plus the
background, SteamVR's 16): they are built after everything else from what is left, the pouches' rings dropped first,
then the bag, then the weapon, and go in under the HUD's under-hands cards and the hands (which reach into them), over
the screen. First person usually leaves room for all three (the screen's one or two, two hands, a few cards). Each is
a 256 x 256 image of its own drawn again only when what it shows changes (the weapon, lit or not, which pouches) or
the view of it drifts 2 deg or 3% in distance; otherwise its last image moves with its zone. Hidden while the HUD
cards are off for the session. VR start logs `(VR) Zone markers: on: ...` (or `off`), the first marker `(VR) Zone
markers: swapchains created (...)`, and nothing per frame; `PCSX2_VR_HANDS_SELFTEST=1` also runs `(VR) Zone markers
self-test: N checks passed` (the placement against StepZone, the holster and sling poses, the press route, the
budget, the view rule, the rings and the bag, and the redraw rules).

Hand targeting (VR setting `HandTargeting`, off by default, ini only, read live; a prototype for File #1 played offline,
not in private co-op or online): it measures the right hand against the J's Bar Handgun's trigger volume and writes
nothing to the game and presses nothing. `CameraDriver` publishes the numbers `camera.lookAt` places the eye with (the
character, eye height, the true base yaw, the recenter) so `GameFromVr` / `VrFromGame` can map the grip position into
game centimetres and back (the eye_forward offset is not part of the map). `ItemReach::EvaluateHandTarget` walks the
pickup cue's trigger list (same filters and node cap) for nodes whose item is id 0 AND whose volume centre lies within
10 cm of the J's Bar counter pistol's (8493.5, 140, 12922.5 game cm, from a RAM dump), so no other Handgun is ever a target
(the item, centre, tolerance and the range / keep / near distances are the profile's `reach.hand`: edit the YAML in the build's `resources/vr-profiles` and restart the emulator, no rebuild)
and reports the hand's distance to its floor quad (0 inside) and height range, whether it is `inside` (no margin) and
whether the game's own forward search would pick the same node. A dim wireframe box (a fourth zone-marker layer, the first
dropped when layers are short) shows the volume, brighter within 30 cm and brightest inside; the right hand gets one
light tick on entering it. A target exists while the volume's middle is within 3 m of the hand. Emulog lines:
`(VR) Hand targeting: on (offline File #1, right hand, the J's Bar counter pistol's trigger only; read-only)` and `... mapping check: eye->head
error X mm, round trip Y mm` once per first-person arm; `target found: node ..., item ..., volume centre game (...) cm,
VR (...) m` and `target lost ...`; and once a second while a target exists the hand (VR m, game cm), the horizontal and
vertical distance to the volume, to its middle, `inside` and `forward search agrees`, and a projection check: the volume
middle's direction (yaw, pitch) from the game's own view matrix (0x3062E0) against its direction from the head pose that
frame was rendered from (the newest matched frame, the very write that view matrix was built from: a consistent pair, so
the check measures the mapping, not display timing; it is skipped while the camera's base yaw has turned since that frame,
and meant for standing still), with the difference in degrees. A difference of about a degree, steady while standing still,
means the map puts a world point where the game's camera sees it; it does not show that the volume matches the visible
pistol (the volumes are 2-5 times the objects) or cover display latency.

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

Reload gesture (VR setting `ReloadGesture`, on by default, on the VR settings page; profile key
`feedback.weapon.reloadButton: L1`, both Files; `VR/ReloadDriver.cpp` wires the pure detector `VR/ReloadGesture.cpp`
into `VRInputSource::PollEvents`). The game's own reload (research `2026-10-12-reload-timing-research.md` 2a): L1 held
with the weapon LOWERED and the character standing still; File #1 loads a round 0.31-0.36 s after the press and one
every 0.33 s while held, File #2 the whole magazine at once; nothing with R1 held, while walking, without spare ammo or
with a full magazine. The gesture, every input poll while first person is armed, a gun is equipped (ItemData `IsGun`)
and no game screen is up: with the stance raised (local record +0x08 == 2 now or in the last 0.3 s), the left grip
squeezed inside the magazine spot (12 cm round the belt zone's centre, placed exactly as that zone: from the head,
turned with its yaw) takes a magazine (a short tick in the left hand); carried to within 15 cm of a point 4 cm under
the gun hand's grip it is ready (a firmer tick); opening the grip there, or pulling the left trigger, is one reload
request (timeouts: 1.5 s from the grab to the gun, 2 s held at the gun; tracking gaps up to 0.2 s waited out). From
the grab until that grip opens again the belt zone and the belt pouches do not see the left hand (no Start, no pouch
squeeze or name) and its L1 is held back (a trigger pull at the gun is the reload, not an L1 with the weapon raised).
A request (not tried while walking, toast `Stand still to reload`, nor when every item cell is read and none holds ammo,
toast `No spare ammo`) holds back every control on the player's pad bound to R1 (the right bumper, the holster's
Pressed, the backpack's PressedLongGun) until the stance leaves 2 (at least 0.05 s, at most 0.3 s: if it stays raised,
another pad holds R1 and nothing is pressed), then holds the Gamepad's control bound to L1 (LeftTriggerPress) until the
rounds have not risen for 0.8 s (with File #1's action byte: also as soon as the reload action ends, or 0.35 s with no
reload action at all), or after 4 s unless a round came in the last 0.45 s (a 15-round Handgun from empty takes 4.9 s),
6 s at most; the left stick past 0.25, a game screen, or first person or the gun going away let go at once. Then L1 goes
up, and 0.05 s later R1 comes back: a bumper or holster grip still held raises the weapon again by itself; a left
trigger still held stays at 0 until it is let go once (release-first). While it runs the first-person hand keeps
showing the gun (`ReloadDriver::Reloading`, read by CameraDriver for `HandModel::SetWeaponHeld`) although the game holds
it lowered, so it does not jump to the holster for a second. Each round loaded is felt through the weapon reader's
existing reload tick (both hands, per rise). A reload that loaded nothing toasts `No spare ammo` (no ammo in
the cells, an empty gun, or a gun below the most rounds it has shown this session) or `Magazine full`, once per attempt.
Only the player's own R1 is held back and their own L1 pressed, nothing is written, so it runs wherever first person
arms (offline and private co-op; never on obsrv). The first time a gun is raised in a session the toasts `Reload: lower
gun, hold left trigger` and, with the gesture on, `Or grab a magazine at your left hip` (3.5 s each) explain both ways.
Logs: `(VR) Reload gesture: on (magazine at the left hip; L1 with the weapon lowered).` once per game (or `off (why)`),
`(VR) Reload: 3 -> 7 rounds in 1.4 s (gesture).` per reload (or `nothing loaded in 0.4 s, the magazine is full (...)`,
`not tried, ...`, `stopped, ...`), `(VR) Reload gesture: cancelled: too slow.` for a gesture dropped after a grab.
`PCSX2_VR_SELFTEST` runs `(VR) Reload gesture self-test: passed.` (the detector on scripted hands) and `(VR) Reload
driver self-test: passed.` (the lowering, L1 hold and stop rules and R1 coming back after L1, against a fake File #1
and File #2, plus the stance hysteresis and the release-first latch).

Physical melee (VR setting `PhysicalMelee`, off by default, ini only; File #1's profile `melee` block and its
`camera.silence` entries with `whenSetting: PhysicalMelee`; `VR/PhysicalMelee.cpp`, polled from
`VRInputSource::PollEvents` after the reload; research `2026-10-14-melee.md`, `2026-10-14-melee-coop.md`). Offline (no
network adapter, not online-safe), or online with `PhysicalMeleeOnline` (below), first person armed, a melee weapon
equipped (the local record's equipped item record, ItemData
`Melee`), stance normal or raised, no game screen: the right controller's point `tip` (0.40 m) up the handle is tracked;
a stroke starts above `speed` (2.2 m/s, smoothed over 25 ms), ends below `rest` (0.8 m/s) for 80 ms, and counts once,
when it has travelled `travel` (0.20 m) with its straight-line distance at least `straightness` (0.6) of its path, within
0.6 s. A count presses Circle for `pressMs` (100 ms) on the VR Gamepad's pad (R1 too, held from the request until the
swing is over, when the weapon was down; given up after `raiseMs`), waits up to 0.3 s for the game's swing (sub-action
3), ticks the right hand (0.35, 25 ms) as it starts, and buzzes it (1.0, 90 ms) the first time an enemy's HP drops during
it or `contactMs` (250 ms) after; the next swing is taken `cooldownMs` (250 ms) after. Every poll it can, it writes the
VR weapon's matrix in game space (`HandModel::ModelFrame`, the placement the hand draws the model with, mapped point by
point with `CameraDriver::GameFromVr`) to the block at `0xFF000` with the magic, the local record and the game counter's
stamp; the caves use it for the local player's attack object while it is at most 6 frames old, and the original copy
otherwise; cave C (`0xFE100`, called by the seven melee motions' spawn events instead of `FUN_00565C80`) opens the local
player's hit window at motion frame 2 under the same test, every other record keeps the game's own frame. When the
override stops the whole block is zeroed. Online (`PhysicalMeleeOnline`, off by default, ini only; private co-op only,
never online-safe): every console runs every player's swing from the state the attacker sends and the room's master
(the first slot of NETAQ.BIN's row for the room, `melee.online`) decides the enemies' HP and reactions for everyone, so
there the swing always presses the game's own attack and the block and caves go in only while this console is that
master and the caves' free RAM read zero (`VR::PhysicalMelee::Decide`: `CoopPress` / `CoopFull`; CameraDriver's
`whenSetting` gate asks `ProfileDB::SilenceSettingCoopLive`). Logs: `(VR) Physical melee: on (...)` or `off (why)` once per game, `(VR) Melee: swing at 4.1 m/s (0.21 m of
stroke); the game's swing started 33 ms later.`, `(VR) Melee: contact, enemy 49 HP 600 -> 540.`, `(VR) Melee: swing not
made: <why>.`, online `(VR) Melee: online, room N: this console (slot S) runs its enemies, so the hit follows the VR
weapon.` or `... the console of slot M runs its enemies ..., so the hit is the game's own swing (press only).`;
`PCSX2_VR_MELEE_LOG=1` adds a line twice a second (the mode, the gates, the peak speed, the phase, the block).
`vr_selftests` runs `Physical melee` (the detector on scripted swings, stabs, sweeps, shakes and tracking loss; the
driver against a fake game; the matrix), `Melee profile parser`, the File #1 entry check, `Physical melee online` (the
mode table, the room master read, the free-RAM check) and `Physical melee caves` (caves A, B and C run from the
profile's words on a small EE interpreter, for the local record and for others).

Contact mode (VR setting `PhysicalMeleeMode`, `Contact` by default, `Press` for the above; ini only; profile
`melee.contact`, `camera.silence` entries with `whenSetting: PhysicalMeleeContact`; research `2026-10-14-vr-melee.md`).
Wherever the caves may run (`Overrides`: offline, or `CoopFull` online), nothing is pressed: the detector and driver are
idle and the character never enters an attack action. Every poll the block is written, the weapon's own hit shape is
read from the game (the weapon type `+0xCA8` -> its attack id -> the attack's shape: the pipe's capsule from x = +40 to
-60 of the model, r 30; the knife's sphere r 30 at x = -10) and placed with the block's matrix; each live enemy (present,
kind 2, HP above 0) gives its hurt spheres (`+0x1E0` count, `+0x1D8` shapes, `+0x1DC` world centres); the shape, swept
from the last poll, touches one when the segment comes within the summed radii plus `lead` (8 cm). The tip speed is the
far hit point's in the headset's space (smoothed 20 ms). `StrikeStep`: an armed enemy touched at or above `speed`
(1.8 m/s, knives 1.5) is struck, once per swing (re-armed when the tip has been under `rearm` for `rearmMs`, or the
weapon off it for `leaveMs`), never while the last strike's object lives or within `cooldownMs`; it waits up to `holdMs` (35 ms) after the first
touch while the tip still speeds up (the strength from the fastest it went). A strike writes the
window (`+0x18`), the strength (`+0x1C`, `Strength`: `light` 0.6 rising to 1 at `firm` 3 m/s, 1 to `heavy` 5.5 m/s,
then `strong` 1.5), its stamp (`+0x28`) and the strike number (`+0x0C`); cave D (`0xFE200`, called from `0x5CDF64`
before the frame's hit test) calls `FUN_005521B0` for the local record and gives the new attack object that life;
cave E (`0xFE400`, at `FUN_00636990`) multiplies the game's critical multiplier by the strength for that object's node
only; cave D goes on to cave F (`0xFE500`), which runs the frame's hit test `FUN_005A9DB0` itself and then, within a
strike's window + 3 frames, handles the record's hit notice `+0x116` (written by the hit, cleared by `FUN_005ABA30` next
frame; read otherwise only by the swing's stage `FUN_006474A0`) the way that stage does: for a pipe, pole, brush or spear
(weapon class 4 or 5) that hit, `+0xBA0` bit 31 and `FUN_0062CDA0`, so the next character loop spends durability through
the game's `FUN_0062D2C0` -> `FUN_0062CDE0` -> `FUN_00581A00` (Kevin's pipe 5 a hit, 40 -> 35; at the last use it bends,
1 -> 20, `FUN_00584580`; knives nothing), never during the canned swing. Online (`CoopFull` only; the host writes
`kPartnerSwingFrames`, 30, to `+0x30`) cave F also shows partners a swing: after a strike one type-1 state message
(`FUN_0064C620`) built while the record's action bytes read stance 2, sub 3, phase 0 (put back at once with `+0xBA0` and
the frame's last-type mark `0x23BF0C`; this console never runs the swing), and 30 frames later the swing's end as the
game sends it (its filter `FUN_006337C0(record, 2, 3)` first; nothing while hurt); `+0x8C` counts them (starts in the low
byte, ends in the next). `PCSX2_VR_TEST_PARTNER_SWING=1` sets the frames offline too (the sender returns at once
offline), for the headset-free checks. A buzz at the strike (0.65-1.0 for 50-110 ms by strength). Logs: `(VR) Melee: contact mode (...)` / `press mode`,
`contact shape for weapon type T: the game's capsule, 100 long, radius 30.`, `strike, enemy N (sphere S) at V m/s,
strength X.`, `hit, enemy N HP a -> b, reaction class C (...), zone Z, M ms after the strike (h of s strikes landed).` or
`strike on enemy N made no hit (...)`; with `PCSX2_VR_MELEE_LOG=1` also each poll's touch. `vr_selftests`: `Physical melee
contact` (strength, segment and sphere distances, the swept touch, scripted swings: fast, very fast, at 20 polls a
second, a slow sweep, a waggle inside a zombie, the back swing, the same swing held inside, two zombies, a live strike,
over the head), the File #1 `melee.contact` and `PhysicalMeleeContact` entry check, and `Physical melee contact caves`
(caves D, E and F on the interpreter with stubs for the game's spawn, multiplier, hit test, sender, filter and item mark: a
fresh request, served, the canned swing, stale requests and blocks, a live strike's countdown, a refused spawn, the
strike's node and every other node; the hit notice for each weapon class, code 1, 2 and 3, the canned swing, a pending
use, no strike lately, another slot; the partners' swing: its message bytes, everything put back, one at a time, not
while hurt, its end through the filter).

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

Depth warp (`VR/DepthWarp`, research note `research/2026-10-13-depth-export.md`, section "Our own warp"). `[VR] DepthWarp`
(off by default, read when VR starts; needs `PacerCopies` off and `HeadsetRateSubmit` on; turns `DepthLayer` off): the GS
thread copies each frame (both eyes) and its exported depth into a ring of three compositor images instead of the eye
swapchains, and the VR pacer redraws each eye's swapchain every display frame through a 128 x 128 mesh displaced by the
depth, from the eye's display-time position onto the same render-pose quad. Logs `(VR) Depth warp: on ...`, `mesh pass
ready`, `source ring made`, `the pacer draws into an eye swapchain`, `the eyes are warped ...` or `drawn as they are
(<why>)`, and `frames go unwarped (<why>)` once per change, and every 5 s `(VR) Warp: N frames warped, M drawn as they
are, ...; GPU x ms mean, y ms max per display frame`. Offline test aids: `PCSX2_VR_DEPTHWARP_TEST_SHIFT=<metres>` moves
the warp's eyes to the head's right (the layers stay), `PCSX2_VR_DEPTHWARP_DUMP=<dir>` writes one `warp_source.ppm` /
`warp_warped.ppm` pair of the first eye (never commit them).

Depth layer and depth probe (`VR/DepthExport`, research note `research/2026-10-13-depth-export.md`). `[VR] DepthLayer`
(off by default, read when VR starts): in first person the shown frame goes to the runtime as one projection layer at
the pose it was rendered from instead of the two screen quads, with its depth chained to each view
(`XR_KHR_composition_layer_depth`, a `D32_SFLOAT` depth swapchain per eye), so the runtime's own reprojection can use
it. With the switch off the instance does not even enable the extension: no depth swapchain, no depth copy, no
projection layer. Logs `(VR) Depth layer ([VR] DepthLayer): ... enabled.` (or that the runtime does not offer it),
`(VR) Depth layer: on ...` (or why it stays off: no extension, `PacerCopies` on, no D32 format), `(VR) XR depth
swapchain created (eye N): ...`, `(VR) Depth export: first frame: ...` (which depth target, the fit and the range the
runtime is given) and `(VR) Depth layer: first projection layer with depth: ...` (each view's pose and fov). While the
depth is missing for a present the frame goes as quads, and `(VR) Depth export: no depth for the shown frame: <why>.`
says why once per reason. `PCSX2_VR_DEPTHLAYER_NODEPTH=1` sends the same projection layer with no depth chained, an A/B
for whether the runtime's use of the depth shows. Headset-free check, offline only: `PCSX2_VR_DEPTHPROBE=<seconds>`
(no session needed) reads the exported depth back every that many seconds without waiting on the GPU and logs
`(VR) Depth probe #N: ...` with a 5 x 3 grid in metres; `PCSX2_VR_DEPTHPROBE_AT="x,y,z;..."` adds points in metres
from the render camera (x right, y up, -z ahead), each read at its pixel in each eye's layer (the layer's disparity
for that depth applied) with a 9 x 9 window's nearest / median / farthest; `PCSX2_VR_DEPTHPROBE_DUMP=<skip>,<count>`
also writes `depthprobe/depth_N_L0.pfm` and `_L1.pfm` (metres) and the same present's colour `colour_N_L0.ppm` into the
install folder (never commit them); `PCSX2_VR_DEPTHPROBE_LIVE=1` reads the depth target as it is at the present instead
of the snapshot, to show what the hand-off would give without it. Example, slot 7 (J's Bar counter), scripted head as
`handtest.py` launches it: `PCSX2_VR_DEPTHPROBE=2 PCSX2_VR_DEPTHPROBE_AT="0.063,-0.349,-1.040"` reads the counter top
under the Handgun's trigger volume, 1.045 m ahead. `PCSX2_VR_SELFTEST` / `vr_selftests` run `(VR) Depth export
self-test: N checks passed.` (the conversion, the runtime's reading of the range, the projection views, the tracker).

Menu panel (`VR/MenuPanel`, research note `research/2026-10-13-menu-panel.md`). `[VR] MenuPanel` (off by default, ini
only, read live; File #1, any profile with `camera.lookAt.pauseWhen`): a game screen no longer hands the camera back.
CameraDriver keeps writing the eye and target from the head under it ("camera only": no stick turn, aim, walk-and-shoot
or body-follow; menu mode gives the sticks and buttons to the menu as before; the sprint latch lets go), so the room
keeps rendering in stereo. In the GS each draw is classed (room: depth-tested perspective; clear: a depth write over the
whole buffer; overlay: flat; backdrop: a blended overlay over the whole frame) and the colour target is snapshotted
just before the screen's first draw (its depth clear or backdrop after the room, else any overlay after it with a
screen up); Merge shows the snapshot, so the first-person screen has the room alone, a compute pass writes what the
screen's draws changed (left eye, opaque, the rest clear) into the panel image, and the screen's backdrop draws are
skipped while it is up. The compositor shows that image on a world-placed quad 1.0 m ahead of the eyes and 0.12 m
below them, 0.80 m tall at 4:3, placed where the head looks as the screen comes up, and publishes it as the menu
pointer's screen. Logs: `(VR) CameraDriver: a game screen is up; first person keeps the camera ...`, `(VR) Menu panel:
the screen's draws split off the shown frame ...` (or `no panel (<why>)`), `(VR) Menu panel: swapchain created ...`,
`shown ...` / `hidden ...`, `(VR) Screen transform (menu pointer): menu panel centred at ...`; at VR start `(VR) Menu
panel: on ...` or why it is off (`PacerCopies` on: screens pull away as before). While a screen is up the desktop window
shows the room without it and the depth export declines (the screen's depth clear covers the room's). Debug, offline
only: `PCSX2_VR_MENUTRACE=<vsyncs>` logs every GS draw of that many vsyncs as `(VR) MTRACE` lines when a screen comes
up, 1.5 s later and when it goes (`SNAP` the snapshot point, `SKIP` a backdrop left out);
`PCSX2_VR_MENUPANEL_DUMP=<dir>[,everyN]` writes `menupanel_<n>_panel.png`, `_room.png`, `_frame.png` and `_shown.png`
(the present's picture after the presentation lift; never commit them) with a coverage line and `the room as shown is
N% of its snapshot's brightness`. `PCSX2_VR_SELFTEST` / `vr_selftests` run `(VR) Menu panel self-test: N checks, 0
failed` and `(VR) Menu panel feel self-test: N checks, 0 failed`.

Calm menus (`[VR] MenuPanelRoom`, `MenuPanelRoomBrightness`; research note, "Calm menus"). With `MenuPanel` on, what the
room behind the panel does while a screen is up. `Freeze` (default): `MenuPanel::RoomFreeze` holds the pose camera.lookAt
last wrote as the screen came up. CameraDriver writes the camera from it every vsync (not predicted, stamped as the head
now), so the game's view matrix holds and every frame is placed at that render pose; the head drives the camera again as
the screen goes. `Freeze` and `Dim` dim the room: `MenuPanel::RoomLookNow` (GS thread) eases the amount in over 0.25 s and
out over 0.2 s while the shown frame is a split room. `VR::PresentationLift` folds it into the lift's ShadeBoost pass
(brightness times `dim^gamma`, saturation 0.8), also without an XR session. While the room is held, the compositor
sends no edge fill (`MenuPanel::RoomFrozen`). The panel's place is `MenuPanel::PanelPlacer`: it is shown where the head
looks (yaw only) and held in the world; past 60 deg away it glides round (0.15 s time constant). Logs: `(VR) CameraDriver:
the room holds still behind the panel ...`, `(VR) CameraDriver: the screen is gone; the head drives the camera again
(held N s, the head D deg from the held view)`, `(VR) Menu panel: the room behind it at N% brightness ...`, `(VR) Menu
panel: the head turned more than 60 deg away; the panel comes round ...`, and `hidden (N images copied, brought round N
time(s))`.

VR presence (VR setting `VrPresence`, off by default, ini only, with `VrPresencePeer = host:port`, `VrPresencePort`
(47800) and `VrPresenceKey`; File #1's profile `presence` block; `VR/VrPresence.cpp`, polled every vsync after the HUD;
research `2026-10-21-vr-presence.md`, which has the two-machine test). In private co-op two PenguinScreen2 consoles send
each other their players' VR head and hands, in the game's coordinates (`CameraDriver::GameFromVr`), over a UDP side
channel of their own (156-byte packets, 30 a second, a key hash, a session and a sequence number; never the game's
connection), and each draws the partner's VR head (a shell and a dark visor) and teal CC0 hands on the partner's character
in the same room, in one impostor (`ZoneMarkers::kPartner`; its swapchain only once a partner shows up, skipped with a log
line when SteamVR has none left). The peer must be a private IPv4 address (LAN, Tailscale 100.64/10, 127/8; a name is
resolved and checked); nothing is sent and no socket is open unless first person is armed in private co-op, or for the
offline loopback test with `PCSX2_VR_PRESENCE_LOOPBACK=1`, the game offline and a 127/8 peer. Reads game memory only.
Logs `(VR) Presence: ...` (on, live / waiting, channel open / closed with its counts, a partner is drawn or why not);
`PCSX2_VR_PRESENCE_LOG=1` adds the positions every 2 s (always on in the loopback test); `PCSX2_VR_PRESENCE_DUMP=<dir>`
writes one `presence_partner.png` (never commit it). `presence_echo.py` is the loopback partner (it sends each packet
back as another slot); `presence_test.py <dev install> [slot] [--dump <dir>]` runs the whole loopback check headset-free
(it sets the presence keys in that install's ini, refuses to start beside another emulator or SteamVR, and kills only its
own emulator). `vr_selftests` runs `VR presence (side channel, partner head and hands)`.

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
| Camera focus slot (near-cull spares it) | u8 `0x3AEF74`; near-cull radii f32 `0x6D6CF0` (class 1 = characters at +4, 80); the cull is `FUN_005902F0`, NOP'd at `0x590304` in first person; the focus character's draws skipped at `0x3452E0`, `0x3450F8`, `0x560C74` (`research/2026-10-13-near-cull.md`) |
| View matrix (row vectors; look = -col 2) | `0x3062E0` |
| Stance / action / stick magnitude | record +0x08 (1 normal, 2 aiming) / +0x54F (0 idle, 1 walk, 3 run, 128 aim) / +0xF98 (u16) |
| Grabbed (shake to break free) | record +0x08 == 5 (action 176, status +0xBA0 stays 0x00); input block +0xF78 (+0 pressed, +8 held word) |
| Cut handlers (NOP to own the camera) | `0x58EAEC` = `0x0C163B1C`, `0x58EAFC` = `0x0C163C8C`, `0x58EB0C` = `0x0C163B60`; keep `0x58EB14` (view build) |
| Door follow-camera eye writes | `0x5A91E0`, `0x5A9340`, `0x5A9410` = `0x0C0D013C` |
| HP / max HP (feedback, wrist card) | record +0x544 / +0x546 (u16) |
| Virus (wrist card) | counter record +0xBAC (u32) over the u32 maximum for the character id u8 record +0xB98 in the table at `0x6E6C70` (GAME overlay data) |
| Bleeding (wrist card) | record +0xC5A (u16, non-zero while bleeding) |
| Room (VR presence) | u16 `0x3065AC` (the local player's); record +0x1E2 (u16, each character's); a used record's max HP +0x546 is not zero |

File #2 equivalents are in `bin/resources/vr-profiles/SLPM-65692.yaml` (heading offset not yet confirmed live).
