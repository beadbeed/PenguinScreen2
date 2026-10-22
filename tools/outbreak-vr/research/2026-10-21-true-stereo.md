# True per-eye rendering for File #1: the render pipeline and the decision (2026-10-21)

ROADMAP road step 5. SLPM-65428 (English patch), Kevin, offline, J's Bar (savestate slot 7). Static reading of the slot 7
savestate (EE RAM, scratchpad, VU1 data and micro memory, extracted to the session scratchpad, never committed) with
small disassemblers written for this (EE incl. COP2/MMI; VU1 upper and lower). Live checks on a portable dev install
`D:\dev\PenguinScreen2-build\run-stereo` (local non-LTO build of branch `truestereo`, Vulkan, 3x, MTVU on), headset-free
(no OpenXR runtime, the head scripted over PINE with `PCSX2_VR_TEST_INPUT`), normal speed, network adapter off. Dumps,
RAM and images stayed in the scratchpad.

Labels: **verified** = read in the code or memory and cross-checked; **checked offline** = measured in a run;
**assumed** = inferred, not checked.

## Short answer

- **The game's 3D transform is one projective matrix per object, built on the EE and uploaded to VU1.** The EE multiplies
  model x view x projection-with-viewport (`M V PS`) per object (VU0 macro code) and UNPACKs it into a header at the
  bottom of VU1 data memory; the VU1 program does `MVP * v`, `Q = 1/w`, `xyz * Q`, a guard-band clip, and kicks. No
  backface culling, no per-eye anything. (verified)
- **So the second eye can be made exactly from the one draw stream**: a vertex the VU1 would emit for the other eye
  is a fixed 4x4 map of the vertex it emitted for this one (homogeneous screen space, `w` recovered from Q or the
  Z fit). It is the same picture "replay the display list with the other eye's matrix" would draw, rasterized per
  eye with its own depth test, so nothing is image-warped and nothing is disoccluded.
- **Prototype (checked offline):** with `PCSX2_VR_TRUESTEREO_PROBE=5`, the right layer is the slot 7 view moved
  5 cm sideways by that map. Compared with the game's own render from a camera actually moved 5 cm, it matches in
  **186 of 186** 96 px tiles to within 1 px (median 0). The mean pixel difference is 0.44, the same as two frames of
  the same view (0.45). Only 0.5% of pixels differ by more than 16, against 24.8% for the unmoved image.
- **Cost (checked offline, at 100% speed):** EE thread 4.9 to 5.3 ms per vsync (29-32% of a core), VU1 thread 1.5 to
  2.6 ms (9-15%), GS thread 2.3 ms (14%), GPU 6.3 to 7.4 ms per presented frame (2 multiview layers at 3x). The probe
  added nothing measurable.
- **Recommendation: (d) per-eye geometry in the GS**, a fourth option next to (a) to (c). Each eye's vertices come from
  the game's own VU1 output through that eye's view and projection, presented as an OpenXR projection layer per eye.
  Keep (b) (VU1 replay) as the fallback for any view-dependent VU1 effect that proves visible. (a) costs EE time and a
  deep game patch for the same pixels. (c) halves the per-eye rate.

## 1. The frame on the EE

