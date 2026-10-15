# File #1 depth export and the OpenXR depth layer (2026-10-13)

SLPM-65428 (English patch), Kevin, offline, savestate slot 7 (J's Bar, at the counter facing the Handgun). Live checks on
the portable dev install `D:\dev\PenguinScreen2-build\run-depth` (local build of branch `depthproj`, Vulkan, 3x internal
resolution). Headset-free runs used no OpenXR runtime (`XR_RUNTIME_JSON` pointing at a missing file) and
`PCSX2_VR_TEST_INPUT=1` with `pine.py`'s `FakeController` for a scripted head. Null-headset runs used SteamVR 2.17.10 with
`null_driver.py on`, restored with `null_driver.py off` (byte-identical, checked with md5). The network adapter was off,
and the game never ran faster than normal speed. Dumps and captures stayed in the install folder and the session
scratchpad; none are committed. Code: `pcsx2/VR/DepthExport.*`, hooks in `GSRendererHW` and `GSRenderer`, and the layer in
`XRCompositor`.

This is the foundation of road step 5 (`ROADMAP.md`): the depth of the frame the headset shows, correct and in metres,
handed to the runtime. It is not our own reprojection; that comes next.

## Short answer

- **Where it lives:**
  - the GS depth target at block `0x1C00`: PSMZ32, 1536x1344 (512x448 at 3x), two array layers (one per eye) in
    first-person stereo;
  - Vulkan `D32_SFLOAT_S8_UINT`, holding `z * 2^-32` (`tfx.glsl`).
- **Not at the hand-off:** File #1 draws the next frame before the shown one is presented. It draws it into the other
  colour buffer (`0x0` and `0xE00` alternate) and clears that one depth buffer first. So when frame N goes to the
  compositor, the depth target already holds frame N+1.
- **The export:**
  - the depth is snapshotted at the draw that starts changing it: the next frame's clear, a full-screen sprite into its
    own colour buffer with ZTST ALWAYS writing z = 0;
  - at the present, the snapshot whose colour buffer is the one shown is copied, laid out as the merged colour image
    and in the same GS command buffer, before the present's submit;
  - the depth layer copies it into the depth swapchain in the same submit as the colour.
- **Metres:**
  - `depth_m = 1 / (unitsPerMeter * beta * (z/2^32 - a0))`, exactly the Z-driven depth fit (`q = 1/w = beta (z - a0)`,
    R2 = 1.000), with 100 game units a metre;
  - File #1: a0 = -7.816e-8, beta = 255.998; the cleared z = 0 is the game's far plane, 500.2 m.
- **Measured** (slot 7):
  - the counter top under the Handgun's trigger volume reads **1.045 m** ahead; the volume centre is 1.040 m ahead, so
    that is +5 mm (0.5%);
  - the counter is flat at **35.1 cm** below the eye at 1.04, 1.22 and 1.46 m ahead (35.06, 35.16, 35.16 cm). That puts
    it 119.8 cm up in the game, where the trigger volume's floor is 120 cm.
- **Same frame:** with the head turning at 30 deg/s, the snapshot's depth edges line up with the shown colour at
  **0 px**, twice. Reading the live target at the present instead (`PCSX2_VR_DEPTHPROBE_LIVE`) gives +2, -14 and
  +9 px.
- **SteamVR:**
  - it offers `XR_KHR_composition_layer_depth` (the null headset too);
  - it creates `D32_SFLOAT` depth swapchains (format 126);
  - it takes the projection layer with depth on every frame for 20 s: no xrEndFrame error, nothing in `xrclient_pcsx2-qt.txt`;
  - its picture matches the first-person quads it replaces to **0 px** (correlation 0.999, both eyes).
  - **Unknown:** whether SteamVR's reprojection uses the depth. With the head still, the null headset shows the same
    picture with and without the depth (correlation 1.000).

## How the frame is drawn and shown (draw trace)

A temporary trace (six vsyncs of draws, removed again) showed one frame's GS stream at 30 fps:

| Order | Draw | FRAME | ZBUF | Test |
|---|---|---|---|---|
| 1 | the last translucent pass of the previous frame | previous buffer | `0x1C00`, no write | GEQUAL |
| 2 | the depth clear: a 512x448 sprite, z = 0 | this buffer | `0x1C00`, write | ALWAYS |
| 3 | about 100 opaque draws | this buffer | `0x1C00`, write | GEQUAL |
| 4 | about 68 translucent draws | this buffer | `0x1C00`, no write | GEQUAL |

How that stream maps onto the vsyncs:
- the vsync that shows a buffer comes after the next frame has been drawn into the other one;
- each frame is presented twice (30 fps on 60 Hz);
- the display rectangle is the whole buffer, read 1:1 (framebufferRect = displayRect = 0,0-512,448, scale 3, no y
  offset), so depth texel (x, y) is the shown image's pixel (x, y).

On the null headset the first frame after first person arms comes from a one-layer depth snapshot, before the depth
target is promoted to stereo; both eyes then take that one layer.

## The export (code)

- **The tracker** (`DepthExport::DepthTracker`, pure, unit-tested) sees every draw before it is rendered:
  - a run of depth-tested draws into one (colour, depth) pair is that frame's live depth;
  - a draw that would change that depth for another frame ends the run, and the depth target is snapshotted before
    that draw (`CopyRect` in the GS command buffer, in draw order). Such a draw is a depth-tested or depth-writing draw
    with the same ZBUF and another FRAME, or any draw whose FRAME is the depth buffer itself;
  - at the present, the shown colour target (`GetOutput`) takes the live depth if it is still the latest, else the
    snapshot labelled with it, else none.
- **The export** (`GSRendererHW::VRBeforeEndOfFrame`, right after Merge):
  - it copies the depth aspect, each layer, into a device-local buffer laid out as the merged colour image, in the GS
    command buffer, so the present's submit takes it to the GPU before `VR::EndOfFrame`'s copies read it;
  - no export (logged once per reason, the frame then goes as quads) when: not first person, no valid fit, no
    depth-tested frame in the shown buffer, two colour targets merged, or a display that stretches or offsets the frame.
- **The depth layer** (`XRCompositor`, `[VR] DepthLayer`):
  - a `D32_SFLOAT` depth swapchain per eye chain;
  - each depth image is waited with its eye and filled by `vkCmdCopyBufferToImage` in the same submit as the colour
    copy, then released with the colour under `submit_mutex` (`ReleaseAndPublish`), so a depth image never goes out
    with another frame's colour or pose. A depth image not ready, or a release that fails, sends that present as quads;
  - the published set carries the depth with its Screen entries, and keeps it with them when they are kept.

## The conversion and the range the runtime is given

The Z-driven fit (`GSRendererHW`, `VRZQDepthFit`) regresses the texture Q (1/w) of each perspective draw against its
screen Z. On File #1 every frame's draws agree: slope 5.960436e-08, intercept A = -335.2, R2 = 1.000000. So
`q = beta * (z_norm - a0)`, with a0 = A / 2^32 and beta = slope * 2^32.

The stereo already relies on q being 1/w in game units: StereoAuto puts zero disparity at `conv = d * unitsPerMeter`.
The depth in metres is therefore `1 / (unitsPerMeter * q)`. By hand, z = 20953 (a J's Bar draw, q = 0.00127 in the
trace) gives q = 0.00126887, w = 788.1 units = 7.881 m. That is a self-test check.

OpenXR reads a depth image as a projection's depth: 1/distance is affine in the stored value, from 1/nearZ at minDepth
to 1/farZ at maxDepth. The game's 1/w is affine in z too. So the raw values go to the runtime unconverted, and
`RangeForRawDepth` picks the ends:
- **The range:** the values of 500 m and 5 cm clamped to [0, 1], so minDepth = 4.9e-11 and maxDepth = 7.81e-4;
- **The ends' distances:** each end's distance is read back from its float, nearZ = 500.0 m and farZ = 0.0500 m. That
  is the reversed order the extension allows (nearZ > farZ: larger values are nearer);
- **Exact:** the runtime's reading of any stored value equals `LinearDepthM` to within 1e-5 relative, from 6 cm to
  400 m (a self-test check);
- **Outside the range:** the cleared background (z = 0, 500.2 m) clamps to the far end.

Exactness assumes |a0| is small next to the values' spread. File #1: a0 = -7.8e-8 against values of about 5e-6. A
game whose z sits near a large a0 would lose far precision in float, and the conversion would need a shader.

## The projection layer

The first-person frame is two quads (one per eye) placed at the render pose, roll-level, 1.5 m ahead. They are sized
to the frame's FOV: 100 deg vertical offline and on the null headset, 4.77 x 3.58 m. With the depth layer on, the pacer
builds one projection layer from those quads instead (`DepthExport::QuadViewForEye`, unit-tested):
- **Each view:** the quad's orientation, placed at the eye that would see the quad (`fp_head + EyeOffset(render
  orientation, -/+ IPD/2)`, the head's roll included, then turned with the screen by the stick turn), with the fov
  whose tangents meet the quad's edges from there. So every image pixel lands where the quad put it for that eye;
- **The depth:** the views share the render camera's forward axis and sit beside it, so the exported view-axis depth
  is each view's own depth.

The null headset found a bug in the first version. Its views are rolled 180 deg (orientation (0, 0, -1, 0), the left
eye at +x), which is also why its Headset Window shows the world upside down. The first version placed the eyes
roll-level, while SteamVR draws the quads from the actual rolled eyes, so the two pictures differed by +-11 px. With
the eyes taken from the full render orientation they match to 0 px. On a real headset roll is small, but it is no
longer an approximation.

## What is verified, and what is assumed

Verified:
- **built:** a local build of `depthproj`;
- **self-tests passed:** `vr_selftests` 22 of 22, with the new depth export suite at 33 checks (the conversion, the
  range, exactness, the quad-to-view geometry with 0, 30 and 180 deg roll, the tracker);
- **checked offline:**
  - where the depth lives and when it is overwritten (the draw trace);
  - the snapshot is the shown frame's (0 px against the colour with a turning head) and the live target is not;
  - the counter top and the flat-plane height cross-check, the same to 0.1 mm in two runs;
- **null-headset checked:**
  - the extension is offered and enabled only with the switch on (`xrclient` logs its extension list);
  - the depth swapchains are created and the layer is accepted with no error;
  - the picture is identical to the quads;
  - with the switch off, no extension, no depth swapchain, no depth lines.

Assumed or not checked:
- **SteamVR's use of the depth:** it is not known to use app depth for positional reprojection. Its documented
  reprojection is rotational, plus motion smoothing. Only a lean in the headset with `DepthLayer` on, against
  `PCSX2_VR_DEPTHLAYER_NODEPTH=1`, will tell.
- **Per-eye depth:** each eye's layer is drawn with the same z and only x shifted, so its depth is right for its own
  image. Below the stereo's near floor (0.5 m) the picture's disparity is clamped, but the depth still says nearer.
- **What has no depth:** HUD and 2D sprites drawn without a depth test keep the depth of what is under them, and
  translucent effects (no depth write) take the depth behind them. A runtime warping with the depth would move those
  with the wall behind.
- **Untried:** File #2, other rooms, co-op, cutscenes and menus. There the export usually just declines, and the frame
  goes as quads.
- **Not supported:** `PacerCopies` (the depth layer stays off with it), and interlace offsets. Stretched or offset
  displays decline.
- **Cost:** one full depth snapshot per game frame and one export copy per present (about 16 MB each at 3x, on the GPU),
  plus one buffer-to-image copy per eye.
  On the null headset xrEndFrame went from about 0.5 to 1.0-2.2 ms in first person with the layer. The null headset's
  timing is not evidence.

## Next step toward translation reprojection

1. **Lean in the headset**, with `DepthLayer` on, and on with `PCSX2_VR_DEPTHLAYER_NODEPTH=1`: does SteamVR move near
   things against far ones? If it does, that is free positional reprojection for the 30 fps frames.
2. **Our own warp**, whether or not SteamVR uses the depth. The pacer has each eye's depth, its render pose and the
   per-display-frame eye pose. Per display frame it can forward-warp (or mesh-warp) each eye's last image from its
   render eye to the current eye, into the projection layer it already submits. That moves the work off the runtime and
   works on any runtime. The export, the conversion and the per-eye view geometry here are its inputs. The warp also
   needs a hole fill (the edge fill's blur, or the other eye's image) and the HUD's depth.

## Our own warp: `[VR] DepthWarp` (2026-10-15, branch `eyewarp`)

**Method** (`VR/DepthWarp.*`, and `WarpCopyFrame` / `WarpPass` in `XRCompositor.cpp`):
- **The GS thread:**
  - each present copies both eyes of the frame into a slot of a ring of three compositor images (the swapchain's
    format), and this present's export buffer (both planes) into the slot's buffer;
  - one submit on the session's queue, as the swapchain copy was. It never acquires an eye swapchain;
  - the publish names the slot (`PublishedSet::fp_warp`), kept with the Screen entries when they are kept, and the
    render eyes (`fp_head` plus `EyeOffset`, turned like the screen);
  - it writes only a slot that is neither published, deferred, nor claimed by the pacer. All copies and passes are on
    one queue, ordered by barriers.
- **The pacer**, every display frame under `submit_mutex`:
  - acquires and waits each Screen swapchain (both or neither, 2 ms budget), draws the slot into it with one render pass
    per eye, then releases;
  - the pass is a 128 x 128 cell grid (98 304 vertices, no vertex buffer), reaching 6% past each edge, where the edge
    texels stretch;
  - each vertex takes the nearest depth within 6 texels and is moved from the render eye E to the display-time eye F,
    in the quad's own frame: `X = E + (Q - E) d / E.z`, `Q' = F + (X - F) F.z / (F.z - X.z)`;
  - a D16 test keeps the nearer surface where the mesh folds;
  - triangles across a depth edge stretch, so a disocclusion shows a stretched band of background, never black.
