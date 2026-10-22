# Outbreak VR: status, what's next, what needs investigation

Start here. This page sums up first-person VR Resident Evil Outbreak in PenguinScreen2. **Scope is File #1 (SLPM-65428)
only** until it feels right. The direction, and the plan to make it feel like real VR, is in `ROADMAP.md`.

Details live elsewhere:
- `ROADMAP.md`: Alex's goal, why it still feels like a flat game in a headset, and the road to real VR;
- `HANDOFF.md`: older history (10-10 to 10-12); no longer updated;
- `README.md`: every tool and debug switch;
- `bin/resources/vr-profiles/README.md`: the profile format;
- `research/`: game memory facts and design research;
- `STEAM-FRAME.md`: the Steam Frame standalone port (on hold).

Updated by the Windows PC session on 2026-10-13 at night: no headset session that day; everything below dated 10-13 is
checked offline or on SteamVR's null headset only.

**2026-10-13 in short:**
- **Direction:** Alex chose PenguinScreen2 as the foundation. The native-port option (REO) was assessed and parked:
  `research/2026-10-13-reo-assessment.md`.
- **Local builds:** they are the everyday loop now (see Local builds below).
- **New work, on a chain of pushed branches, each built on the one before:**
  1. `handtarget`: hand targeting;
  2. `nearcull`: nothing vanishes up close;
  3. `depthproj`: the shown frame's depth, and `DepthLayer`;
  4. `menupanel`: `MenuPanel`, menus on a panel without the pull-away.
- **Merging:** none of these are merged into `headset1` yet. `menupanel` contains all of them, so after Alex's headset
  check, merge `menupanel` into `headset1`.

**2026-10-14 playtest build:** branch `playtest` = `menupanel` + `reloadwire` (the reload gesture) + `meshraster` (the mesh
renderer, not drawn with yet), built locally with LTO like CI (25 of 25 self-tests). Launch with
`D:\Games\PS2\Launch-Playtest-SteamVR.cmd`. It runs `D:\dev\PenguinScreen2-build
un-playtest`, a separate portable
install with its own settings (`HandTargeting`, `MenuPanel` and `ReloadGesture` on, `DepthLayer` off), memory cards and copies
of the File #1 savestates. Headset-free speed in slot 5 matches the CI build.
`D:\Games\PS2\Launch-Playtest-Models-SteamVR.cmd` runs the same with `handmodels` on top (the game's own weapon models in
the hand, holster and sling, and the CC0 hand model; `GameWeaponModels`, on), from `run-playtest-models`: built with LTO, 26 of
27 self-tests. Checked on SteamVR's null headset (2026-10-14): the models draw in a real session (mesh images 0.5 ms
mean, 60 frames/s submitted), and the run found SteamVR's 16-swapchain limit starving the menu panel; fixed (12e76d9: the
edge fill gives its swapchain up and the creation is retried), also on `playtest`. It also has `doors` (`SeamlessDoors`, off:
your own door opening keeps first person; 15 camera-shot calls NOP'd, offline only; the words are checked offline, a door
opened with the switch on is not yet).
Since 2026-10-14 21:15 `run-playtest-models` (and `Playtest\Slot*.cmd`) runs branch `melee` (handmodels + `PhysicalMelee`, off),
an LTO build with 30 of 30 self-tests; its `resources` junction points at the `melee` worktree.

**Morning of 2026-10-15, for Alex:** `D:\Games\PS2\Playtest\Slot5/6/7/9.cmd` run `nightly` (local LTO build, 39 of 39 self-tests),
in `D:\dev\PenguinScreen2-build
un-playtest-models` with its own settings. On there: `PhysicalMelee` (Contact), `MeleeHitJolt`,
`SeamlessDoors`, `MenuPanel` (+ `MenuPanelRoom = Freeze`), `HandTargeting`, `ReloadGesture`, the game weapon models;
off: `BeltPouches`, `DepthLayer`, `DepthWarp`, `PhysicalMeleeOnline`. A/B for leaning (slot 7, lean 10-20 cm at the counter):
`DepthWarp = true` (our own warp: the review's critical fix is in, and it ran clean on the null headset after the merge:
6,216 frames warped, 0 skipped, 0.06-0.08 ms GPU, the frozen menu room warped too), then `DepthLayer = true` **with
`ZoneMarkers = false`** (otherwise SteamVR's 16-swapchain limit is full and the depth layer gives its swapchains to the hands
within seconds), then neither (in `run-playtest-models\inis\PenguinScreen2.ini`, `[VR]`).
Everything new since 10-14 evening is checked offline (and the menus and the warp on the null headset) only.

**2026-10-15 overnight build: branch `nightly`** = `vrmelee` (contact-driven melee: `PhysicalMeleeMode = Contact`, the game's own
attack spawn called on the VR weapon's contact, strength from swing speed; `meleecoop`'s local-only caves and
`PhysicalMeleeOnline`; `BeltPouches` off) + `panelfeel` (`MenuPanelRoom = Freeze`: the room holds still and dims behind a
menu). Local LTO build, 36 of 36 self-tests, profile check passes. `run-playtest-models` (and `D:\Games\PS2\Playtest\Slot*.cmd`)
runs it, with `PhysicalMelee = true`; its `resources` junction points at the `nightly` worktree. Everything new is checked
offline (and the menus on the null headset) only. Next headset check: melee in slot 5 (equip a pipe or knife yourself: hard
swings knock down, light ones flinch, nothing on small moves; `PhysicalMeleeMode = Press` to compare), and menus (is Freeze
calm, is 0.55 dim right, try `Dim` / `Live`).

**2026-10-15 branch `nightfix`** (from `nightly`; local build, 39 of 39 self-tests; checked offline, headset-free only):
- **The menu room is warped too.** With `DepthWarp` (or `DepthLayer`) and `MenuPanel` on, the room behind a game screen
  now has its depth: it is copied at the same point as MenuPanel's room colour, before the screen's own depth clear. So a
  lean still has parallax behind a menu under `Freeze`. Offline: every present exported through a 6.9 s item screen, the
  counter at 1.045 m as before it (`research/2026-10-13-depth-export.md`, "The menu room keeps its depth").
- **Door by stick works in first person.** The 10-15 failure was where the script stood: against the door's jamb, 9 cm
  off the doorway. The game's own push then picks another trigger (kind 24, does not lead out). Walking into the
  doorway opened it: the walk-in trigger, room 1 to 4. First person did not interfere (`research/2026-10-14-doors.md`,
  "Door by stick"). Nothing changed in code for it.

