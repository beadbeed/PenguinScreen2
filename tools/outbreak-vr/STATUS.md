# Outbreak VR: status, what's next, what needs investigation

Start here. This page sums up first-person VR Resident Evil Outbreak (File #1 SLPM-65428, File #2 SLPM-65692, English
patch) in PenguinScreen2 on one page. It is for Alex, the Mac session and the next PC session.

Details live elsewhere:
- `HANDOFF.md`: the history, Alex's decisions and the co-op plan;
- `README.md`: every tool and debug switch;
- `bin/resources/vr-profiles/README.md`: the profile format;
- `research/`: game memory facts;
- `STEAM-FRAME.md`: running it standalone on the Frame.

Written by the Windows PC session on 2026-10-12, at the end of a long autonomous run (waves 4 to 8, 2026-10-11 and 12).

## Where things are

| | |
|---|---|
| Branches | `pc-firstperson` = `coop-firstperson` = `wave4-integration` (the branch with the CI cache) = the latest docs commit (code as of `e6dbd77`) |
| Installed on the PC | `D:\Games\PS2\PenguinScreen2`, CI build of `e6dbd77` (run 38230328003): `0a9fa63` plus the frame-advance online lock |
| Tested in the real headset | **Nothing since the 10 Oct morning session.** That is about 18,800 lines across 58 files (`63cc827..HEAD` in `pcsx2`, `pcsx2-qt`, `bin`), all checked headset-free only. |
| Steam Frame standalone | Plan in `STEAM-FRAME.md`. **M0 to M3's code are done** on ARMSX2 branch `outbreak-vr-frame` (`bd7198a`, CI green); nothing has run on a Frame yet (see below). |

**Builds** come from GitHub Actions only:
- start one with `gh workflow run build-windows.yml -R beadbeed/PenguinScreen2 --ref <branch>`: about 20 min on a branch with
  the CI cache, about 52 min on a new one;
- install with `tools/outbreak-vr/install_build.ps1 -RunId <id>`.

Each install keeps the previous program files in `D:\Games\PS2\PenguinScreen2-backup-<time>`. Each backup is about 113 MB,
and there are 37 of them (about 4 GB); Alex can delete the old ones.

## What the build does

First person in both Files:
- **The view:** the game camera follows the head, the body is hidden, smooth turning (snap optional) with a comfort blink,
  you walk where you look, and you aim with the right controller (point-to-aim, aim pitch).
- **Frame loop:**
  - frames go to SteamVR from a pacer thread at the headset's rate, over an opaque black background (no SteamVR Home);
  - the GS never waits on the runtime;
  - losing the session falls back to flat play.
- **Hands:** they hold what Kevin has equipped (pistol, long gun, melee, thrown, spray). The laser shows for guns only. Shots
  give recoil, a muzzle flash and haptics. The wrist card shows the weapon's name and ammo.
- **Menus:**
  - a beam and dot from the right hand;
  - point-to-select on the item screen (4 cells and the equipped box) steers the game's own cursor;
  - only the trigger confirms; the pointer never confirms by itself;
  - a command submenu stops the pointer.
- **Belt pouches:** a 2x2 chest rig mirrors the item grid, and the belt buckle is the equipped box. Reaching in names the item
  and ticks; a squeeze opens the item screen at that cell.
- **Grabbed:** shaking a controller breaks free. Hurt and heartbeat haptics keep working.
- **Stereo:**
  - true depth down to 1.0 m;
  - parallax follows head roll;
  - separation is derived from the headset IPD;
  - menus are flat.
- **Online safety:**
  - with the network adapter on, pause, state loads and saves, and speed changes are refused;
  - first person arms online only in private co-op (the Internal DNS sends the login host to a LAN, Tailscale or loopback
    address);
  - it writes only the local player's record (identity byte `+0x03`).
- **New and off by default** until Alex has tried the build:
  - `FirstPersonFov = Auto` sizes the game camera's FOV to the headset, so the room reaches the edge of the view (at the
    cost of a softer centre);
  - `EdgeFill = true` replaces the black around the first-person screen with a dim blur of its edges.

