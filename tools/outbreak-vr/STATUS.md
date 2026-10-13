# Outbreak VR: status, what's next, what needs investigation

Start here. This page sums up first-person VR Resident Evil Outbreak in PenguinScreen2. **Scope is File #1 (SLPM-65428)
only** until it feels right. The direction, and the plan to make it feel like real VR, is in `ROADMAP.md`.

Details live elsewhere:
- `ROADMAP.md`: Alex's goal, why it still feels like a flat game in a headset, and the road to real VR;
- `HANDOFF.md`: the history and Alex's decisions;
- `README.md`: every tool and debug switch;
- `bin/resources/vr-profiles/README.md`: the profile format;
- `research/`: game memory facts and design research;
- `STEAM-FRAME.md`: the Steam Frame standalone port (on hold).

Updated by the Windows PC session on 2026-10-12 at night, after Alex's first two headset sessions.

## Where things are

| | |
|---|---|
| Working branch | `headset1` (all of tonight's work, merged and pushed). `pc-firstperson` and `coop-firstperson` are still at the 10-12 morning build and get `headset1` once Alex is happy with it. CI builds of `headset1` go through `wave4-integration`, the branch with the CI cache. |
| Installed on the PC | `D:\Games\PS2\PenguinScreen2`, CI build of `3eb2524` (the second headset session ran on it). `headset1` has more since: the menu re-press and the CI self-tests. |
| Headset sessions | **1:** 2026-10-12 ~18:45, build `e6dbd77`. **2:** ~22:07, build `3eb2524`. Both were File #1. Logs: `D:\Games\PS2\sessions\<time>` and `logs\emulog-headset-20261012.txt`. Summary: `tools\outbreak-vr\session_report.py --session`. |
| Steam Frame standalone | **On hold** (Alex: File #1 first). ARMSX2 branch `outbreak-vr-frame` has M0 to M3, CI green, with a Monado headless OpenXR test and the sync tool: `STEAM-FRAME.md`. |

**Builds** come from GitHub Actions only:
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
- **Going:** the pull-away to a flat screen is next to go (`ROADMAP.md` step 1).

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
3. **Near-cull patch** (when in): do zombies stay visible up close while your own body stays hidden?
4. **Seamless doors** (when in): open a door yourself. Do you just arrive in the next room?
5. **Menus without the pull-away** (when in): does the room stay around you with the menu on a panel?
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
| `DedicatedXrQueue` | false | true: the OpenXR session gets its own Vulkan queue. The emulog says `(VR) XR queue: dedicated` or `shared with the GS (why)`. |

## In progress (agents, 2026-10-12 night)

**Game changes (Opus):**
- **Menus without the pull-away:** design. `research/2026-10-12-menus-in-vr-design.md`.
- **Near-cull patch:** live tests on the grab save. `research/2026-10-12-near-cull-research.md`.
- **Seamless doors:** `research/2026-10-12-seamless-doors-research.md`.
- **True per-eye rendering:** depth reprojection, and replaying the game's VU1 draw program per eye.
  `research/2026-10-12-true-vr-rendering-research.md`.

**Builds on branches:**
- **`pacer2`:** frame stats, a steady layer array, an always-present projection layer, and opt-in pacer-owned copies
  (`PacerCopies`). This targets the SteamVR stalls: `research/2026-10-12-steamvr-endframe-research.md`.
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
- **Scripted controllers:** `PCSX2_VR_TEST_INPUT=1` plus `pine.py`'s `FakeController`. PINE serves one client at a time.
- **No OpenXR runtime:** set `XR_RUNTIME_JSON` to a missing file, plus `PCSX2_VR_FAKE_HEADPOSE`. The scratchpad helper
  `wave4_tests.launch()` does it.

**Game speed:** the OSD, or the game counter u8 `0x48BF93` (212.4/s = 100%).