**2026-10-21 branch `truestereo`** (from `nightly`; local build, 43 of 43 self-tests; checked offline headset-free and on
SteamVR's null headset only): **`[VR] TrueStereo`** (off), road step 5's real step. Each eye's layer is drawn with that
eye's own view and projection (the GS maps the game's VU1 vertices through the eye) and goes to the runtime as a projection
layer of the eyes, with no first-person screen. Against the game's own renders from a moved camera: +-5 cm, 99-100% of
tiles within 1 px; +-10 deg turns, 83-97% (the rest is the strip outside the game's view, a walking NPC, subtitle timing).
Null headset: the layer is accepted every frame, eyes 63 mm apart, each eye's depth chained with `DepthLayer`; GS thread
+0.5 ms per vsync, GPU +1.6 ms per frame. Next: Alex in the headset with `TrueStereo = true` (and `DepthLayer = true`):
the counter and Handgun at arm's length, a zombie close, leaning, rolling the head, the HUD text, a menu with `MenuPanel`.
Research and the plan: `research/2026-10-21-true-stereo.md`.

## Where things are

| | |
|---|---|
| Working branch | `headset1` (all of tonight's work, merged and pushed). `pc-firstperson` and `coop-firstperson` are still at the 10-12 morning build and get `headset1` once Alex is happy with it. CI builds of `headset1` go through `wave4-integration`, the branch with the CI cache. |
| Installed on the PC | `D:\Games\PS2\PenguinScreen2`, CI build of `3eb2524` (the second headset session ran on it). `headset1` has more since: the menu re-press and the CI self-tests. |
| Headset sessions | **1:** 2026-10-12 ~18:45, build `e6dbd77`. **2:** ~22:07, build `3eb2524`. Both were File #1. Logs: `D:\Games\PS2\sessions\<time>` and `logs\emulog-headset-20261012.txt`. Summary: `tools\outbreak-vr\session_report.py --session`. |
| Hand targeting | Branch `handtarget` (from `headset1` 43de18c): the read-only prototype, `reach.hand` in the profile, the pickup cue and zone marker self-tests in CI (21 suites, run 38488998201 green at 8dbff61). Checked offline, headset-free, 2026-10-13 (below). Not in the headset yet. |
| Near cull | Branch `nearcull` (from `handtarget`): zombies and partners stay drawn up close, your body stays hidden. A File #1 profile change only; checked offline, headset-free, 2026-10-13 (below). Not in the headset yet. |
| Depth layer | Branch `depthproj` (from `nearcull`): the shown frame's depth exported beside its colour, in metres, and (`[VR] DepthLayer`, off) sent to SteamVR with the first-person frame as a projection layer with depth. Road step 5's foundation. Checked offline and on the null headset, 2026-10-13 (below). Not in the headset yet. |
| Menu panel | Branch `menupanel` (from `depthproj`): with `[VR] MenuPanel` (off) a game screen keeps first person, the room keeps rendering around you, and the screen's own draws are split off the frame onto a flat panel in front of you, where the menu pointer aims. Road step 1, stage 1. Item screen, map, take-it prompt and pause checked offline headset-free and on the null headset, 2026-10-13 (below). Alex played it on 2026-10-14 ("it all works", but the room behind the panel swims). |
| Calm menus | Branch `panelfeel` (from `meleecoop`): `[VR] MenuPanelRoom` (with `MenuPanel` on). `Freeze`, the default, holds the camera at the view it had as the screen came up, so the room stays put in the world behind the panel; the room is dimmed to 55% and the panel comes round only after a turn of more than 60 deg. `Dim` dims without the hold; `Live` is the 10-14 behaviour. Checked offline headset-free and on the null headset, 2026-10-14 (below; `research/2026-10-13-menu-panel.md`, "Calm menus"). Not in the headset yet. |
| Dev installs (local builds, portable, own settings) | `D:\dev\PenguinScreen2-build
un-dev`: `nearcull` with `HandTargeting = true`. `run-depth`: `depthproj`. `run-menu`: `menupanel` (set `MenuPanel` / `DepthLayer` in its `inis\PenguinScreen2.ini`). `run-panel`: `panelfeel` (`MenuPanel = true`, `MenuPanelRoom = Freeze`). `D:\Games\PS2\PenguinScreen2` is unchanged (CI build `3eb2524`). |
| Steam Frame standalone | **On hold** (Alex: File #1 first). ARMSX2 branch `outbreak-vr-frame` has M0 to M3, CI green, with a Monado headless OpenXR test and the sync tool: `STEAM-FRAME.md`. |

**Local builds (2026-10-13)** are the everyday loop; CI is for checkpoints. All in `D:\dev\PenguinScreen2-build`, nothing in
`D:\Games\PS2\PenguinScreen2`:
- **Toolchain, matching CI:** VS 2026 Build Tools 18.10.4 (MSVC 14.51.36231, the same compiler as CI's VS 18.10.2), Windows
  SDK 10.0.26100, CMake 4.4.3, Ninja 1.13.2, 7-Zip 26.03.
- **`run-deps.bat`:** the repo's `build-dependencies.bat`, unmodified, into `deps\` (13 min 38 s clean). It is built without
  FFmpeg (that stage needs GNU make); FFmpeg is only video capture, and dev runs take its DLLs from a CI build.
- **`build.bat <tree> <LTO ON|OFF> [target]`:** CI's configure line. `build-dev` (LTO off) is the iteration tree: clean
  4 min 01 s, a one-file change (`ItemReach.cpp`) 27 s, about 30 s with the compiler setup. `build-release` (LTO on, exactly
  CI) is for performance checks. `vr_selftests` builds there too: 21 of 21 pass locally.
- **`deploy-dev.sh build-dev run-dev`:** a portable install in `run-dev\` with its own settings, memory cards and savestates.
  Its `resources` is a junction to the worktree's `binesources`, so a profile edit applies on an emulator restart.
- **Machine quirks the wrappers handle:** a stray quote in the user PATH (a Tailscale entry) breaks vcvars; Git's GNU tar
  comes before Windows' tar; vswhere ignores Build Tools without `-products *`.

**CI builds** (GitHub Actions):
- **Cached branch:** `gh workflow run build-windows.yml -R beadbeed/PenguinScreen2 --ref wave4-integration`, about 25 min. It
  now includes a "Run VR Self-Tests" step: 19 suites, and a failure stops the upload.
- **New branch:** about 52-70 min.
- **Install:** `tools/outbreak-vr/install_build.ps1 -RunId <id>`. Each install keeps a ~113 MB backup in
  `D:\Games\PS2\PenguinScreen2-backup-*`. There are about 40 now; Alex can delete the old ones.

## What Alex said (two sessions)

Session 1: "extremely impressed", no lag noticed, and walking and shooting felt great. His asks:
- see the gun at the hip;
- a reload;
- no full-screen menus;
- the game's real weapon models;
- depth (the pistol on the counter felt far away);
- a pickup cue;
- smoother menus.

Session 2:
- **Overall:** it still "feels like a flat screen game with VR thrown on top"; he wants deliberate VR.
- **Menus:** he hates the menu pull-away ("we need a hard fix", and changing how the game runs is OK).
- **Close range:** zombies and people vanish up close, so he can be attacked without seeing it.
- **Melee:** the scrub broom is the wrong size, and it is hard to tell when you're attacking.
- **Doors:** a door he opens himself drops out of first person, but following a teammate through doesn't.
- **Unclear:** "The item slots don't really…": the sentence was cut off; ask what he meant.

The plan from all this is in `ROADMAP.md`.

## What the build does (File #1)

**First person:**
- **Head and body:** the game camera follows the head and the body is hidden.
- **Moving:** smooth turn (snap turn optional) with a comfort blink; you walk where you look.
- **Aiming:** with the right controller; walk-and-shoot works.

**The view:**
- **FOV:** `FirstPersonFov = Auto` fits the game camera's FOV to the headset, 125.7° vertical on the Frame.
- **Edges:** soft blurred edge fill instead of black.
- **Depth:** true depth down to 0.5 m.
- **Stereo:** parallax follows head roll, and separation comes from the headset IPD.

**Frame loop:**
- Frames go to SteamVR from a pacer thread at 90 Hz over an opaque background.
- Optional, off by default: a dedicated XR Vulkan queue (`DedicatedXrQueue`).

**Hands and weapons:**
- **In hand:** the right hand holds what Kevin has equipped, as a placeholder shape for now.
- **Laser:** guns only.
- **Shots:** recoil, a muzzle flash and haptics.
- **Wrist card:** the weapon's name and ammo.

**Holster and markers:**
- **Hip:** the equipped weapon hangs at the right hip (pistols, melee) and brightens when your hand is in reach.
- **Shoulder:** a long gun sits over the right shoulder; reach back and squeeze to raise it.
- **Belt and chest:** a faint bag marks the left-hip belt, and faint rings mark the chest pouches.
- **Setting:** `[VR] ZoneMarkers`.

**Reload gesture** (from `reloadwire`, merged on `playtest`): with a gun raised, squeeze the left grip at the left hip to
take a magazine, carry it to the gun and open the hand (or pull the left trigger); the build lets go of R1 (the weapon
lowers), holds L1 until the rounds stop going in, and gives R1 back. Nothing is confirmed in any menu.

**Reload:** lower the gun, stand still, hold the left trigger. That is the game's own reload: File #1 loads a round every
0.33 s while you hold it. A magazine-grab gesture is coming (`ReloadGesture`, wiring in progress).

**Pickup cue:**
- When something can be picked up, the right controller ticks and the item's name shows, e.g. "Handgun (A)".
- It is a copy of the game's own Circle search, which matched the game 25 of 25 times.
- Setting: `[VR] PickupCue`.

**Menus:**
- **Which screens:** any game screen (u32 `0x720AA0`) goes to the flat menu screen. That covers the item screen, map and
  pause, and also the "Will you take it?" prompt and partner hand-overs.
- **The pointer:** it steers the game's own cursor. Only your trigger confirms. It also works on the take-it prompt's
  squares, but a pull on a full square swaps that item out (it drops to the floor).
- **Going:** the pull-away to a flat screen is next to go (`ROADMAP.md` step 1). With `[VR] MenuPanel = true`
  (branch `menupanel`, off by default) it is gone: first person carries on under the screen, the room stays around
  you, and the screen shows on a panel 1.0 m in front of you, where the pointer works as before.

**Grabbed:** shake a controller to break free.

**Game models** (branch `handmodels`, `[VR] GameWeaponModels`, on):
- The game's own weapon meshes and textures are decoded from the disc at VR start, in memory only: 37 weapons in 130-153 ms
  on File #1.
- They are drawn in the right hand, the hip holster and the shoulder sling at their real size, placed where the game
  itself attaches them to the character's hand (its weapon-type table, `feedback.weapon.hold`), and both hands are the
  CC0 hand model (`HandMesh`), all through `MeshRaster`.
- Melee weapons now run through the fist along the controller's handle, as the game holds them (the placeholder pointed
  them along the aim); the scrub brush is its real 1.4 m.

**Online safety:**
- With the network adapter on: no pause, frame advance, state load or save, or speed change.
- First person runs online only in private co-op.
- It writes only the local player's record.

**Logs:**
- A first-person drop-out names the check that failed. In session 2 those were scripted camera shots, door transitions, grabs
  and a cutscene.
- `session_report.py` turns a session's logs into a plain-English summary (`--html` for charts).

## Next headset session: what to check

Play offline, in Single Play, with the network adapter off. Turn both Frame controllers on before launching.

1. **SteamVR stall:** avoid the Steam button mid-play for now. A dashboard visit makes SteamVR stall the game for 10 to 40
   s. If it happens, note when.
2. **A/B the smoothness fixes** once they're in: first `DedicatedXrQueue = true`, then that plus `PacerCopies = true`. Run
   `session_report.py --session` after each.
2b. **Physical melee** (`melee` build, `PhysicalMelee = true`, offline): equip a melee weapon yourself (the iron pipe or the
   scrub brush in J's Bar; the tools never confirm a menu), raise it (bumper or holster grip), and swing at a zombie
   (slot 5). Does a swing attack every time, and never on a small move? Does the hit land where the pipe in your hand
   meets the zombie (and miss when it does not)? Are the tick and the contact buzz clear? Try a lowered swing too (it
   raises the weapon first, 0.6 s). Send the emulog (`(VR) Melee:` lines; `PCSX2_VR_MELEE_LOG=1` for more).
2c. **VR melee on contact** (`vrmelee` build, `PhysicalMelee = true`; contact is the default): equip a melee weapon
   yourself and hit a zombie in slot 5 the way you would hit something: does every real hit register, at any angle, with
   the weapon up or down, and never on a slow push or a small move? Does a hard swing knock it down and a light one only
   make it flinch? Is the buzz at the hit right? `PhysicalMeleeMode = Press` brings back the old swing for comparison.
   Send the emulog (`(VR) Melee: strike ...` and `hit ...` lines).
3. **Hand targeting** (`HandTargeting = true`, slot 7, at the counter): does the wireframe box sit on the pistol, and do you
   get one tick when your right hand enters it?
3b. **Depth layer** (`DepthLayer = true`): lean toward the counter, then again with `PCSX2_VR_DEPTHLAYER_NODEPTH=1` set.
   If near things move against far ones only with depth, SteamVR is using it; if not, the next step is our own per-eye
   warp in the pacer.
3b2. **Depth warp** (`eyewarp` build, `DepthWarp = true`): slot 7, lean 10-20 cm left and right and toward the
   counter. Do the Handgun and the counter now move against the back wall like real objects, with nothing tearing at
   their edges? A/B in turn: `DepthWarp` on; `DepthLayer` on (with and without `PCSX2_VR_DEPTHLAYER_NODEPTH=1`);
   neither. A stretched band of background beside near things is the expected fill. Send the emulog (`(VR) Warp:`
   lines: GPU ms per display frame). First run: if the screen is black or frozen, turn it off and send the emulog.
3c. **Near-cull patch** (`nearcull`): do zombies stay visible up close, even at arm's length, while your own body
   stays hidden when you lean or look straight down?
4. **Seamless doors** (when in): open a door yourself. Do you just arrive in the next room?
5. **Menus without the pull-away** (`menupanel`, `[VR] MenuPanel = true`): open the item screen, the map, pause and a
   take-it prompt. Does the room stay around you with the screen on a panel? Is the panel (1.0 m ahead, 0.80 m tall,
   a little below the eyes) comfortable and readable? Does pointing at your item cells still steer the cursor, and do
   the translucent windows (they show a flat copy of the room behind them) bother you?
5b. **Calm menus** (`panelfeel`, `MenuPanel = true`, `MenuPanelRoom = Freeze`): open the item screen and look around.
   Does the room now stay still behind the panel, like a paused VR game? Is the dim (0.55) right, or should it be darker
   or lighter (`MenuPanelRoomBrightness`)? Is the black past the held room's edges fine when you turn far (the panel
   comes round after 60 deg)? Is the close smooth (the room picks up your head again at once)? Then try
   `MenuPanelRoom = Dim` (the room follows the head, dimmed) and `Live` (as on 10-14), and say which feels best.
6. **Game weapon models** (`handmodels`, `GameWeaponModels = true`, the default there): does the 45 Auto sit in the
   hand as it should (grip in the fist, barrel along the aim, laser from its muzzle)? Do melee weapons through the fist
   along the handle feel right, or should they point along the aim? Is the hand model fine (skin tone, the trigger
   curl)? Does a melee weapon hanging from the belt, or the slung shotgun, look right? Any hitch when the hands move?
   The 5 s `(VR) Hands:` line now has `mesh images N/s mean X max Y ms`.
7. **Reload gesture** (when in): grab at the left hip, bring it to the gun.

**Switches**, in `inis\PenguinScreen2.ini` under `[VR]`: each one undoes one change.

| Setting | Default | The other value |
|---|---|---|
| `HeadsetRateSubmit` | true | false: the old frame path, one frame per GS present |
| `HandsDisplayRate` | true | false: hands, laser and wrist card placed once per GS present |
| `OpaqueBackground` | true | false: SteamVR's environment shows around the screens |
| `FirstPersonFov` | Auto | Profile: the old 100° view (sharper centre, black edges if `EdgeFill` is off) |
| `EdgeFill` | true | false: black around the first-person screen |
| `NearDepthMetres` | 0 | 0 = the profile's 0.5 m; 1.0 = the wave 8 near field; -1 = flat near field; 0.45 to 1.5 = that depth |
| `FirstPersonAutoStereo` | true | false: the profile's fixed stereo numbers |
| `RollParallax` | true | false: parallax not turned with head roll |
| `MenuPointer` | true | false: no beam, dot or point-to-select |
| `ZoneMarkers` | true | false: no holstered weapon or zone markers |
| `GameWeaponModels` | true | false: the procedural glove and placeholder weapon shapes as before. True: both hands are the CC0 hand model and the weapon in the hand, holster and sling is the game's own model, at its real size, placed where the game attaches it to the hand (`feedback.weapon.hold`); a weapon without a model or a known grip keeps the placeholder (logged once). Branch `handmodels`; `research/2026-10-14-weapon-models-in-hand.md` |
| `WeaponSceneLighting` | true | With `GameWeaponModels`: the hands and weapons are lit from the room on screen (the presented frame read back every 6 presents into a 16 x 12 grid; the lower middle gives the brightness and a half-strength tint, the brighter side the key light; eased over 0.5 s; no rim; never brighter than the fixed look). false: the fixed lit look with the rim. Also: a long game-model weapon (scrub brush, poles) now fits its quad from its box edges cut 12 cm in front of the eye, up to 3.2 m (was 1.6 m, and a fallback round the hand when the pole reached past the eye, which cut the far end). Branch `weaponlook`; written, built, self-tests passed (42/42, Hands 112 checks); not run in the emulator or headset yet |
| `PickupCue` | true | false: no pickup tick or name |
| `BeltPouches` | false | true: the chest-rig pouches and the buckle (reach in to name an item, squeeze to open the item screen at that cell) and their rings. Off since Alex's 2026-10-14 session: grab things through the item screen |
| `ReloadGesture` | true | false: no reload gesture (the hip squeeze opens the item screen even with the gun raised; L1 by hand still reloads) |
| `HandTargeting` | false | true: File #1 offline only, a read-only prototype: the right hand against the J's Bar Handgun's trigger volume (a wireframe box, one tick on entering it, `Hand targeting:` lines in the emulog); nothing is written to the game. Its trigger and distances are the File #1 profile's `reach.hand` |
| `DedicatedXrQueue` | false | true: the OpenXR session gets its own Vulkan queue. The emulog says `(VR) XR queue: dedicated` or `shared with the GS (why)`. |
| `PacerCopies` | false | true (with `DedicatedXrQueue` in effect): the VR pacer copies the frame, hands and cards into the headset's swapchains itself, so the GS thread never calls into the runtime or waits for its queue lock; the frame shows about one GS frame later (`PCSX2_VR_PACER_COPIES_FLUSH=1` takes that back with one more GS submit); the emulog says `(VR) Pacer copies: on` or why it stays off |
| `MenuPanel` | false | true: a game screen (the profile's `camera.lookAt.pauseWhen`, File #1's u32 `0x720AA0`: item screen, map, pause, take-it prompts) keeps first person: the camera alone follows the head (no stick turn, aim or body-follow; the sticks and buttons are the menu's as before), the room keeps rendering in stereo, and the screen's own draws go on a flat panel 1.0 m ahead (0.80 m tall, placed where you look as it comes up), where the menu pointer aims. The screens' full-frame tint is left out. While a screen is up the desktop window shows the room without it, and the depth layer sends quads. Needs `PacerCopies` off (else screens pull away as before). The emulog says `(VR) Menu panel: on`, `shown ...` / `hidden`, or why not |
| `MenuPanelRoom` | Freeze | With `MenuPanel`, the room behind a game screen's panel. Freeze: the camera holds the view it had as the screen came up (the head no longer drives it), so the room stays put in the world as you look around; dimmed, greyed a little, black past its edges (no edge fill while it holds); the head drives the camera again as the screen goes. Dim: the camera follows the head, the room dimmed. Live: the camera follows the head, the room as in play (the 10-14 build). The panel itself stays put in the world and comes round, gliding, after a turn of more than 60 deg. The emulog says `(VR) CameraDriver: the room holds still behind the panel ...`, `... the screen is gone; the head drives the camera again (held N s ...)` and `(VR) Menu panel: the room behind it at 55% brightness ...`. No dim with the GS `ShadeBoost` setting on |
| `MenuPanelRoomBrightness` | 0.55 | 0.2 to 1.0: how bright the room is behind the panel under Freeze and Dim (1 = no dim), corrected for the presentation lift's gamma; eased in over 0.25 s and out over 0.2 s |
| `SeamlessDoors` | false | true: File #1, strictly offline (no network adapter, so not private co-op either): a door you open yourself plays no door shot and no arrival shot (camera mode `0x3AEF75` stays 0), so first person holds while your character opens the door, drops only for the black fade and load, and comes back as soon as you are in the next room. The door, locks, keys, door state, the load and partners are unchanged. Profile `camera.silence` entries with `whenSetting: SeamlessDoors` (the arrival ones `keepThroughDisarm`: patched whenever the switch, VR and HeadCamera are on, armed or not, so an arrival shot is skipped after a cutscene too, and `FUN_00550220` is not proven door-only; review m7 / b7); branch `doors`; `research/2026-10-14-doors.md`. A door opened with the switch on, headset-free, 2026-10-15 (`nightpolish`, below) |
| `PhysicalMelee` | false | true: File #1, offline (online only with `PhysicalMeleeOnline`); how it attacks is `PhysicalMeleeMode` (Contact by default; this row is Press): with a melee weapon equipped, a deliberate swing of the right controller (a point 40 cm up the handle faster than 2.2 m/s over 20 cm of a stroke that goes somewhere) presses Circle on your own pad (R1 first if the weapon is down: 0.6 s later), and code caves put the game's own melee hit test (a capsule along a pipe, a sphere on a knife) on the VR weapon, where your hand holds it; the game's damage, stagger and hit reaction follow. The hit window opens about 70 ms after the press (cave C: frame 2, for the local player only; AI partners and other players keep the game's own window and pose). A tick when the swing goes live, a buzz when an enemy's HP drops. Nothing with a screen up, grabbed or hurt. Profile `melee` block and `camera.silence` entries with `whenSetting: PhysicalMelee`; branches `melee`, `meleecoop`; `research/2026-10-14-melee.md`. `PCSX2_VR_MELEE_LOG=1` logs why a swing does or does not attack |
| `PhysicalMeleeOnline` | false | true (with `PhysicalMelee`): also in private co-op (never online-safe, never on obsrv). The swing presses the game's own attack (partners see the game's own swing), and the hit follows the VR weapon only while this console runs the room's enemies (the room's master, which decides their HP and reactions for everyone; NETAQ.BIN's room list, the profile's `melee.online`); otherwise the game's own swing and hit, exactly. Logs `(VR) Melee: online, room N: ...` whenever that changes. Untested online; the test is in `research/2026-10-14-melee-coop.md`; branch `meleecoop` |
| `PhysicalMeleeMode` | Contact | Press: the swing presses the game's own attack as before (the canned swing, its hit on the VR weapon, R1 first when lowered). Contact (with `PhysicalMelee`, wherever the caves may run: offline, or as the room's master with `PhysicalMeleeOnline`; elsewhere the press): nothing is pressed and the character never enters an attack action. The VR weapon's own hit shape (the game's capsule for a pipe, sphere for a knife) touching a zombie's hurt spheres (head, chest, hips) faster than 1.8 m/s at its far end (knives 1.5) makes the game apply the hit through its own code (cave D: the attack spawn; damage, hit effect, flinch, stagger, knockdown, death are the game's). Strength from the speed: below 3 m/s less damage (from 0.6), 3-5.5 the game's own hit, faster 1.5 x damage and the game's reaction class one up (cave E on the game's critical multiplier): a pipe staggers at once, a knife staggers instead of a jolt. One strike per swing per enemy (again once the tip slows below 0.9 m/s or leaves it). A firm buzz at the strike, harder for a harder swing. Logs `(VR) Melee: strike, enemy N ...` and `hit, enemy N HP a -> b, reaction class c ... ms after the strike`. A strike that hits spends the weapon's durability through the game's own item-use path (cave F: the hit notice handled as the swing's own stage does; Kevin's pipe 5 a hit, it bends at the end; knives nothing), as the canned swing's hit does. Online, as the room's master only, partners are sent the start of the game's swing for each strike (0.5 s, a state message only; Alex's character never runs it). Profile `melee.contact` (per-weapon tuning) and `camera.silence` entries with `whenSetting: PhysicalMeleeContact`; branches `vrmelee`, `meleepolish`; `research/2026-10-14-vr-melee.md` (section 7) |
| `MeleeHitJolt` | false | true (a prototype, with `PhysicalMelee` in contact mode, strictly offline): a contact hit that lands also turns the zombie's struck body part the swing's way and eases it back: the head (neck, chest, waist), the belly (waist, chest) or the knees (the legs, the waist), 8 deg at 1.8 m/s up to 24 deg at 6 m/s, back over 0.17-0.33 s, on top of the game's own flinch or stagger. Cave G in the zombie's update applies it through the game's own per-bone rotation (only the bone matrices it rebuilds every frame; never position, HP or AI). Since `nightpolish` it is armed at the strike and starts on the enemy update the hit lands in (the update the HP drops). Logs `(VR) Melee: jolt armed at the strike ...` and `jolt started on the hit's own update`. Since `springjolt` (profile `melee.jolt.spring`): a spring-damper per bone instead of the linear decay: impulse x swing speed, one overshoot, settled in ~0.5 s, clamped (30 deg, arms 40 and loose), hits add up, the arms and one more bone join on hard hits (4 m/s); cave G unchanged, the host rewrites each entry per update (`PCSX2_VR_SPRING_LOG=1` logs it). Profile `melee.jolt` and `camera.silence` entries with `whenSetting: MeleeHitJolt`; branch `hitjolt`; `research/2026-10-14-vr-melee.md` sections 8 and 9 |
| `DepthLayer` | false | true: in first person the frame goes to SteamVR as one projection layer at its render pose with its depth (`XR_KHR_composition_layer_depth`, a D32 depth swapchain per eye), for the runtime's own reprojection; false = the two screen quads, and no depth swapchain or copy at all. Needs `PacerCopies` off. The emulog says `(VR) Depth layer: on` or why it stays off; `PCSX2_VR_DEPTHLAYER_NODEPTH=1` sends the projection layer without the depth (an A/B) |
| `DepthWarp` | false | true: our own translation warp (branch `eyewarp`, `VR/DepthWarp`): in first person the VR pacer redraws each eye's image into its swapchain every display frame from where that eye is now, a 128x128 mesh displaced by the frame's exported depth onto the same render-pose quad, so a lean moves near things against far ones (10 cm sideways: a 1.05 m counter -14 px, a 4 m wall +20 px, the far plane +32 px on the 1536 px image). The GS copies each frame and its depth into a ring of 3 images and the pacer owns the eye swapchains (no new swapchain). Frames without depth (not first person, the first frames, a screen's first and last frame) go unwarped; the room behind a MenuPanel screen keeps its depth (since `nightfix`), logged once per reason; a 5 s `(VR) Warp:` line gives counts and GPU ms. Needs `PacerCopies` off; turns `DepthLayer` off. Test aids `PCSX2_VR_DEPTHWARP_TEST_SHIFT=<m>` (the warp's eyes moved to the right) and `PCSX2_VR_DEPTHWARP_DUMP=<dir>` (one source/warped PPM pair). Checked on the null headset 2026-10-15: the warp runs every display frame at 0.06-0.35 ms GPU for both eyes, the test shift's parallax matches (back room -26 px, counter front +29 px), off is the old path |

## In progress

**Night polish (`nightpolish`, from `nightly`), checked offline headset-free on 2026-10-15** (run-npolish, local build,
38 of 38 self-tests):
- **Hit jolt on the hit's update** (`MeleeHitJolt`; `research/2026-10-14-vr-melee.md` section 9, "Latency, fixed"): the
  jolt is written at the strike with a wait byte, and cave G (54 words) starts it when the enemy's own hit counter
  `+0x13F` is set, the same update the HP drops. Slot 5, the poked pipe, scripted 120 deg swings, 4 hits (head and belly,
  class 1 and 2): in all 4 the struck bone turned 0 deg on the update before and 11-23 deg on the HP-drop update itself,
  and the entry's wait byte went 1 -> 0 on that update (it was 1-2 updates later). The angles are now exact for the
  game's x-then-y-then-z order: the neck's measured axis is within 3-5 deg of the commanded one (about 12 before); the
  waist (bone 10) reads 8 deg off because the game's own flinch jolt turns that bone too.
- **SeamlessDoors, a door opened with the switch on** (`research/2026-10-14-doors.md`, "Live check with first person"):
  slot 4, first person armed, scripted sticks to the men's bathroom door, Circle at it. On: camera mode `0x3AEF75` read
  0 throughout, first person held through the 1.7 s opening motion, disarmed only for the load (status 3), room 1 -> 4,
  re-armed as status went back to 2 (1.5 s sooner than off), and the character walked on in the bathroom. Off: the door
  shot (mode 1) and arrival shot (mode 2) ran as before and first person dropped at the door; all 15 words read the
  game's own value throughout. With the switch on, the 12 door-shot words went back to the game's value at the disarm
  for the load (the 3 arrival words stayed patched, as meant). Not checked: turning the switch off while the game runs
  (no runtime settings over PINE). Holding the stick into that door for 2.5 s did not open it in first person (motion
  136-138, door kind 24, no room change); Circle did.


**Calm menus (`panelfeel`, from `meleecoop`), checked offline headset-free and on the null headset on 2026-10-14**
(`research/2026-10-13-menu-panel.md`, "Calm menus"; road step 1):
- **Freeze:** with the scripted head turning 40 deg under each screen, the game's view matrix turned 0.0 deg under the
  item screen, the map, pause and the take-it prompt. The game kept running (counter 254.6-255.7/s; pause 0/s). As the
  screen closed, the view was at the head by the first 30 ms sample. Dim and Live followed the head as before (40.0 deg).
- **The dim:** the room as shown measured 54.7-54.9% of its own snapshot headset-free; on the null headset 75.2%
  against Live's 136.8% (the profile's gamma 1.2 lift), a ratio of 0.550.
- **Pointer:** it still steers cells 1, 3 and 0 with no confirm, on the null headset with ray hits within 0.001 of the
  targets.
- **Self-tests:** 33 of 33 (new suite "Menu panel room and placement", 49 checks).
- **Not yet checked:** the headset; the panel coming round in a real session (the null head does not turn); the edge
  fill going black while the room is held (reviewed only).

**Physical melee (`melee`, from `handmodels`), checked offline headset-free on 2026-10-14**
(`research/2026-10-14-melee.md`; road step 4):
- **How the game swings:** Circle with the weapon raised starts sub-action 3; the swing's motion (pipe 596, brush and
  spear 602, knife 265) spawns an attack object at one frame and ends it at another; the object copies the weapon
  object's world matrix every frame and the game tests a capsule (pipe: 100 cm along the model's X, r 30 cm) or a sphere
  (knife, r 30 cm) on it against the enemies (`0x3FFCA0`, 0xD90 a slot, s16 HP `+0x544`), stages the damage in `+0x138`
  and `FUN_0060F0C0` applies it. Bullets go the same way (a 10 m ray from the muzzle).
- **The override:** two caves at `0xFE000` (free RAM) and five patched words make the local player's attack object copy
  the VR weapon's pose (written by the host at `0xFF000` each input poll, stamped against the game's frame counter)
  instead. Checked offline over PINE and then in the build: with the volume 3 m off the zombie the same swing misses, on
  it a zombie 2 m out of Kevin's reach is hit; a scripted VR swing presses Circle, the game swings 33 ms later, its attack
  object sits on the VR weapon, the zombie loses 60 + 10 HP and staggers, the contact is logged; switch off: nothing
  patched or pressed.