The last wave (8) fixed the 8 findings of a pre-headset review:
- **Menu pointer:**
  - it no longer undoes the player's own D-pad moves;
  - a trigger pull confirms where the dot rested, not where it dipped;
  - only the right hand points;
  - a trigger held through a tracking gap is not re-pressed.
- **Frame loop:** the GS deferral uses the pacer heartbeat, and the eye pair is published atomically.
- **Private co-op detection:** stricter.
- **Text:** the toast sits nearer than the near field and under the hands.

## First headset session: what to check

Play offline: Single Play, network adapter off. Turn both Frame controllers on in SteamVR before launching. Graphics settings
stay as Alex has them.

1. First person arms. In the emulog, check the binding report: which interaction profile each hand got.
2. Turn the head slowly, then fast. Is there judder? Is SteamVR Home gone? Read the `(VR) Frames:` lines (submitted per
   second, new GS frames, xrEndFrame mean and max).
3. Is the game at full speed? The OSD should read about 100%.
4. Rate depth, brightness and comfort from 1 to 5. Do near objects look right at arm's length?
5. Smooth turn, then snap turn. Walk where you look. Go through a door and a camera cut; first person must come back.
6. Is the body hidden? Recenter with both stick clicks and with the Steam button.
7. **Menus:**
   - point at item cells and pull to select;
   - push the D-pad yourself while pointing: the pointer must not fight you;
   - a quick point-and-pull confirms the cell you rested on;
   - open an item's submenu: the pointer must stop.
8. **Pouches:** reach to the chest; the item name shows. Squeeze: the item screen opens at that cell.
9. **Weapon:** raise it. Do the hand and laser look right? Does the shot land where the laser points? Does the wrist card name
   the weapon?
10. Get grabbed and shake to break free. Take a hit and feel the haptics.
11. Take the headset off for 30 s, then put it back. The game must keep running.
12. Optional, after the above: set `[VR] FirstPersonFov = Auto` and `EdgeFill = true` and compare.

Save the emulog (`D:\Games\PS2\PenguinScreen2\logs\emulog.txt`, or the `emulog-<time>.txt` copies), SteamVR's
`vrserver.txt` and the session recorder folder (`D:\Games\PS2\sessions\<time>`).

**If something is badly wrong**, these switches in `inis\PenguinScreen2.ini`, section `[VR]`, undo the big changes one at a
time:

