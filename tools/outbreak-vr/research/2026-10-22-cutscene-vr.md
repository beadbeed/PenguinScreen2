# Cutscenes in VR: the game's own camera through a world-locked stereo window (2026-10-22)

Branch `cutscenevr` (from `nightly` 33f8a81), `[VR] CutsceneStereo` (on). SLPM-65428 (English patch), offline, headset-free
on a portable dev install (`D:\dev\PenguinScreen2-build\run-cut`, non-LTO, Vulkan 3x). Savestates copied from
`D:\Games\PS2\PenguinScreen2\sstates`; RAM, dumps and images stayed in the session scratchpad.

Labels: **verified** = read in the code or memory and cross-checked; **checked offline** = measured in a run;
**assumed** = inferred, not checked.

## Short answer

- **Where the cutscene camera comes from (verified, code).** Status 4 (`u8 0x48BF60`, handler `0x5CE2F0`) runs the
  cutscene player `0x5BD690` from step 3 (`0x5CE484` -> `0x5BDA70`). The player keys the ordinary camera block: eye
  `0x306338`, target `0x306344`, FOV `0x306380`, roll `0x30638C`, through `0x590400` (camera-key sets, a 3-point curve per
  key, `0x3415E0`, eased by `0x341550`) and `0x5908A0` (12 curve tracks: roll, eye xyz, lens -> FOV, target xyz; keys
  interpolated by `0x1A42E0`). Both then call `0x345740`, and the frame draw `0x341B80` calls it again each frame, so the
  view `0x3062E0` and the screen matrix `0x2771A0` are rebuilt from those values every frame, exactly as in play. The room
  camera `0x58E420` is not run in step 3. Near, far and screen distance are not changed per shot; FOV is. So the director's
  camera is read where first person already reads the game's camera. (Static reading of slot 7's RAM by a research agent.)
- **Two kinds of cutscene (verified, code and memory).** `u8 0x35FFFF` picks the frame callback at the start (`0x5BC140`):
  1 is the minimal frame `0x341AF0` (no 3D: a movie), anything else the full frame `0x341AB0` (real time).
- **What is built:** `[VR] CutsceneStereo` shows status 4 through a world-locked window. The game's camera is placed where
  your head was as first person handed over (level, same yaw). The window is the camera's image plane 1 m ahead, at its own
  field of view, so the scene keeps its real size. The GS draws each eye's layer through that window from where the eye
  really is (TrueStereo's vertex map, off-axis), and the compositor sends the two layers as a projection layer. Leaning
  looks around a little: the head's offset is softly clamped at 15 cm. Nothing in the game is written: the cutscene, its
  timing, its flags and the speed are untouched.
- **Checked offline:** slot 7's own cutscene plays to the end with the feature on (28.5 s of status 4, the same as with
  it off), and first person comes back on the vsync status returns to 2. The window's per-eye map is checked in the GS on
  first-person frames forced through it: each eye's layer is the game's own render from that eye, shifted by the window's
  off-axis centre (15 px at 3x for 3.2 cm, as predicted). The cutscene in slot 7 is a movie (kind 1), so its frames
  are flat in both eyes, on the window's plane, as they should be. On SteamVR's null headset the window goes up as the
  cutscene starts and is sent as a projection layer of the eyes, 60 frames/s, no XR errors. No real-time 3D cutscene
  could be reached from the savestates, so that case is not run (see Not checked).

## 1. The status-4 path (verified, code; research agent's static reading of slot 7)

| What | Where |
|---|---|
| Player struct | `0x35FFD0`: state +0, sub-state +1, frame counter +8 (+2 a frame when `u8 0x30659C` is set, else +1), length +0x10 |
| Per-frame events | 16 handlers from `0x6D8A70`, timeline events of 16 bytes matched on a u16 frame at +2 |
| Camera keys, sets | handler 0 `0x5BDD10`: sets from `0x3613B0`, records of 0x38 bytes (frame +2, point +4, FOV +0x14, roll +0x18, control point +0x1C, ease +0x30/+0x34, next key +0x3A); byte +1 picks the eye path (slot `0x23BD48`) or the target path (`0x23BD4C`) |
| Camera tracks | handler 8 `0x5BEF10`: 12 tracks at `0x35FF6C + 4i` (16-byte keys: value, time, two tangents), `0x5907C0` |
| Writers of the camera block | `0x590400` (copies `0x3062E0..0x306390`, overwrites eye, target, FOV, roll, copies back), `0x5908A0` (only while `u8 0x35FF60` is set); both then `jal 0x345740` |
| Projection | `0x345480` resets near 1, far 50000, screen distance 500, FOV 27 at each start (`0x341ADC`); FOV per key after that |
| Frame | `0x341B80` -> `0x345740` at `0x341BDC` and `0x341DA0`; kind 1 uses `0x341AF0` (no 3D) |

