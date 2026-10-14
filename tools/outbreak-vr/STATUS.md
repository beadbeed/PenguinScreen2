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
`D:\Games\PS2\Launch-Playtest-SteamVR.cmd`. It runs `D:\dev\PenguinScreen2-buildun-playtest`, a separate portable
install with its own settings (`HandTargeting`, `MenuPanel` and `ReloadGesture` on, `DepthLayer` off), memory cards and copies
of the File #1 savestates. Headset-free speed in slot 5 matches the CI build.

## Where things are

| | |
|---|---|
| Working branch | `headset1` (all of tonight's work, merged and pushed). `pc-firstperson` and `coop-firstperson` are still at the 10-12 morning build and get `headset1` once Alex is happy with it. CI builds of `headset1` go through `wave4-integration`, the branch with the CI cache. |
| Installed on the PC | `D:\Games\PS2\PenguinScreen2`, CI build of `3eb2524` (the second headset session ran on it). `headset1` has more since: the menu re-press and the CI self-tests. |
| Headset sessions | **1:** 2026-10-12 ~18:45, build `e6dbd77`. **2:** ~22:07, build `3eb2524`. Both were File #1. Logs: `D:\Games\PS2\sessions\<time>` and `logs\emulog-headset-20261012.txt`. Summary: `tools\outbreak-vr\session_report.py --session`. |
| Hand targeting | Branch `handtarget` (from `headset1` 43de18c): the read-only prototype, `reach.hand` in the profile, the pickup cue and zone marker self-tests in CI (21 suites, run 38488998201 green at 8dbff61). Checked offline, headset-free, 2026-10-13 (below). Not in the headset yet. |
| Near cull | Branch `nearcull` (from `handtarget`): zombies and partners stay drawn up close, your body stays hidden. A File #1 profile change only; checked offline, headset-free, 2026-10-13 (below). Not in the headset yet. |
| Depth layer | Branch `depthproj` (from `nearcull`): the shown frame's depth exported beside its colour, in metres, and (`[VR] DepthLayer`, off) sent to SteamVR with the first-person frame as a projection layer with depth. Road step 5's foundation. Checked offline and on the null headset, 2026-10-13 (below). Not in the headset yet. |
| Menu panel | Branch `menupanel` (from `depthproj`): with `[VR] MenuPanel` (off) a game screen keeps first person, the room keeps rendering around you, and the screen's own draws are split off the frame onto a flat panel in front of you, where the menu pointer aims. Road step 1, stage 1. Item screen, map, take-it prompt and pause checked offline headset-free and on the null headset, 2026-10-13 (below). Not in the headset yet. |
| Dev installs (local builds, portable, own settings) | `D:\dev\PenguinScreen2-buildun-dev`: `nearcull` with `HandTargeting = true`. `run-depth`: `depthproj`. `run-menu`: `menupanel` (set `MenuPanel` / `DepthLayer` in its `inis\PenguinScreen2.ini`). `D:\Games\PS2\PenguinScreen2` is unchanged (CI build `3eb2524`). |
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

**Game models:**
- The game's own weapon meshes and textures are decoded from the disc at VR start, in memory only: 37 weapons in 153 ms on
  File #1.
- They are not drawn yet; the mesh renderer is in progress.
- The CC0 hand model is converted (`HandMesh`) but not drawn yet either.

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
3. **Hand targeting** (`HandTargeting = true`, slot 7, at the counter): does the wireframe box sit on the pistol, and do you
   get one tick when your right hand enters it?
3b. **Depth layer** (`DepthLayer = true`): lean toward the counter, then again with `PCSX2_VR_DEPTHLAYER_NODEPTH=1` set.
   If near things move against far ones only with depth, SteamVR is using it; if not, the next step is our own per-eye
   warp in the pacer.
3c. **Near-cull patch** (`nearcull`): do zombies stay visible up close, even at arm's length, while your own body
   stays hidden when you lean or look straight down?
4. **Seamless doors** (when in): open a door yourself. Do you just arrive in the next room?
5. **Menus without the pull-away** (`menupanel`, `[VR] MenuPanel = true`): open the item screen, the map, pause and a
   take-it prompt. Does the room stay around you with the screen on a panel? Is the panel (1.0 m ahead, 0.80 m tall,
   a little below the eyes) comfortable and readable? Does pointing at your item cells still steer the cursor, and do
   the translucent windows (they show a flat copy of the room behind them) bother you?
6. **Game weapon models** (when in): do the size, the grip and the holster look right?
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
| `PickupCue` | true | false: no pickup tick or name |
| `ReloadGesture` | true | false: no reload gesture (the hip squeeze opens the item screen even with the gun raised; L1 by hand still reloads) |
| `HandTargeting` | false | true: File #1 offline only, a read-only prototype: the right hand against the J's Bar Handgun's trigger volume (a wireframe box, one tick on entering it, `Hand targeting:` lines in the emulog); nothing is written to the game. Its trigger and distances are the File #1 profile's `reach.hand` |
| `DedicatedXrQueue` | false | true: the OpenXR session gets its own Vulkan queue. The emulog says `(VR) XR queue: dedicated` or `shared with the GS (why)`. |
| `PacerCopies` | false | true (with `DedicatedXrQueue` in effect): the VR pacer copies the frame, hands and cards into the headset's swapchains itself, so the GS thread never calls into the runtime or waits for its queue lock; the frame shows about one GS frame later (`PCSX2_VR_PACER_COPIES_FLUSH=1` takes that back with one more GS submit); the emulog says `(VR) Pacer copies: on` or why it stays off |
| `MenuPanel` | false | true: a game screen (the profile's `camera.lookAt.pauseWhen`, File #1's u32 `0x720AA0`: item screen, map, pause, take-it prompts) keeps first person: the camera alone follows the head (no stick turn, aim or body-follow; the sticks and buttons are the menu's as before), the room keeps rendering in stereo, and the screen's own draws go on a flat panel 1.0 m ahead (0.80 m tall, placed where you look as it comes up), where the menu pointer aims. The screens' full-frame tint is left out. While a screen is up the desktop window shows the room without it, and the depth layer sends quads. Needs `PacerCopies` off (else screens pull away as before). The emulog says `(VR) Menu panel: on`, `shown ...` / `hidden`, or why not |
| `DepthLayer` | false | true: in first person the frame goes to SteamVR as one projection layer at its render pose with its depth (`XR_KHR_composition_layer_depth`, a D32 depth swapchain per eye), for the runtime's own reprojection; false = the two screen quads, and no depth swapchain or copy at all. Needs `PacerCopies` off. The emulog says `(VR) Depth layer: on` or why it stays off; `PCSX2_VR_DEPTHLAYER_NODEPTH=1` sends the projection layer without the depth (an A/B) |

## In progress

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
- **Game models:** mirroring against our axes; the ids with no mesh (6, 16-18, 26); the grip and muzzle points per weapon.

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
