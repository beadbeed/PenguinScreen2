# Menus without the pull-away: the menu panel (File #1, 2026-10-13)

ROADMAP road step 1, stage 1. SLPM-65428 (English patch), Kevin, offline, Single Play. Live checks on the portable dev
install `D:\dev\PenguinScreen2-build\run-menu` (local build of branch `menupanel`, Vulkan, 3x internal resolution).
Headset-free runs used no OpenXR runtime (`XR_RUNTIME_JSON` at a missing file) and `PCSX2_VR_TEST_INPUT=1` with
`pine.py`'s `FakeController` for a scripted head. Null-headset runs used SteamVR 2.17.10 with `null_driver.py on`, put
back with `null_driver.py off` (md5 of `steamvr.vrsettings` identical before and after, no `forcedDriver`). The network
adapter was off and the game never ran faster than normal speed (the GS presented 90 vsyncs in 1.5015 s, 59.94/s, in
the trace's timestamps). Screens were opened with the game's own buttons through scripted input (Start through the
belt squeeze, Triangle, Circle at the sink, Select) and closed with the same button or Cross; nothing was confirmed:
the take-it prompt was declined with Cross. Captures stayed in the session scratchpad; none are committed. Tool:
`tools/outbreak-vr/menupanel_test.py`. Code: `pcsx2/VR/MenuPanel.*`, hooks in `GSRendererHW`, `CameraDriver` and
`XRCompositor`.

## Short answer

| | Item screen | Map | Take-it prompt | Pause |
|---|---|---|---|---|
| The room still drawn every frame | yes | yes | yes | yes |
| The game keeps running (counter u8 `0x48BF93`) | yes, 255/s as with no screen | yes, 255/s | yes, 255/s | **no**, 0/s (offline pause) |
| The game's view matrix follows the head when first person keeps the camera | 40.0 of 40.0 deg | 40.0 of 40.0 deg | 40.0 of 40.0 deg | 40.0 of 40.0 deg |
| Split off the frame (share of the picture on the panel) | yes (43.9%) | yes (27.0%) | yes (38.3%) | yes (51.4%) |
| Menu pointer on the panel | cells 1, 3, 0 reached, 0 confirms | (not a pointer screen) | (cells, as before) | (not a pointer screen) |

With `[VR] MenuPanel = true` a game screen keeps first person: the room keeps rendering around you in stereo, from your
head, and the screen's own draws are shown on a flat panel 1.0 m in front of you. Off (the default), nothing changes.

## 1. Does the game render the room under a screen, from which camera, and does it keep running?

**Yes for all four screens seen.** A trace of every GS draw (`PCSX2_VR_MENUTRACE`, below) shows each screen's frame as
the normal frame plus the screen's draws after it, in the same colour buffer:

| Frame | Draws | Order |
|---|---|---|
| No screen | ~65-110 | the frame's depth clear (a 512x448 sprite writing z = 0), then the room (depth-tested perspective draws) |
| Item screen | ~124 | clear, room (57), **a second depth clear**, the screen's backdrop (4 full-frame blended sprites from the menu texture at `0x2E20`/`0x2E40`), its windows and text (UV sprites), a full-frame translucent fill, more text |
| Map | ~107 | the same pattern; the map itself is ordinary UV sprites and a few depth-writing UV triangles |
| Take-it prompt | ~135 | clear, room, a few draws into an offscreen buffer (`0x2A20`), a full-frame darkening sprite from it (the screen's first draw), a depth clear, the backdrop, the windows, the item's own turning 3D model, another clear, a full-frame fill, then the text, **depth-tested** against the screen's cleared depth |
| Pause | ~480 | clear, room (123-128), then the pause screen (about 350 draws, mostly glyphs) |

Each frame is drawn into the other of the two colour buffers (`0x0`, `0xE00`) and shown twice (30 fps), as in normal
play.

**The camera.** File #1's room camera writers are already silenced in first person (the profile's `camera.silence`,
whenever the GAME overlay is resident), so under a screen the camera is whatever first person writes. With the switch
on, CameraDriver keeps writing the eye and target from the head under the screen ("camera only", below). Measured
with the scripted head turning 40 deg while each screen was up (`menupanel_test.py`): the game's own view matrix
(`0x3062E0`, the matrix it builds from eye and target and renders with) turned 40.0 deg with it under all four screens,
within 0.0 deg over the last second, and the render-pose sync matched 26-33 frames to our writes in each 4 s. With the
switch off the camera held still, as before (turned 0.0 deg; lookAt active in 0% of the samples).

**Running.** The game counter rose at 255/s with no screen and 252-256/s under the item screen, the map and the take-it
prompt: play goes on, as the UI research found (a zombie can still attack you). Pause offline stops the counter
(0/s), yet the room is still drawn every frame and still follows the camera, so the room around you holds still in
time but turns with your head.

## 2. Can the screen's draws be told from the room's, and split off?

**Yes.** The split is per frame, at the screen's first draw (`VR::MenuPanel::SplitTracker`, pure and unit-tested):

- **Classification** (every draw, before it is rendered, from the GS state the stereo code already reads):
  - *room*: depth-tested and perspective (textured with STQ whose Q varies, or untextured with a varying Z);
  - *clear*: depth-writing, not depth-tested, covering at least 90% of the scissor (File #1's frame clear and the
    screens' own);
  - *overlay*: flat (UV coordinates, one Q, or untextured at one Z), whether or not depth-tested (the take-it text is);
  - *backdrop*: a blended overlay over the whole frame that writes no depth (the screens' tint and fill).
- **The split point.** The colour target is snapshotted (the GS's own `CopyRect`, in draw order, both eyes' layers) just
  before the screen's first draw: a clear or a backdrop after the frame's room (every screen seen starts with one or the
  other), or, with a screen known to be up, any overlay after the room. Everything after it in that frame is the
  screen's, its own 3D included (the take-it prompt's turning herb). Only when a plain overlay asked and the room
  carries on is the snapshot dropped (2D inside the room). A frame ends when its target has been shown and is drawn
  into again.
- **The backdrop is left out** while a screen is up (the draw is skipped, render only). The room needs no tint around
  you, and on the panel the tint would have covered 95% of the picture (measured before the skip: 1960011 of 2064384
  px).
- **The present.** Merge reads the room snapshot instead of the finished target (`GSRendererHW::GetOutput`), so the
  first-person screen shows the room alone. A compute pass (in the GS command buffer, before the present's submit)
  compares the finished target's left-eye layer with the snapshot's and writes every pixel that changed by more than
  2.5/255 opaque, the rest clear, into the panel image the compositor copies.
- **First frames.** The clear and the backdrop ask for the snapshot even before CameraDriver's flag reaches the GS
  (the trace: the item screen's first frame with screen draws, vsync 896, was already split), and whether a frame
  shows split is decided at its present. No frame with the screen drawn over the room was shown on the first-person
  screen in the traces.

Checked against dumps (`PCSX2_VR_MENUPANEL_DUMP`: the panel, the room and the finished frame side by side): the room
images carry no screen pixels for any of the four screens; the panels carry the windows, text, icons and the map, with
the room's colours inside translucent windows. Shares of the picture on the panel: item screen 43.9% (within
57,72-1487,1272 of 1536x1344), map 27.0% (its full-width rules reach the edges), take-it prompt 38.3%, pause 51.4%.
The same counts came out on the null headset.

The room's pixels are untouched and keep their stereo. Its depth does not survive a screen: the screen's own depth
clear covers it in the depth target, so while a screen is up the depth export declines (`(VR) Depth export: no depth
for the shown frame: a game screen is up ...`) and with `[VR] DepthLayer` on those presents go as the two quads.

## 3. The room in first person, the screen on a panel, the pointer on the panel

- **Camera only** (`CameraDriver`, `ApplyLookAt(..., camera_only)`): under a screen the eye and target are written from
  the head and nothing else: no stick turn (the right stick is the menu's D-pad in menu mode, as before), no aim or
  walk-and-shoot, no body-follow heading, no walking for the vignette, the hands hold nothing; the yaw anchor stays, so
  the view is where it was when the screen goes. Menu mode is published as before, so the sticks and buttons go to the
  menu exactly as with the pull-away; the sprint latch lets go under a screen (Cross is the menu's cancel). Pickup cue,
  hand targeting and the grab breakout already stand down in menu mode.
- **The panel** (`XRCompositor`): a world-placed quad 1.0 m ahead of the eyes and 0.12 m below them, 0.80 m tall at the
  picture's 4:3 (1.07 m wide), tilted to face the eyes, placed where the head looks as the screen comes up and held
  there while it stays up; over the first-person screen, under the hands and HUD; its own swapchain (the panel image's
  size, 1536x1344 at 3x), copied each present after the GS work that made it, released at once, kept while the GS is
  idle and parked when hidden. The first-person screen, the edge fill, the stereo and the depth layer go on as in play.
- **The pointer**: the published screen transform is the panel, so the menu pointer's ray, dot and point-to-select work
  on it unchanged (the panel covers the whole picture, so its u, v are the picture's).
- **Null headset**: `(VR) Menu panel: shown 1.00 m ahead, 0.12 m below the eyes, 1.07 x 0.80 m`, the transform published
  as the panel; the right hand's ray aimed at the item screen's cells 1, 3 and 0 hit (0.938, 0.246), (0.938, 0.369) and
  (0.851, 0.246) for (0.939, 0.245), (0.939, 0.368) and (0.851, 0.245); the cursor reached each in 0.16-0.25 s with 1-2
  D-pad presses and no confirm. The screen mode stayed first person throughout (no `(VR) Screen: world-locked` line),
  frames went at the null headset's 60/s with no xrEndFrame error. Headset-free, PINE test hits steered the same three
  cells in 0.16-0.38 s.

## What is verified, and what is not

Verified:
- **built** (local `build-menu`), **self-tests passed** (`vr_selftests` 23 of 23; the new Menu panel suite has 43 checks:
  the draw classes, the split point for each screen's pattern, the backdrop skip, two buffers, shown and redrawn, a
  failed snapshot, 2D in the room, no screen known up, a 2D-only frame, slot reuse);
- **checked offline, headset-free**: questions 1 and 2 for the item screen, the map, the take-it prompt (bathroom Green
  Herb, slot 8) and pause; the pointer on the item screen; the switch off as before;
- **null-headset checked**: the panel layer, its transform, the pointer's ray hits and steering, no pull-away.

Not checked, or assumed:
- **The headset.** Whether 1.0 m / 0.80 m is comfortable and readable, and how the room feels around it.
- **Partner hand-overs.** They open the same take-it overlay (the UI research), so they should split the same way;
  not seen (the REQUEST that starts one is a menu confirm, which tests never send).
- **Translucent windows** are opaque on the panel, with the room as it was behind them baked in (flat). The split keeps
  what the screen changed, not how; real translucency would need the screen's draws rendered into a target of their own.
- **Mono panel**: the left eye's layer; screens draw at zero separation, so both are the same.
- **The desktop window** shows the room without the screen while the split is on (it shows what Merge made). The
  headset is the only place the screen shows then.
- **Placement** is fixed (constants in `XRCompositor`, `kPanel*`), not yet tunable, and does not follow a turned head.
- **`[VR] PacerCopies` on**: the panel is off (logged), screens pull away as before.
- **Not seen**: inventory-full swaps, the file reader with a file, cutscene text, co-op (private server). Examine text
  and "Obtained" lines are not screens (no `0x720AA0`): they stay in the room view as before.
- **Cost**: one colour snapshot per screen frame (both layers, about 16 MB at 3x, on the GPU), one compute pass and one
  copy per present; the pass is built at the first screen of a session (a one-off pipeline build).

## Debug aids

- `PCSX2_VR_MENUTRACE=<vsyncs>` (offline only): every GS draw of that many vsyncs as `(VR) MTRACE` lines when a screen
  comes up, 1.5 s later and when it goes, with a per-vsync summary (`draws, room, overlay`). `SNAP` marks the draw the
  snapshot was taken before, `SKIP` a backdrop left out.
- `PCSX2_VR_MENUPANEL_DUMP=<dir>[,everyN]` (offline only): the panel, the room snapshot and the finished frame (left eye)
  as `menupanel_<n>_panel.png`, `_room.png` and `_frame.png`, with a log line of the panel's coverage. Never commit them.

## Next (stage 2 and smaller steps)

- In the headset: comfort, size and distance, and whether the panel should follow a turned head.
- Real translucency: render the screen's draws into a second target (they follow the room in every frame seen), or
  mask by the screen's own draw rectangles.
- Stage 2: a VR-native inventory over the game's menu logic (ROADMAP).