| What | Where | Label |
|---|---|---|
| Camera eye / target | f32 vec3 `0x306338` / `0x306344` | verified (profile) |
| Projection setup (once, on a cut) | `0x345480`: far 50000 `0x306378`, near 1.0 `0x30637C`, screen distance 500 `0x306374`, FOV `0x306380` | verified |
| Per-frame camera rebuild | `0x345740`: projection `jal 0x18F0E0(32, far, near, fov*k, aspect)`, then the view `0x345870` (look-at, into `0x3062E0`), then render-state calls `0x1AE9C0(24, 0x306320)` and `0x1AE9C0(22, 0x3062E0)`. Called by the room camera (`0x58EB14`, the view build first person keeps) and the door camera | verified |
| Projection matrices (row vectors) | `0x277160` P (OpenGL-like: 0.515/0.601 at FOV 40, -1.00004, -2.00004 = near 1, far 50000); `0x2771A0` PS = P x viewport (x' = fx x/w + 2048, fx = 527.5 at FOV 40, 161.1 in first person); `0x2771E0` P x a 512x448 viewport; built in `0x18F0E0` from scratchpad slots 32 and 34 | verified |
| Matrix slots | scratchpad `0x70003700 + 64 * slot`: slot 0 the object's world matrix, slot 33 (`0x70003F40`) the view. Transient: the slot 7 savestate has them zeroed between draws | verified (reads), assumed (who writes slot 33: `0x195970`, the render-state switch behind `0x1AE9C0`) |
| Per-object header build | e.g. `0x1994B0` / `0x1995D0` (about 20 variants `0x199xxx`-`0x19Bxxx`): allocate from the packet heap (`0x18D850`), DMA tag NEXT to the model's own chain, VIF `FLUSHA`, `STCYCL 4,4`, `UNPACK V4-32` to VU1 qw 0, a colour quadword, then `0x198B60`: `MV = M(slot 0) x V(slot 33)` and `MV x PS` (screen matrix), then `0x198AE0`: `MV x P` (clip matrix), then `MSCAL` | verified |
| Geometry | the header's DMA tag chains NEXT into the model's static VIF data (cached display lists); only headers and matrices are rebuilt each frame | verified (tags), assumed (all models) |
| Packet heap | double-buffered, 512 KB halves: index u32 `0x2821B4` (toggles), buffer table `0x2821C4`, base/end/cursor `0x2821B8`/`BC`/`C0`; `0x18D0E0` waits for the other half's DMA, `0x18D150` sends and swaps (`0x18CF10`, assumed to be the VIF1 kick) | verified (heap), assumed (kick) |
| DMA / GS library | Sony libdma/libgraph: D1 (VIF1) register access `0x1131DC`-`0x113464`, D2 (GIF) `0x112D5C`-`0x11382C`, DISPFB/DISPLAY writes (the flip) `0x112908`-`0x112950` | verified |
| Draw passes | characters `0x345210` (first pass `0x345020`), enemies `0x688010` / `0x345170`, NPCs `0x651720`, held items `0x560B60` (near-cull research) | verified (earlier) |
| Frame order on the GS | the next frame is drawn into the other colour buffer (`0x0` / `0xE00`) before this one is shown: depth clear, ~100 opaque, ~68 translucent draws (`2026-10-13-depth-export.md`) | checked offline (earlier) |
| 2D / HUD pass, main loop, vsync wait | not located | open |

## 2. The VU1 program

Read from the slot 7 savestate (`vu1MicroMem`; only 0x000-0x580 holds code, so programs are uploaded per pass with MPG):

- **Header in VU1 data memory** (absolute, not double-buffered): qw0 colour/scale, **qw3-6 the screen matrix `M V PS`**,
  **qw7-10 the clip matrix `M V P`**, qw11-14 clip-to-guard-band screen (1024, -1024, 2048 offsets), qw15-17 a UV
  matrix, qw18-20 a light position; vertices from TOPS (`XTOP`), output buffer at +200, `XGKICK`. (verified)
- **Numerically:** qw3-5 equal rows 0-2 of `V x PS` from `0x3062E0` and `0x2771A0` (the object has an identity
  rotation); qw6 carries its translation. **The projection is folded into the per-object matrix; there is no separate
  view or projection in VU1 memory.** (verified)
- **Per vertex:** `vf27 = MVP * v`, `DIV Q = 1/w`, `xyz *= Q`, `FTOI4`; STQ = UV-matrix x uv x Q, so **Q = 1/w**. Lighting
  is a point light in object space (light minus vertex, 1/sqrt falloff): no eye vector in this program. (verified)
- **Clip:** `CLIPw` of the clip-matrix vertex; `FCAND 0x3FFFF` (all three inside) draws, `FCOR` per plane (all three
  outside one plane) sets ADC (not drawn), else a clip subroutine (`BAL 0x538`). The clip frustum is the guard band:
  the screen matrix scales clip x by 1024 px against the 256 px half screen, so it is 4x the visible tangent (about
  +-63 deg horizontal in the room camera); the near plane is 1 cm. **No backface culling.** (verified)
- Other programs (characters, effects, shadows) were not resident and not read. (open)

So **(b)'s premise holds** (replaying the packets with another matrix would draw the scene from another eye), and so
does its shortcut: for every object `M V' PS' = (M V PS) C` with `C = (V PS)^-1 (V' PS')`, the same for all objects. Each
VU1 output vertex `(X, Y, Z) / w` is that vector divided by w, so the other eye's vertex is `[(X w, Y w, Z w, w) C]` after
its own divide. Exact for positions, texture coordinates (S/Q kept, Q rescaled by w/w') and depth; what differs from a
real second VU1 run is only clipping against the other eye's guard band (4x wide: no visible difference) and
view-dependent shading in programs not read.

## 3. The prototype (checked offline)

`PCSX2_VR_TRUESTEREO_PROBE=<cm>` (`GSRendererHW::DetermineVSConfig`, off unless set): on the first-person multiview
target each perspective draw (Z-fitted or textured perspective) gets the exact parallel-camera shift
`X' = X - fx e / w`. `fx` comes from the game's screen matrix (`0x2771A0`, 161.107 px in first person, cx = cy = 2048:
the log confirms), `w = 1/q` from the Z fit, and the convergence plane, near floor and roll are turned off. It goes
through the existing linear branch of `tfx.glsl` (sep = k/1e5, conv = 1e5). `PCSX2_VR_PERFLOG` logs the thread shares
every 0.5 s (`PerformanceMetrics::Update`).

Runs (`PCSX2_VR_STEREODUMP`, slot 7, head scripted over PINE):
- A: probe 5 cm, head still: layers L and R.
- B: probe ~0 (mono), head at +5 cm, -5 cm and 0: the game's own renders from moved cameras.

| Compared | Tiles within 1 px | Median shift | Mean pixel diff | Pixels off by > 16 |
|---|---|---|---|---|
| A right layer vs B head -5 cm (the real render) | 186 / 186 | 0 px | 0.44 | 0.5% |
| B head 0 vs B head -5 cm (no reprojection) | 4% | 11 px (max 48) | 12.4 | 24.8% |
| A left layer vs B head 0 (same view, two frames) | 99% | 0 px | 0.45 | |

- **Residuals:** the lamp's light glow and one small highlight on the counter. They are animated or view-dependent
  effects; not chased.
- **Sign and the "unshifted left layer": resolved (Phase 2, checked offline).** Both were artefacts of how the dumps
  were labelled, not of the map. The dumps were matched to head poses by time, but the dump writer stalls for up to 25 s
  (two slow PNG writes in flight), and the game itself disarms first person about 20 s after slot 7 loads (scenario byte
  `0x48BF60` reads 4), so the "-5 cm" and "head 0" images were other poses. The stereo dump's sidecar now carries the
  game's view matrix (`"view"`), and each pose reloads slot 7. With that, head +x moves the camera +x along its right
  axis (+-4.98 game units for +-5 cm), and both layers are mapped: see section 7.
- **Not covered by the probe** (covered in section 7): a pure sideways move keeps `w`; turns and asymmetric projections
  change it.

## 4. Costs (checked offline)

At 100% speed in slot 7, first person, headset-free, on the non-LTO dev build (LTO is faster). Per vsync at 60 Hz; the
game draws at 30 fps, so per game frame is double:

| Thread | Per vsync | Share | Per game frame |
|---|---|---|---|
| EE | 4.9-5.3 ms | 29-32% | ~10 ms |
| VU1 (MTVU) | 1.5-2.6 ms | 9-15% | 3-5 ms |
| GS | 2.3 ms | 14% | ~4.6 ms |
| GPU | 6.3-7.4 ms per presented frame | 19-22% | two layers, 1536x1344 |

The spread is run-to-run noise, and the probe made no measurable difference. The share of the EE time that is the
scene's packet build was not measured; it is at most the 10 ms.

## 5. The options

**(a) Double submit in the game.**
- **Change:** call the draw passes twice per frame, with the view in slot 33 (and `0x3062E0`) set per eye. Point the
  second pass at another FRAME/ZBUF (or the second half of a wider buffer); draw the HUD once.
- **Breaks:** the draw passes also run update work. The bone matrices are rebuilt on the EE before upload
  (`2026-10-14-vr-melee.md`), as are particles and the cull and hide flags, so each pass has to be split into update and
  draw first. The heap (512 KB per half) has to hold two scenes. MenuPanel's split and DepthExport's tracker key on
  FRAME/ZBUF and the draw order, so both change. The near cull is eye-independent and stays.
- **Cost:** EE +(scene build, up to ~10 ms per frame), VU1 x2, GS thread x2; it fits the 33 ms budget on paper.
- **Risk:** high. Many game sites, and lots of cave code.

**(b) Packet replay in the emulator.**
- **Change:** after the frame's VIF1 chain, replay it from the heap half, which survives one frame. Multiply the
  matrix quadwords of each header by C on the way in; their offsets differ per variant (qw1-4 or qw3-6), so each variant
  needs a table. Save and restore VU1 memory and VIF state around the replay, and route its GS output to the second
  layer.
- **Cost:** EE 0; VU1 x2 (+3-5 ms per frame); GS thread x2.
- **Risk:** medium-high. MTVU threading, per-variant tables, and GS state snapshots. Its only gain over (d) is shading
  in VU1 programs that use the eye (none found so far).

**(c) Alternate eyes.**
- **Change:** left eye on even frames, right on odd.
- **Cost:** about 0, but each eye shows 15 frames a second, and the eyes see different moments of anything that moves.
- **Risk:** comfort. Dominated by (d), which costs the same.

**(d) Per-eye geometry in the GS (recommended).**
- **Change:** the multiview draw already rasterizes every draw into two layers. Each layer's vertices go through that
  eye's map C_i (view and projection) in the vertex shader, and the result is presented as a projection layer.
- **Cost:** a few vertex-shader instructions. EE, VU1 and GS-thread costs are unchanged.
- **What stays centre-eye:** the game's per-object culling and LOD; VU1 shading; the guard-band clip. All are
  invisible at a 3 cm eye offset if the game FOV covers both eyes. Cutscene and 2D draws stay mono as today.
- **Fallback:** (b), if a view-dependent effect proves visible.

## 6. Phase 2 plan, for (d)

1. **`VR::EyeProjection` (new, pure, unit-tested in `vr_selftests`).**
   - From the game's screen matrix (`fx`, `fy`, `cx`, `cy` from `0x2771A0`), the eye pose relative to the render head
     pose (rotation and translation, in cm via `unitsPerMeter`), and the eye's FOV tangents, it builds the per-eye
     constants: R_i, t_i and the eye's intrinsics in the target's GS pixels.
   - Tests: a pure x shift gives `fx e / w`; identity is a no-op; a canted eye; asymmetric tangents; w' <= near.
2. **Hand-off.** `CameraDriver` (EE thread, at vsync) reads `0x2771A0` with `viewMatrix` and attaches it to the render
   pose `TrackRenderPose` already keeps. The GS thread takes the shown frame's pose and intrinsics, as DepthExport does.
3. **`GSRendererHW::DetermineVSConfig`.**
   - A new `vr_map_mode = 3` with two eye constant blocks in `VSConstantBuffer`, added at the end; the other
     backends ignore them.
   - Only draws whose Z fit applies, or perspective STQ draws, are transformed. FST and constant-Q draws stay mono.
   - `m_r` and the dirty rects are widened by the largest disparity.
4. **`tfx.glsl`** (both the plain and the `VS_EXPAND_SPRITE` paths):
   - camera point `p = ((X-cx) w/fx, (Y-cy) w/fy, -w)`, then `p_i = R_i p + t_i`, `w_i = -p_i.z`;
   - `X_i = fx_i p_i.x / w_i + cx_i`, the same for Y;
   - `Q_i = Q w / w_i`, `S_i = S w / w_i`;
   - `z_i` from `1/w_i` through the fit (`z = q/beta + a0`);
   - drop the triangle when `w_i` is under 1 cm.
5. **Target and FOV.**
   - `FirstPersonFov = Auto` sets the game FOV to the union of both eyes' tangents plus a margin for the EE's culling.
   - Each layer is rendered with its eye's tangents (`cx_i`, `cy_i`, `fx_i` choose the window).
6. **`XRCompositor`:** in first person, use the projection layer `DepthLayer` already builds.
   - `views[i].pose` = the render head pose x eye i;
   - `views[i].fov` = the tangents used;
   - `subImage` = layer i;
   - depth = DepthExport's layer i (now each eye's own depth).
   - This retires the 1.5 m screen, separation, convergence, the near floor and auto orthostereo for first person; they
     stay for menus and cutscenes. `DepthWarp` remains for the 30-to-90 Hz gap.