- **Caveat:** no savestate has a melee weapon equipped and equipping needs a menu confirm, so every live run poked the
  equip (a floor pipe's record, borrowing the held 45's object). Alex has to try a real one.
- **Self-tests:** the detector, the driver, the game matrix, the profile parser and the File #1 entries: 30 of 30 suites.

**VR melee on contact (`vrmelee`, from `meleecoop`), checked offline headset-free on 2026-10-14**
(`research/2026-10-14-vr-melee.md`), after Alex's "sometimes it just hangs for a second":
- **What a swing does now** (`PhysicalMelee = true`, `PhysicalMeleeMode = Contact`, the default): nothing is pressed and
  the character never enters an attack action. The VR weapon's own hit shape (the game's capsule or sphere for that
  weapon) touching a zombie's hurt spheres (head, chest, hips, read from the game each poll) faster than 1.8 m/s at its far
  end (knives 1.5) makes the game apply the hit through its own code (cave D calls the game's attack spawn for one strike,
  just before the frame's hit test); damage, the hit effect, the flinch, stagger, knockdown or death are the game's.
  Strength from the speed, through the game's own critical rule (cave E): slower swings less damage, faster than 5.5 m/s
  1.5 x damage and the reaction class one up (a pipe knocks down instead of staggering). One strike per swing per enemy.
- **Checked offline** (slot 5, poked equip, scripted hand): fast pipe swings 8 of 8 hit (7.1-7.6 m/s, knockdown), raised
  4 of 4 (Kevin stayed in aim idle), medium swings 5 of 5 (stagger), knife 6 of 6 (flinch); slow sweeps, pushes and
  waggles on the zombie 0 strikes in 30 runs; strike to HP drop 30-63 ms. `Press` still works as before.
- **Not done:** a real equip, a headset, online, ragdoll (research only, in the note).
- **Self-tests:** 35 of 35 (contact logic, the profile entries, caves D and E interpreted).

**Melee polish (`meleepolish`, from `nightly`), checked offline headset-free on 2026-10-15**
(`research/2026-10-14-vr-melee.md` section 7):
- **Durability:** a contact strike that hits now spends the weapon the way the game's own hit does. The hit leaves a
  one-frame notice in the attacker's record that only the canned swing's stage acted on; cave F (after the frame's hit
  test) handles it the same way, so the game's next character loop spends through its own item-use code. Checked
  offline (poked equip): contact pipe 40 -> 35 per hit (3 of 3), the canned swing in Press mode 40 -> 35, same two
  writers; at count 1 both bend the pipe (1 -> 20, the curved pipe); the knife spends nothing in either.
- **What partners see (online, as the room's master only):** for each strike cave F sends partners one state message
  with the swing's action bytes (stance 2, sub 3), put back at once, and 0.5 s later the swing's end as the game sends
  it; Alex's own character never enters the swing (no hang). Offline nothing is sent or shown. Checked offline only
  with the test switch `PCSX2_VR_TEST_PARTNER_SWING=1` (the cave's two writes and the put-back, Kevin stayed 1/0) and on
  the interpreter (the message's bytes); what partners see needs the two-player test (co-op note, steps 2b, 2c).
- **Self-tests:** 36 of 36 (cave F interpreted; the profile entries; the partner-swing switch rule).

**Physical melee in co-op (`meleecoop`, from `melee`), traced statically and checked offline on 2026-10-14**
(`research/2026-10-14-melee-coop.md`):
- **How a hit reaches partners (static, NETAQ.BIN read from the disc):** the attacker sends state (a type-1 message as
  the swing starts), every console swings every player's copy and tests it against its own enemies, and the console that
  runs the room's enemies (the first in NETAQ's room list) alone kills and sends HP and reactions; elsewhere a killing
  hit leaves HP at 1 until the master's message comes. Not lockstep, not hit messages.
- **Local only:** the earlier hit window is now cave C (`0xFE100`) for the local record only; the shared spawn frames
  stay stock. Checked offline: with the caves armed but the block naming another record, Kevin's swing spawned at the
  game's own frame and pose and hit as with the switch off; naming Kevin, 63 ms after the swing began, on the block's
  pose; the build's own scripted swing still hits (600 -> 540 -> 530).
- **Online:** `PhysicalMeleeOnline` (off): in private co-op the presses always, the caves only while this console is
  the room's master. Untested online; the two-player test is in the note.
- **Self-tests:** 32 of 32 suites (the mode table and room master read, the caves' words on an interpreter).

**Game weapon models in the hand (`handmodels`, from `playtest`), checked offline headset-free on 2026-10-14**
(`research/2026-10-14-weapon-models-in-hand.md`):
- **How the game attaches a weapon:** the local record's u16 `+0xCA8` is the weapon type, an entry of the 0x44-byte
  table at `0x6E5944` holds the model's origin in the right hand bone's frame and its turn. Twelve savestates (lowered)
  and a live raised 45 Auto (23 samples) match it to 0.05 cm; the spray's turn fixes the Euler order.
- **Placement:** the bone frame on the hand model's wrist; a gun's barrel onto the aim (0 deg for the pistols, up to
  31.6 deg for the grenade launchers). The 45's origin lands 2 cm from the placeholder's trigger.
- **Checked:** the 21 weapons of J's Bar's item table drawn in the hand, from the side, in the holster or sling (image
  dump, kept on the PC); the equipped 45 read as type 7 in slots 6 and 9; the hands self-test (98 checks) and a profile check of
  `feedback.weapon.hold`: 26 of 26 suites.
- **Cost:** a hand and real pistol image 0.5-0.7 ms (2 x 2 samples), a long gun 0.9-1.0 ms, against the placeholder's
  0.14 ms; drawn only when an image has to be redrawn, on the GS thread, never the pacer.
- **Not checked:** the headset and any XR session (null headset off limits); other weapons equipped live (a menu
  confirm); the left hand's fore-end on the long-gun models is still the placeholder's point.

**Hand targeting (`handtarget`), checked offline headset-free on 2026-10-13** with `handtest.py` on a local build (slot 7):
- the target is the J's Bar counter pistol: node `0x3B6158`, item 0, volume centre (8493.5, 140, 12922.5) cm, from
  `reach.hand`; the game's own forward search agrees;
- mapping: the eye check is 0.000 mm and the round trip 0.008 mm; a hand placed at the reported centre maps back to within
  0.1 cm of it and is inside; 45 cm to the side maps 45 cm away in the game (outside, still the target);
- the projection check agrees to 0.00 deg, but with a still scripted head it proves only that the game's view matrix and
  the map agree, not the timing;
- the hand gone: the target is lost and the reason logged;
- **not checked yet:** the marker and the tick (they need an XR session), the menu and disarm paths live (reviewed only),
  and whether the box sits on the pistol in the headset. That last one needs Alex in the headset.

**Near-cull patch (`nearcull`), checked offline headset-free on 2026-10-13** with `nearcull_test.py` on the dev install
(`research/2026-10-13-near-cull.md`):
- **What changed:** File #1 profile only, four `camera.silence` words, all draw-only and put back on disarm:
  - the game's near-camera cull (`0x590304`) never culls;
  - the camera's focus character, which is you, skips its body draw, its first draw pass and what it holds
    (`0x3452E0`, `0x3450F8`, `0x560C74`).
- **Gone:** the old focus-slot holds and the 75 radius. One hold remains, a repair for savestates made under the old
  focus hold (slot 3).
- **No switch:** it is part of first person.
- **Checked, slot 5** (a zombie walks up):
  - before, the zombie was not drawn in 356 of 514 samples where it was nearer than the cull radius;
  - after, it was drawn in all 460, down to 35 cm across the floor;
  - the body was not drawn, the partners were;
  - all four words came back when the grab released first person.
- **Checked, slot 6 and slot 3:** looking down shows no body, as before. Slot 3's body is hidden only thanks to the
  repair.
- **Also checked:** `vrtest.py --check-profile` and the self-tests (21 of 21) pass.
- **Not checked:** co-op (reviewed: nothing in a character record is written), rooms whose camera shots hide
  characters, and the headset.
- **Found on the way:** offline, the local slot byte `0x48BF7D` reads 1 or 2 for a moment every frame while the game
  runs the AI partners. Anything reading it at vsync, `camera.base` included, can catch a partner's slot.

**Depth layer (`depthproj`), checked offline headset-free and on the null headset on 2026-10-13**
(`research/2026-10-13-depth-export.md`):
- **Where the depth is:** the GS depth target `0x1C00` (D32 z/2^32, both eyes' layers). File #1 draws the next frame
  into the other buffer, clearing that depth, before the shown one is presented, so the depth is snapshotted when that
  clear starts; at the hand-off the live target already holds the next frame.
- **Metres:** `1 / (100 * beta * (z - a0))` from the Z-driven depth fit; unit-tested (33 checks, `vr_selftests` 22 of 22).
- **Cross-check, slot 7:** the counter top under the Handgun's trigger volume reads 1.045 m ahead (expected 1.040 m);
  the counter is flat at 35.1 cm below the eye, about 119.8 cm in the game, where the volume's floor is 120 cm.
- **Same frame:** with a turning head the snapshot lines up with the shown colour to 0 px; the live target is 2 to 14 px off.
- **Null headset:** SteamVR offers the extension and takes the layer with no error; the picture matches the quads to
  0 px. Restored to the real settings afterwards (byte-identical).
- **Not checked:** whether SteamVR's reprojection uses the depth (leaning), and the headset.

**Menu panel (`menupanel`), checked offline headset-free and on the null headset on 2026-10-13**
(`research/2026-10-13-menu-panel.md`, `menupanel_test.py`):
- **The room under a screen:** the item screen, the map, the take-it prompt (bathroom Green Herb, slot 8) and pause all
  draw the room every frame first and the screen over it in the same buffer. With first person kept, the game's own
  view matrix turned 40.0 deg with a 40 deg head turn under each of them; with the switch off the camera held still,
  as before.
- **Running:** the game counter ran at 255/s under the item screen, map and take-it prompt, as with no screen; pause
  stops it (0/s) but the room is still drawn and still follows the head.
- **The split:** the colour target is snapshotted just before the screen's first draw (its depth clear or its backdrop
  after the room); the first-person screen shows the snapshot, and what the screen's draws changed goes on the panel.
  Panel share of the picture: item screen 43.9%, map 27.0%, take-it 38.3%, pause 51.4%. The room images carry no
  screen pixels. The screens' full-frame tint is left out (with it the panel covered 95%).
- **Null headset:** the panel layer 1.0 m ahead, 1.07 x 0.80 m, published as the pointer's screen; the ray hit cells 1,
  3 and 0 within 0.001 of their centres and the cursor got there in 0.16-0.25 s, 1-2 D-pad presses, no confirm. No
  pull-away, 60 frames/s (the null headset's), no xrEndFrame error. SteamVR restored afterwards (byte-identical).
- **Self-tests:** 23 of 23 (the new Menu panel suite, 43 checks).
- **Not checked:** the headset; partner hand-overs (same overlay, not seen: starting one needs a menu confirm);
  co-op. Translucent windows show opaque with a flat copy of the room behind them; the panel's place is fixed.

**The 2026-10-12 research agents (menus without the pull-away, seamless doors, true per-eye rendering) stopped at the
usage limit and wrote no notes.** Those three are still open; the ROADMAP has the plan.

**Merged since session 2:** `pacer2` (frame stats, a steady layer array with parked layers, a projection layer always
sent, opt-in `PacerCopies`; targets the SteamVR stalls, `research/2026-10-12-steamvr-endframe-research.md`); the menu
re-press; the VR self-tests in CI.

**Builds on branches:**
- **`reloadwire`:** the reload gesture drives the native L1 reload.
- **`meshraster`:** the textured mesh renderer, so the game's weapon models and the hand model can be drawn.

**Next, once those land:**
- the game models in the hand and holster;
- physical melee (swing to attack, contact buzz);
- a scripted-camera-shot review: which of them really need to drop out of first person.

## Needs more investigation

- **The SteamVR stall.**
  - **Seen:** `xrEndFrame` held one display period for 10 to 40 s at a time. It began at a dashboard or Steam-button focus
    change, and once 5 s after launch. The game fell to 39-50 fps then.
  - **Likely cause:** SteamVR's timing mode after a focus change.
  - **Fixes:** the queue and `pacer2`.
- **"The item slots don't really…":** ask Alex what he meant.
- **Scripted camera shots:** the room camera mode `0x3AEF75` reads 1 during them, and they drop out of first person. Decide
  which can stay first person.
- **Menu pointer thresholds** are estimates for real hands: dwell 150 ms, rest 100 ms, pull start 0.1.
- **Test harness flakiness on this PC:**
  - `menutest`: multi-press steers occasionally stalled before the re-press fix;
  - fast-click: its "held" flag is too short to see over PINE;
  - `vrtest smooth-turn` comes up short.
  - The old build showed the same, so these are not regressions.
- **The map's ~0.64 s open delay** is the game's own disc read. The "Fast CDVD" emulator option might shorten it; that is
  Alex's setting to try.
- **Game models:** the ids with no mesh on File #1 (2, 5-7, 12, 13, 15-18, 24, 26, 30, 107, 109; they keep the placeholder);
  what the weapon-type table's second grip and turn (variant B) are for; the long guns' fore-end point for the left hand.

## Important to know

**Rules (Alex's, still in force):**
- Never run the game faster than normal speed (the obsrv server bans it).
- Never pause or freeze the emulator while online (the server drops you after about 30 s).
- Never load or save a state online.
- First person may write game memory and patch code online, but never another player's character.
- Online only when Alex says so, and only in private co-op with his brother on the Mac's private Bioserver. On obsrv, first
  person stays off. No logins, terms or publishing without him.
- Never upload or share discs, BIOS, memory cards, savestates, dumps or anything extracted from the disc. This repo holds
  code only.
- Ask before changing SteamVR configuration. The null-headset test is allowed and always restored.
- Never confirm a menu entry automatically. The only exception: a deliberate reload gesture may drive the reload, and it uses
  the native L1 reload, so no menu is involved.
- Ask before changing Alex's emulator graphics settings: internal resolution, Fast CDVD and so on.
- Commits use the beadbeed noreply address.

**Test saves on the PC** (File #1, in Alex's savestate folder, never committed):
- slot 3: pitch test;
- slot 4: J's Bar, the usual spot;
- slot 5: a zombie grabs Kevin 5-7 s after load;
- slot 6: calm, gun equipped;
- slot 7: at the counter, facing the pistol;
- slot 8: the men's bathroom (Green Herb prompt);
- slot 9: the bathroom with a .45, a Handgun, 9mm and .45 rounds, one free cell.

**Headset-free test kit** (everything refuses to run with the network adapter on):
- **Self-tests:** `PCSX2_VR_SELFTEST=1` and friends at launch. They also run in CI now (`tests/vr/`). To run them locally:
  `cmake --build build --target vr_selftests`, then `build/tests/vr/vr_selftests bin/resources`.
- **`vrtest.py`:** first-person scenarios. `--check-profile` needs no emulator.
- **`menutest.py`:** the menu pointer and pouches; `--test-uv` works with no runtime.
- **`grabtest.py`:** shake to break free.
- **`session_report.py`:** a session's logs as a summary.
- **`gamemodels_check.py`:** decodes the disc's weapon models read-only and prints the same summary as the emulator.
- **`PCSX2_VR_DUMP_HANDS=<folder>`:** once in game, draws every weapon of the scenario as held, holstered and from afar
  into PNGs there (the game's models: keep them on the PC).
- **`handtest.py <dev install>`:** hand targeting from slot 7 (above).
- **`nearcull_test.py <dev install> <before|after> <slot> <seconds> [zombie|down]`:** the near-cull patch against
  the committed profile; slot 5 faces the approaching zombie, slot 6 looks down at the body (above).
- **`menupanel_test.py <dev install> <out dir> [items] [map] [takeit] [pause] [--xr]`:** `[VR] MenuPanel`: per
  screen, whether first person keeps the camera (the game's view matrix against a 40 deg head turn), the game
  counter, the panel's split (dumps of the panel, the room and the frame), and on the item screen the pointer steering
  cells 1, 3 and 0 (PINE test hits, or with `--xr` on the null headset a ray at the published screen). Slots 6 and 8.
- **Scripted controllers:** `PCSX2_VR_TEST_INPUT=1` plus `pine.py`'s `FakeController`. PINE serves one client at a time. While
  scripted input is live, the pickup cue and hand targeting run without an OpenXR session too (rumble stays session-only).
- **No OpenXR runtime:** set `XR_RUNTIME_JSON` to a missing file, plus `PCSX2_VR_FAKE_HEADPOSE`. The scratchpad helper
  `wave4_tests.launch()` does it.

**Game speed:** the OSD, or the game counter u8 `0x48BF93` (212.4/s = 100%).