| Setting | Default | The other value means |
|---|---|---|
| `HeadsetRateSubmit` | true | false: the old frame path, one frame per GS present (ran the game at about 50% speed on the null headset) |
| `HandsDisplayRate` | true | false: hands, laser and wrist card placed once per GS present |
| `OpaqueBackground` | true | false: no black layer (SteamVR's environment shows around the screens) |
| `FirstPersonAutoStereo` | true | false: the profile's fixed stereo numbers instead of ones from the headset IPD |
| `RollParallax` | true | false: parallax not turned with head roll |
| `NearDepthMetres` | 0 | 0 = the profile's 1.0 m; negative = flat near field (the old look); 0.45 to 1.5 = that depth |
| `MenuPointer` | true | false: no beam, dot or point-to-select |
| `FirstPersonFov` | Profile | Auto: FOV fitted to the headset |
| `EdgeFill` | false | true: blurred edges instead of black |

## What's next (in order)

1. **Alex's first headset session with this build**, using the checklist above. Everything below waits on what it shows.
2. **Fix what that session finds**, starting from the emulog. If the pacer misbehaves, compare `HeadsetRateSubmit = false`.
3. **Decide about the full view:** if Alex likes `FirstPersonFov = Auto` and `EdgeFill`, make them the default. Then ask
   about a sharper image (internal resolution) or 120 Hz; his settings are not touched until he says so.
4. **Steam Frame standalone:** Alex runs `frame-xrprobe` once in the headset (`STEAM-FRAME.md`, ARMSX2 `OUTBREAK-VR.md`).
   Then continue M0: copy the VR layer, port the hooks, and get the game on a flat screen on the Frame.
5. **Co-op proof with Adrian** on the Mac's private Bioserver: a two-machine session, only when Alex says so; only he logs
   in.
6. **Smaller features still open:**
   - finger curl on the hand models;
   - aim assist (optional);
   - reconnecting after a SteamVR restart;
   - physical reload (a gesture);
   - a dedicated XR VkQueue (it removes the shared-queue lock, if the GPU has a second queue);
   - research item B1 (`ndcWidthRatio`; needs a null-headset stereo dump).

**Port back to PenguinScreen2** (found by the ARMSX2 port; listed in ARMSX2's `pcsx2/VR/VR-SOURCE.md`):
- the frame-advance online lock: **done**, in `e6dbd77`;
- the pose-to-photon log line in `XRCompositor.cpp`;
- `CanPause` refusing online only while the VM runs, so a paused VM can be resumed: **done**, in `portback`;
- the Qt pause lock asking `VMManager::IsOnlineLocked()` (so `PCSX2_TEST_ONLINE_LOCKS=1` covers the buttons): **done**, in `portback`;
- PINE's state save and load refusals using `IsOnlineLocked()`: **done**, in `portback`;
- the VR settings page's Edge Fill "recommended" value: it should be Unchecked now that `EdgeFill` defaults to false;
- the M3 layer budget (`maxLayerCount`) and the flat-quad fallback lines in `XRSession.cpp` and `XRCompositor.cpp`;
- `pine.py`'s Linux socket transport (it changes nothing on Windows): **done**, in `portback`. The offline check's ini path is
  still the Windows one; on Linux set `PCSX2_INI`.

## Needs more investigation

- **Real-headset behaviour of the frame loop.** Pacer timing, deferral counts and xrEndFrame times were measured only on the
  null headset, which accepts about 22 frames/s and is flaky. The real Frame at 90 Hz is untested.
- **Menu pointer thresholds** are estimates for real hands: dwell 150 ms, rest 100 ms, pull start 0.1, 500 ms
  resting-finger cutoff, 120 ms lookback, 25% grown rect, 250 ms settle. Tune them from the session.
- **`menutest` is flaky on this PC.** A multi-press steer sometimes aborts with "the cursor did not move" (about 1 in 17
  steers). The belt squeeze sometimes fails to open the item screen. The submenu test sometimes sees no flag. An A/B run
  showed the same flakes on the previous build (97bd99c), so they are harness or game-timing issues, not wave 8
  regressions. The pouch scenarios need the null headset (no head pose without a runtime). Worth making the harness
  sturdier (longer presses or a re-press).
- **`hand: left` no longer works** for the menu pointer. Both shipped profiles use `right`. Since wave 8, a left ray only
  stands in for an untracked right controller.
- **Full view, minor review findings left (they only apply when it is turned on):**
  - a FOV change while the camera holds perfectly still can keep the quad sized for the old FOV until the view moves;
  - canted displays understate the sideways reach in the fit and the logged margin;
  - the wider FOV may cost some speed or show pop-in at the edges (not checked in the game);
  - the edge fill is head-locked, so it is slightly off during fast turns.
- **vrtest `smooth-turn` comes up short on this PC.** It turned 96 to 146 deg where the test expects 180 +-20. The
  previous build (97bd99c) did the same (-145, -148, one pass), so the PC's timing is the cause, not wave 8. Recheck after
  the reboot. arm, holster, point-to-aim, belt and body-follow pass on 0a9fa63.
- **File #2 harness gaps:**
  - run-latch needs a save with open floor ahead;
  - the `pitch` scenario fails when raised through the holster from some saves.
- **`grabtest idle-shake` held-word blips:** one-frame D-pad and Cross blips that `PCSX2_VR_PADLOG` showed do not come
  from the VR input. Recheck after the reboot.
- **The PC hitch** (about 300 ms every second, seen 2026-10-11 from about 13:40) was gone by 2026-10-12 ~04:00: 97% game
  speed and no PINE stalls. Keep an eye on it.
- **Steam Frame:** performance and heat with two-eye rendering at 72 to 90 Hz in about 10 W; the runtime's extensions and
  queues; the SteamVR-for-Linux bugs #975, #989 and #972. See `STEAM-FRAME.md`.

## Running on the Steam Frame itself

Feasible, and nothing found so far blocks it. It would run on ARMSX2 (native ARM64 recompilers), not on this x86 build. The
image would be softer than the PC's (expect 1x to 1.5x internal resolution and 72 Hz first) and the battery would last about
1 to 1.5 h. The plan has six milestones, about 3 to 5 weeks at CI pace, plus 5 or 6 short headset sessions: `STEAM-FRAME.md`.

**M0's code is done** on ARMSX2 branch `outbreak-vr-frame`, at `17d5c67`. CI is green: the build, the unit tests and the
`frame-smoke` job. `outbreak-vr` and `master` are untouched until Alex has seen it. It has two slices:
- **The probe:** `frame-xrprobe`, a small program that answers the runtime questions in one headset session, plus the
  `ENABLE_OPENXR` build option (off by default; only `outbreak-vr.yml` turns it on).
- **The VR layer:** PenguinScreen2's `pcsx2/VR` (from `c2c5ee2`, 39k lines) copied in. It compiled with clang 17 on the
  first try, with no fixes needed. Shims and the M0 hooks are added, all under `#ifdef ENABLE_OPENXR`: the queue lock, frames
  to the headset, settings, input with rumble, and `--vr` / `-vr-info`. Also `frame/launch-outbreak-vr.sh` (logs to
  `~/armsx2-vr/logs`, writes a first-run ini: Vulkan 1x, MTVU, VR on, head camera and stereo off) and a hook audit.
  `pcsx2/VR/VR-SOURCE.md` lists every copied file and about 50 hook sites, with their status.

**M1 is done too** (`22a1676`, CI green), on the same branch:
- the per-vsync VR block: head pose drives the game camera;
- one owner for the stereo state;
- PenguinScreen2's first-person GS changes: near floor, roll parallax, splits, depth classes and the snapshot layer;
- a `(VR) Pose-to-photon:` line every 5 s;
- `tools/outbreak-vr/hook-parity.py`, which checks in CI that each PenguinScreen2 VR hook has its ARMSX2 counterpart.

**M2 is done too** (`b3ba84d`; CI green on `2566847`):
- the online locks: no pause, frame advance, state load or save, or speed change while the network adapter is on, in
  VMManager, the Qt buttons, the hotkeys and PINE;
- the PadLook probes, the recenter chord (L1+R1+L3+R3) and right-stick turning;
- the VR hotkeys (recenter, brightness);
- PINE's VR commands 0xE0-0xE5;
- smooth turn in the first-run ini.

**M3 is done too** (`bd7198a`; CI green on `60f5acd`):
- hands, HUD, wrist card, menu pointer, pouches and item names all run (no hook site was missing);
- a layer budget that stays within the runtime's `maxLayerCount`, dropping the least important layers first, and logs it;
- the flat-quad fallback when the runtime has no cylinder layer, logged;
- the Qt VR settings page, plus every `[VR]` key listed in `README-FRAME.md` section 6 (the Qt window can't be reached in the
  headset);
- `tools/pine.py` reaching the Linux PINE socket, so the research and test tools work against the Frame over SSH.

Hook parity is 60 of 85 PenguinScreen2 VR blocks fully ported, 5 ported in part and 20 deliberately left out (each with its
reason). The checklists for Alex's Frame session are sections 3 to 5 of `README-FRAME.md` (M1 to M3). M4, performance and
heat, needs measurements from the device.

The launcher's first-run ini now starts in first person. `FRAME_VIEW=flat` (a Steam launch option) gives M0's flat screen.
The artifact is `outbreak-vr-frame-<sha>` from the latest green run: a tarball with the AppDir, the launcher, the probe and
`README-FRAME.md`. Hands, the HUD, menus and pouches come in M3.

The pose-to-photon edits to `XRCompositor.cpp` should be ported back to PenguinScreen2. They are listed in ARMSX2's
`pcsx2/VR/VR-SOURCE.md`, and were not ported yet, to keep the PC build Alex tests unchanged. CI is green on `4bc0998`: the build plus a `frame-smoke` job that runs the probe with no runtime on ubuntu-24.04-arm. The
artifact is `outbreak-vr-frame-<sha>` (a tarball with the AppDir, `frame-probe.sh` and `README-FRAME.md`). How to deploy and
run it, and what it answers, are in ARMSX2's `OUTBREAK-VR.md` and `frame/README-FRAME.md` on that branch.
- The probe has never met a real OpenXR runtime; only the no-runtime path ran in CI.
- It needs glibc 2.38, which the SteamOS host has (2.39). It will not start inside a Sniper container.
- It suggests both Frame controller profile paths. Valve's input page names `/interaction_profiles/valve/frame_controller_valve`;
  PenguinScreen2 uses `/interaction_profiles/valve/frame_controller`, which SteamVR on the PC accepted in full in the 10 Oct
  session (`emulog-20261010-121201.txt`).

Next on the Frame:
1. Alex runs the session from `frame/README-FRAME.md`: the probe, then M0 (`FRAME_VIEW=flat`), then the M1 checklist (first
   person, depth, the world holding still, speed). He sends back the logs.
2. Retune `camera.lookAt.syncFramesBack` and `screen.firstPerson.poseLagMs` from the `(VR) Pose-to-photon:` lines if needed.
3. M3 (hands, HUD, menus, pouches), then M4 (performance and heat).
   - The M2 checklist is section 4 of `README-FRAME.md`.
4. Merge `outbreak-vr-frame` into `outbreak-vr` once Alex agrees.

The first-run ini binds no pad, so Pad 1 has only the VR Gamepad overlay; a keyboard or Steam Input pad needs binding in the
ini.

## Important to know

**Rules (Alex's, still in force):**
- Never run the game faster than normal speed (the obsrv server bans it).
- Never pause or freeze the emulator while online (the server drops you after about 30 s).
- Never load or save a state online. First person may write game memory and patch code online, but never another player's
  character.
- Online only when Alex says so, and only in private co-op with his brother on the Mac's private Bioserver. On obsrv, first
  person stays off (3D screen). No logins, terms or publishing without him.
- Never upload or share discs, BIOS, memory cards or savestates. This repo holds code only.
- Ask before changing SteamVR configuration. The null-headset test (`null_driver.py on`, then `off`) is allowed and always
  restored; `steamvr.vrsettings` is the original.
- Never confirm a menu entry automatically.
- Leave Alex's emulator graphics settings alone until he has tried the build.
- Commits use the beadbeed noreply address.

**Test saves on the PC** (in Alex's savestate folder, never committed):
- File #1: slot 3 (pitch test), slot 4 (the usual test spot), slot 5 (gun equipped; a zombie grabs Kevin 5 to 7 s after
  loading), slot 6 (calm, gun equipped, empty grid, .45 in the equipped box).
- File #2: slot 1 (Training Ground), slot 2 (Training Ground, .45 equipped).

**Headset-free test kit** (everything refuses to run with the network adapter on):
- self-tests at launch: `PCSX2_VR_SELFTEST=1` (camera, gamepad, auto stereo, FOV, edge fill, item names, pouches, menu
  pointer), `PCSX2_VR_HUD_SELFTEST=1`, `PCSX2_VR_FEEDBACK_SELFTEST=1` and `PCSX2_VR_HANDS_SELFTEST=1` (that one needs a VR
  session). Results go to the emulog. On 2026-10-12 every one passed on 5be8d87: MenuPointer 200 checks, BeltPouches 65,
  item names 31, HUD and feedback 0 failures.
- `vrtest.py`: first-person scenarios on both Files. `--check-profile` needs no emulator.
- `menutest.py`: menu pointer and pouches; `--test-uv` works with no runtime.
- `grabtest.py`: shake to break free.
- `test_fakevr.py`.
- Scripted controllers: `PCSX2_VR_TEST_INPUT=1` plus `pine.py`'s `FakeController`. Close the PINE connection before starting
  another PINE client.
- The SteamVR null headset: `null_driver.py on`, run, then `null_driver.py off`.
- No OpenXR runtime at all: `XR_RUNTIME_JSON` pointing at a missing file, plus `PCSX2_VR_FAKE_HEADPOSE` for a fake head.

**Game speed:** read the OSD, or the game counter at EE `0x48BF93` (u8; 212.4 increments/s = 100% on File #1). The telemetry
vsync count undercounts.