7. **Unchanged:** the near cull, MenuPanel's split (it keys on draw kinds) and the depth export's tracker.
8. **Verify:**
   - the probe comparison generalised: eye offsets, small yaw and asymmetric tangents against the game's own renders
     from those poses (CameraDriver), expecting at most 1 px;
   - fix the sign and layer-0 findings first;
   - self-tests green;
   - the null headset (the projection layer accepted, both views);
   - then Alex in the headset: the counter and Handgun at arm's length, a zombie at 40 cm, leaning, the HUD.
9. **Later (Phase 3):** re-rasterize the last frame's GS draw list at 90 Hz from the newest pose, the same C per eye.
   This needs the frame's draws and textures kept.

The probe code (`PCSX2_VR_TRUESTEREO_PROBE`, `PCSX2_VR_PERFLOG`) is committed, off by default; self-tests 42 of 42.

## 7. Phase 2: `[VR] TrueStereo` (built 2026-10-21, off by default)

What is built (branch `truestereo`, 43 of 43 self-tests):

- **`VR::EyeProjection`** (pure; 17 self-test checks: identity, sideways shift, asymmetric corners, canted centre,
  quaternion, roll, behind the eye, degenerate input).
  - Inputs: the game camera (fx, fy, cx, cy and the 512x448 target), the headset's IPD, cant, per-eye fov tangents
    (`XRCompositor::GetEyeInfo`), and the head's roll.
  - Output per eye: the camera-to-eye map, the eye's intrinsics (its tangent window spans the target), and its OpenXR
    pose and fov relative to the camera.