## 2. Live: slot 7's cutscene (checked offline, `tools/outbreak-vr/cuttest.py`)

Slot 7 (J's Bar) enters status 4 by itself 19.1 s after the load (`u8 0x48BF88` = 1):

| | Value |
|---|---|
| Kind (`0x35FFFF`) | 1: a movie (the minimal frame) |
| Player | state 1, frame counter 0 -> 696 in 11.6 s (60 a second), then state 0; "NOW LOADING" for the rest |
| Camera block | untouched during it (first person's last eye and target); FOV 100 -> 40 at the start; eye (0, 0, 500), target 0, FOV 27 (`0x345480`'s defaults) as it ends |
| Status 4 | 28.49 s with `CutsceneStereo` on, eyes forced 5 cm right; 28.50 s with it off; 28.49 s and 28.48 s with it on and no eyes asked for (two runs where SteamVR failed and the build ran flat); the cutscene starts 19.10-19.13 s after the load in every run (polled every 30 ms) |
| Game counter `0x48BF93` | stops in status 4 (0 counts/s; it is the play clock, not a speed gauge there); 255-265 counts/s in status 2, as before |
| First person | DISARMED on the vsync status reads 4, ARMED again on the vsync it reads 2 (`(VR) CutsceneStereo: the cutscene is over.` on the same vsync) |
| Stereo dumps | 90-103 cutscene pairs per run (sidecar `"cutscene": true`): both layers identical (mean difference 0.000-0.001), as for any movie: its frames are pictures, drawn flat on the window's plane |

The same layers stayed identical with `CutsceneStereo` off and the old world-screen separation (0.012): the movie has no
3D draws to separate, whatever the stereo.

## 3. The design: a window, not a full view

A cutscene's camera is the director's: it pans, dollies, cuts and frames close-ups. Three ways to show it:

1. **Full view (the camera is your head):** the whole headset view is the cutscene camera. Every pan and dolly is motion
   your body does not feel (vection), in front of a seated player, many times a cutscene. The game's field of view (27-40
   deg tall in cutscenes) is also much narrower than the headset's, so the rest of the view would be black or stretched,
   or the game would have to draw a wider FOV, which changes the director's framing. Not comfortable, not one-to-one.