- **Translation only:** the quad stays at the render pose, so the runtime still corrects rotation. For a sideways move D
  the shift on the quad is `D (1 - 1.5 m / d)`. The far plane (the cleared z, read as 500 m) keeps its direction.
- **Fallback:** a frame without depth draws undisplaced, exactly the frame as before, and the reason is logged once per
  change. That covers not first person, a menu, the first frames, no fit, no head position and a split screen.
- **Swapchains:** no new swapchain; the eye swapchains only change owner.

**Checked:**
- **Self-tests passed:** `vr_selftests` 39 of 39 (38 before). The new Depth warp suite has 21 checks:
  - the quad frame and the round trip;
  - parallax at known depths for 10 cm sideways: the 1.045 m counter -14.0 px, a 4 m wall +20.1 px, the far plane
    +32.1 px (1536 px over 4.77 m);
  - any lean (12 cm forward plus sideways) against the direct projection;
  - the two eyes shift alike;
  - the shader's float depth reading against `LinearDepthM` (0.06-400 m, within 0.02%);
  - the grid on a synthetic counter-in-a-room plane: at an edge the footprint takes the nearer depth, and the margin
    covers a 15 cm lean.
- **Checked offline** (a CPU reference of the same per-pixel displacement on the captured slot-7 pair from 10-13,
  `depth_000361_L0.pfm` and its colour, 10 cm to the right):
  - the counter front (0.77 m) moves -31 px and the back room (7.3 m) +26 px, 57 px between them;
  - the Handgun on the counter top (about 1.05 m) moves -14 px;
  - the hanging lamp and the beer taps move against the back wall;
  - a per-pixel scatter would leave 4.6% holes, which the mesh stretches over instead.
- **Not checked:** the GPU pass in a session. Both null-headset launches failed inside SteamVR before any frame
  (`xrGetVulkanGraphicsDevice2KHR`: `XR_ERROR_RUNTIME_FAILURE`; xrclient: "failed to find VkPhysicalDevice matching
  adapter UUID"). The emulator ran flat. The settings were restored (`null_driver.py off`, `forcedDriver` gone) and no
  SteamVR process was left. So the GPU pass, the change of swapchain owner and the timing are built, not run.
- **GPU cost:** the timestamps are in place (the `(VR) Warp:` line). Estimated well under the 1.5 ms target: two eyes of
  98k vertices with 9 depth reads each, and 2 x 2 Mpx of fill. To be measured.

**Limits:**
- HUD sprites and translucent effects without depth move with the surface behind them.
- `MenuPanelRoom Freeze` goes unwarped: the frozen room has no export, so it takes the "no depth" path. Warping it needs
  the depth kept with the frozen snapshot.
- Below the stereo's near floor (0.5 m) the image's disparity is clamped, but the warp uses the true depth.
- A frame-size change re-makes the ring (a queue idle under `submit_mutex`).
- A GS device restart without a VR restart is not handled.