- **Profile and CameraDriver.** `camera.lookAt.screenMatrix: 0x2771A0`; CameraDriver reads it every vsync and publishes the
  intrinsics (`StereoState::PublishGameCamera`).
- **The GS** (`GSRendererHW::DetermineVSConfig`, `vr_map_mode 3`; `tfx.glsl` both vertex paths).
  - On the first-person multiview target every perspective vertex is mapped through its view's eye: w from the Z fit
    (or Q), Z' through the fit, Q and S, T times w/w', so S/Q stays.
  - Flat draws (FST, constant Q) go on a plane 1.5 m ahead, sized to 90% of the view both eyes share. While MenuPanel
    splits a game screen they stay where the game drew them.
  - Full-target fills (the depth clear, fades) are left alone. The draw area is widened to the scissor.
- **The compositor** (`BuildTrueProjection`): the two eye quads go as one projection layer.
  - View i is at `camera x eye_i`: the camera is the quad's yaw/pitch at `fp_head`, the eye pose from EyeProjection with
    the render pose's roll, fov = the eye's tangents.
  - Each eye's depth is chained when DepthLayer is on.
  - No new swapchains: the eye chains are the quads' own.
- **Interactions.**
  - `PacerCopies` refuses TrueStereo.
  - `DepthWarp` is turned off by it (the warp redraws a screen image).
  - `FirstPersonFov` always takes Auto's fit, so each eye's window lies inside the game's view.
  - Hands, HUD cards and the menu panel stay their own layers.
  - The near cull is EE-side and unchanged.
  - With the switch off nothing changes: N3 below shows no per-eye layer.