2. **A flat screen (today's world screen):** comfortable, but flat.
3. **A world-locked stereo window (chosen).** The window holds still in the room, so your vestibular sense and your eyes
   agree; the camera's motion happens inside the frame, as in a cinema. Through it the scene is in true stereo at its real
   size: the window spans exactly the camera's field of view from where your head was, so an object 2 m from the camera
   is 2 m from you. Each eye sees through the window from where it is (an off-axis view), so leaning gives a little
   parallax, as a real window does. The window's plane is 1 m ahead: things nearer than 1 m to the camera come out of the
   window (crossed disparity), the rest sit behind it. The frame's edges, the letterbox, fades and subtitles sit on that
   plane, where the game drew them.

Comfort: the head's offset from where it was is softly clamped (`r' = 0.15 tanh(r / 0.15)` m), so small leans are
one-to-one and the look-around eases to a stop. Turning the head only moves where the window is in your view (the
runtime's job: the layer is world-locked). The camera's roll and pitch stay in the picture: the window itself is level.

## 4. What is built

- **`VR::CutsceneView`** (pure; 19 self-test checks): the soft clamp, the eye positions from the head in the anchor's frame
  (half the IPD along the head's own x axis, so a turned or rolled head moves the eyes as it should), each eye's window
  tangents (`(D t0 - e) / (D + e_z)`), the eyes as EyeProjection eyes (not turned, placed at their offset, each spanning the
  window), the head in the anchor frame. Checks: no disparity on the window plane for any eye, the window's corners on the
  target's, uncrossed disparity `fx ipd (1 - D / z) / D` behind it, crossed in front, the clamp, a turned head, degenerate
  input.
- **Profile:** `camera.cutscene: [{ address: 0x48BF60, equals: 4, width: 1 }]` (parsed like `roomLoad`).
- **`CameraDriver`:** every vsync, a cutscene is `camera.cutscene` holding with first person disarmed (VR and HeadCamera
  on, no split screen); while it holds, the screen matrix (`0x2771A0`) is read for the eyes' intrinsics. Read only.
  `CutsceneActive()` (with the switch) and `InCutscene()` (without, for the stereo dump) lapse 250 ms after the last vsync
  that decided them. Logged: `(VR) CutsceneStereo: a cutscene: the game's camera in a stereo window.` and `... the cutscene
  is over.`
- **Compositor:** while a cutscene is on the world screen, the world screen is the cutscene window: placed at the world
  anchor (where the head was as the screen took over, level), always a flat quad per eye (not curved, not head-following).
  Each present it asks for the eyes of the frames to come (the head now, in the anchor frame, clamped) and shows the frame
  with the eyes the GS last drew with (`StereoState::SetCutsceneRequest` / `TakeCutsceneUsed`); `BuildSubmitArray` sends the
  two eye quads as a projection layer (`BuildTrueProjection`: view i at anchor x eye i, fov = that eye's window). The window
  is held while a switch back to first person waits for its blink, so it is never swapped for a flat screen in sight.
  Needs `PacerCopies` off.
- **GS** (`DetermineVSConfig`): on the stereo target, outside first person, with a fresh request: TrueStereo's `vr_map_mode
  3` with the cutscene eyes. Perspective draws are mapped per eye; flat draws (FST, constant Q) are put on the window's
  plane at the game's own scale (so they land where the game drew them, the same in both eyes); full-target fills stay as
  they are. The menu panel's flat exception does not apply.
- **Entering and leaving:** the existing comfort blink covers both changes of screen (first person -> window as the cutscene
  starts, window -> first person as status goes back to 2); first person arms on the vsync status reads 2, and the window is
  held until the blink is black.
- **Test aids** (headset-free, offline): `PCSX2_VR_CUTSCENE_TEST=<x>,<y>,<z>[,<ipd>]` (the head at that offset when there is
  no compositor to ask), `PCSX2_VR_CUTSCENE_FORCE=1` (the window map on first-person frames too), `PCSX2_VR_CUTSCENE_LOG=<n>`
  (the first n draws of a cutscene, mapped or not, and the first outputs), the stereo dump during cutscenes (sidecar
  `"cutscene"`), `tools/outbreak-vr/cuttest.py` (slot 7's cutscene end to end; `--xr` on SteamVR's null headset). The tool
  refuses to start while another emulator or SteamVR runs.

## 5. Checks

| Check | Result | Label |
|---|---|---|
| `vr_selftests` | 63 of 63 suites (the new `Cutscene view`: 19 checks) | checked offline |
| Slot 7's cutscene plays to its end, timing unchanged | status 4 for 28.49 s with the feature on (eyes forced), 28.50 s with it off, 28.48-28.49 s on without eyes; starts 19.10-19.13 s after the load in every run; player frames 0 -> 696 | checked offline |
| First person comes back | ARMED on the vsync status reads 2, the same vsync the cutscene is over | checked offline |
| The GS maps the cutscene's draws | `(VR) CutsceneStereo: first cutscene draw per eye; game fx 527.515 ... eyes at (0.016, 0, 0) and (0.080, 0, 0) m, window 1.00 m, right eye fov -29.5/22.1/20.0/-20.0 deg` (eyes 5 cm right: the clamp gives 4.82 cm); the first draws: map mode 3, kind 1 (Z fit), each eye's own cx and offset | checked offline |
| The window map in the GS against the game's own render (slot 7, first person, `PCSX2_VR_CUTSCENE_FORCE=1`, eyes at +-3.2 cm, against a run with the head 3.2 cm right and the identity eyes: the view's translation moved 3.20 units) | the right layer is the moved camera's render shifted by the window's off-axis principal point, as it must be: per 96 px tile a uniform dx of 15 px (expected 161.107 x 3 x 0.032 / 1.0 = 15.5), 85-87% of 174-178 tiles within 1 px of it, dy 0; mean pixel difference 1.1-1.2 after the shift, 9.5 before (three frame pairs) | checked offline |
| The two layers differ (the same forced frames) | right against left per tile dx p5 -27, median -7, p95 +8 px at 3x (the far limit is fx ipd / D = 161.1 x 3 x 0.064 = 31 px; things nearer than 1 m cross) | checked offline |
| Movie frames | both layers identical, on the window's plane | checked offline |
| SteamVR's null headset (`cuttest.py --xr`, SteamVR started first with `vrstartup.exe`; two runs before that failed in `xrGetVulkanGraphicsDevice2KHR` and one session was ended by SteamVR at once, the known flakiness) | the window goes up 64 ms after status reads 4 (`Screen: world-locked`, `the cutscene window is up`), the first window frame 20 ms later, and the projection layer is built from the eye quads: `first window projection layer: left eye at (0.032, 0, 0), right at (-0.032, 0, 0) m; fov left -27.3/24.4/20.0/-20.0, right -24.4/27.3/20.0/-20.0 deg; 1536x1344` (the null headset's head pose is rolled 180 deg, so its eyes are swapped; first person's TrueStereo line shows the same, right eye at -0.0315 m). 60 frames/s submitted at 90 Hz, 57.6-60 new GS frames/s, "no projection 0", no XR errors; status 4 for 28.46 s (both runs); on the way out first person is armed on the vsync status reads 2 and the window comes down with the blink (64 ms in one run; 0.68 s in the other, where the game's room load stalled the presents). The null driver was put back after (`null_driver.py off`; `steamvr.vrsettings` byte-identical to its backup) | checked on the null headset |

## 6. Not checked, and what to watch
- **In the headset:** that the window holds still while the head turns and leans, and how the blink at both ends feels
  (the null headset's head does not move).
- **The eyes of the shown frame** are the ones the GS drew with last, so a frame whose draws spanned a present shows with
  the newer eyes (a head move of one vsync, under 1 cm at normal speeds). The first 0.1 s of a cutscene still run under
  first person's disarm grace (`disarmAfterVsyncs: 6`), as before.
- **Headset:** is the window size right (the camera's FOV: 40 deg tall at slot 7's start, 27 by default), is 1 m the right
  plane, is 15 cm of look-around comfortable?
- **Co-op:** the host broadcasts the start; the window is display only, so it is the same online. Not run online.

## 7. A real-time cutscene (J's Bar cutscene 3, kind 0)

**Finding it (verified, code and memory).** Op 0x62's five arguments go into a 16-byte queue entry; its first becomes the kind
`0x35FFFF` (`0x5BF6E0`; 3 is played as 1). In J's Bar's script (`0x90E1E0`-`0x911000`) 16 cutscenes have kind 0. Blocks are
`EC flag 1 0 ... E5` (`0x65F910`: runs once when event flag n of `0x48AD90` is set, marking it in `0x48ADD0`). Cutscene 3
(`0x90E690`: 0, 3, 1, 0, 0x39) runs on flag 0x39, which the block for flag 2 sets (`0x90E5F0`). Flag 2 is set by an area node
(the trigger list at `*0x23BD90`, gp = 0x241C70; type 3, `0x5939C0`) over x 9089-9299, z 12557-12687 by the door, with condition
kind 2: the character's `+0xB98` must be 1, so the AI partner (record 1) has to walk in, not Kevin. The other kind-0 cutscenes'
flags are set by nodes in other rooms.

**What was tried.** Kevin walked into that area (scripted stick, head-yaw steering, nothing pressed) from three starting
points, waiting 20-50 s, also with right-stick flicks: no cutscene; the partner wandered off (to 8533-8915, 12425-12745).
Walking into the flag 0x1A areas did nothing (those nodes set per-slot flags, `+0x44` = 1).

**The test harness.** In the dev install only, offline, from its own savestate (slot 0, after slot 7's movie): event flag 2
set over PINE (`0x48AD90` word 0x3 -> 0x7), the flag the partner's node would set. The game's own script then plays
cutscene 3 (`cutwalk.py`, `CUT_FLAG=2`); slot 4 there is saved at its start.

| Check | Result | Label |
|---|---|---|
| A real-time cutscene | kind 0; the camera block moves (110-113 distinct eyes in 35 s); the player's frames 0 -> 722 | checked offline |
| Both eyes | the first draw is mapped per eye (fx 527.5; eyes 5 cm right: (0.016, 0, 0) and (0.080, 0, 0) m). In stereo dumps of the 3D shots (the bartender on the floor, another shot of the bar), right against left per 96 px tile: dx p5/p95 -3/+15 px and -29/+18 px; mean pixel difference 6.4 and 8.5; no doubling seen. The loading frames after the shots are identical in both eyes. | checked offline |
| Timing unchanged | from slot 4: status 4 for 35.39 s with it on, 35.35 s off, 35.38 s on the null headset; 722 player frames each | checked offline |
| First person comes back | ARMED on the vsync status reads 2, every run | checked offline |
| The window on SteamVR's null headset | the window is up as status reads 4. First window frame and projection layer 8 s in, at the first 3D frame: eyes +-3.2 cm; fov right -16.1/19.4/13.5/-13.5 deg (FOV 27, the cutscene default, so a smaller window than the movie's 40). 57.8-60 frames/s, "no projection 0", no XR errors. Down 58 ms after first person is back | checked on the null headset |

Not checked: the partner walking in by itself (the natural trigger), the headset.