Checks:

| Check | Result | Label |
|---|---|---|
| Both layers, eye 5 cm left / right (`PCSX2_VR_TRUESTEREO_TEST=pose,...`) vs the game's render from the camera moved 5 cm (view matrices from the sidecars) | 181/181 and 184/184 tiles within 1 px, mean diff 0.51 / 0.53 (frame noise 0.45) | checked offline |
| Both layers, eye 6.6 cm left turned 10 deg left / 6.8 cm right turned 10 deg right (the moved camera's exact relative pose) | 97% / 83% of tiles within 1 px, median 0 px, mean diff 2.4 / 2.1. Pixels off by > 16 are 19% / 11% in the outer sixth (outside the game's view, black by design), 1-5% elsewhere (a walking NPC, a subtitle only in one) | checked offline |
| Regression after the HUD change: eye 5 cm right | 184/184 within 1 px | checked offline |
| SteamVR null headset (90 Hz, views +-45 deg, IPD 63 mm) | projection layer accepted every frame, no xrEndFrame error. Eyes 63.0 mm apart; GS eye fx 256 px vs game fx 161. Each eye's depth chained (DepthLayer). MenuPanel on: no errors (no menu opened) | checked on the null headset |
| Cost, null headset, TrueStereo on vs off (DepthLayer on) | EE 3.9 vs 3.5 ms (noise); GS thread 2.65 vs 2.15 ms per vsync; GPU 12.8 vs 11.3 ms per frame; 100% speed both | checked on the null headset |

Not checked:
- Asymmetric or canted real headset views: the unit tests only; the null headset is symmetric.
- A real headset; the HUD text in a dump; a menu opened with MenuPanel.
- Roll in a run.
- Draws the Z fit misses: untextured draws with no depth test fall back to the flat plane.

