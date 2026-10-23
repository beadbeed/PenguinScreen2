# Evening review, 2026-10-22 (`33f8a81..e4ca0cf`, merges `minorsC`, `losfix` and `cutscenevr`, on `nightly`)

This was a read-only review: no edits to code, no builds, no emulator runs, no headset. All line numbers refer to the
`evreview` worktree (`D:\dev\PenguinScreen2\.claude\worktrees\evreview`) at `e4ca0cf`.

What was reviewed:
- `losfix`, the whole path:
  - `HeadInWall` (`FaceCounts`, `SegmentBlockedBy`, `RefreshGrid`, `LineBlocked`, `CharacterChest`, the grid cap);
  - its two callers, HandPickup (sight mask, head point to search point) and GunRay (shot mask, chest to VR muzzle);
  - cave I's new wall shot (`GunRay::WallShot`, then `ShotFrame` with the game's back offset), checked against every cave
    rule.
- `cutscenevr`, the whole path:
  - the profile's `camera.cutscene` and CameraDriver's decision;
  - `CutsceneView` (pure maths, self-test);
  - the request/used hand-off in `StereoState`;
  - GSRendererHW's per-eye map for cutscene draws;
  - the compositor's window: the present, `BuildPublishedSet` and its kept/deferred sets, `BuildSubmitArray` and
    `BuildTrueProjection`;
  - the transitions (status 2/4 edges, movies, a state load mid-cutscene, the setting turned off at runtime).
- `minorsC`, read for regressions: `RegionFreeOrOurs` and its three users, `FlashEvidence`, `ClearLoadedBlocks`,
  `ScriptedShotCameraOnly`, the GameText DATE exclusion, VrPresence's `r == base`, HeadInWall's cap and dim ease.

Anything marked **unverified** was reasoned from the code, the research notes or PCSX2's own sources, not shown at
runtime.

**Counts:** critical 0, major 0, minor 11.

---

## Critical

None.

## Major

None.

---

## Minor

### m1 (line of sight; unverified): when the room can't be read, the test passes without a word in the log, for both the hand and the gun
- **Where:** `HeadInWall.cpp:523-534` (`LineBlocked` returns `known = false` on `Refresh::NoHeader` or `BadGrid`, with no
  log); `HeadInWall.cpp:559-583` (`CharacterChest` false); `HandPickup.cpp:775` and `GunRay.cpp:676`
  (`walled = lr.known && lr.blocked`).
- **What is wrong:**
  - `ReadGrid` refuses the whole of grid A for any one bad face: a face without a unit normal, more than 32 x 32 cells,
    more than 65536 list entries or 8192 unique faces.
  - Then both callers carry on as before the fix, as designed, but nothing is logged. HeadInWall's own warning ("room %u's
    wall grid does not read as expected") prints only from `Apply`, that is only with the HeadInWall switch on, and only
    for its own copy.
  - Only two rooms (J's Bar and the bathroom) were surveyed (`research/2026-10-22-line-of-sight.md` section 5).
- **Scenario:** in a room whose grid A refuses, the hand takes an item through a wall, and a VR muzzle pushed through the
  wall fires through it again (late review m6 reopened). The session log reads as if the test had passed.
- **Fix:**
  - From `LineBlocked`, log once per room and CRC when the grid cannot be read (`(VR) Line of sight: room N's wall grid
    does not read; hand and gun are not checked here`).
  - Log the same once when `CharacterChest` fails while a gun is out.
  - Have `session_report.py` count both lines.

### m2 (line of sight, cost; unverified): a grid that does not read is read again in full on every call
- **Where:** `HeadInWall.cpp:113-114` (the cache only counts when `c.valid`); `:121-127` (`BadGrid` sets
  `c.valid = false`).
- **What is wrong:**
  - After a refusal, the next call finds `c.valid` false and walks the whole grid again: up to 65536 list reads plus the
    faces.
  - This was already true of `Apply`. `LineBlocked` now adds its own copy, `s_lines`, which is called:
    - by GunRay on every input poll while a gun is out;
    - by HandPickup on every vsync while an item is highlighted.
  - So a bad room can cost three full grid walks per vsync on the CPU thread. The night review's m9 cap bounds each walk,
    not how often it happens.
- **Scenario:** a malformed grid near the cap, a gun in hand, and HeadInWall on. Several ms of guest reads each vsync, on
  the thread that runs the EE. Emulation could fall below full speed in that room. Slower, not faster, but still a timing
  change.
- **Fix:** after a refusal, keep the headers and the CRC and mark the cache "known bad". Read again only when a header,
  the room or the CRC changes, or after the usual 120 calls.

### m3 (cave I wall shot; unverified in game): the wall shot's origin has no bounds; it can land behind the character, or right on the wall
- **Where:** `GunRay.cpp:113-128` (`along = max(0, t * len - kWallShotBefore)`); `ShotFrame` `:106`
  (`origin = point + back * dir`); `back` from `:636-646` (0 if it does not read; accepted from -200 to +50).
- **What is wrong:**
  1. **Behind the chest.** The game's back offset is about -35 cm (research: the wall at 83 cm from the chest gives an
     origin 47 cm along). With the wall closer to the chest than about 36 cm, the origin is behind the chest. The game's
     own origin, behind its own muzzle 40-60 cm in front of the chest, is always in front of it.
     - **Scenario:** Kevin stands 25 cm from a wall and the VR gun is pushed through it. The capsule starts 11 cm behind
       his chest and runs forward through his body to the wall.
     - A zombie right behind him, whose hurt sphere overlaps the capsule's start, can take a shot that "went into the
       wall".
     - In a narrow corridor, the origin can also sit behind the wall at his back, where the one-sided faces do not cut
       it.
  2. **On the wall.** The 1 cm is measured along the chest-to-muzzle line, not along the face's normal. At a grazing angle
     that is much less: at 10 deg it is about 1.7 mm from the plane.
     - With `back` 0 (its read failed: `back_ok` false), the capsule starts within its own radius of the wall.
     - Whether the game's wall test at `0x5AB154` cuts a capsule that starts inside its radius is unverified. Many such
       tests skip a start that already overlaps.
     - Also, any `back` above +1 (the code accepts up to +50) puts the origin beyond the wall, which is the old
       shoot-through.
- **Fix:**
  - Put the start a fixed distance before the wall along the face's normal: more than the capsule's radius, for example
    10 cm.
  - Use `min(back, 0)` for a wall shot.
  - Clamp the origin so it is never behind the chest: `along + back >= 0`. If it would be, start at the chest.

### m4 (rule: free RAM checked zero-or-ours): after a state load, the new free-RAM checks are skipped for the gun block, the gun object block and the fist word
- **Where:**
  - `GunRay.cpp:516`, `:519` (`OnStateLoaded`: `s_armed = s_addr != 0`, `s_obj_armed = s_obj_addr != 0`);
  - `PhysicalMelee.cpp:2908` (`s_fist_armed = s_fist_addr != 0`);
  - the checks are `s_armed || RegionFreeOrOurs(...)` (`GunRay.cpp:653`), `s_obj_armed || ...` (`:729`) and
    `s_fist_armed || ...` (`PhysicalMelee.cpp:3315-3316`).
- **What is wrong:**
  - In a session that has armed once, every later state load sets "armed" without reading the RAM, so the next write
    skips `RegionFreeOrOurs`.
  - `ClearLoadedBlocks` (`GunRay.cpp:441-455`) zeroes only a block that reads our magic. A block holding foreign words is
    neither cleared nor checked, and is then written over.
- **Scenario:** a state from another build, a cheat or a patch that uses 0xFEB80, 0xFF500 or 0xFEBF0 is loaded after a
  shot or a punch this session. Our block is written over its words. The risk is low, because the research found these
  ranges zero in every File #1 state, but the house rule is broken on that path.
- **Fix:** in both `OnStateLoaded` hooks, set armed only when the loaded block reads our magic. Otherwise leave it
  unarmed, so the next write goes through `RegionFreeOrOurs`.

### m5 (CharacterChest; unverified): two pointer reads skip the address mask and the bounds the function checks
- **Where:** `HeadInWall.cpp:562-569`.
- **What is wrong:**
  - `in_ram` checks `(p & 0x1FFFFFFF)` up to `+0x40`, but:
    - `memRead32(record + 0x34)` and `memRead32(model + 0x84C)` use the unmasked pointer;
    - the second read is 0x84C past a pointer checked only to +0x40.
  - A garbage `model` (the record mid-change), such as `0x4xxxxxxx` or one in the last 2 KB, passes the check but is
    unmapped in the EE's TLB. `memRead32` then goes to `vtlb_Miss` (`vtlb.cpp:481-508`):
    - with the interpreter, it raises a TLB exception in the EE from host code;
    - with the recompiler, it logs;
    - with `PauseOnTLBMiss`, it pauses the VM.
  - Bone 11 is not checked against the model's bone count (`model + 0x816`), as cave U and VrBody do.
  - Offline only, because GunRay's caller is gated.
- **Fix:**
  - Read both pointers with `ReadGuest` (masked and bounded), and bound `model + 0x820 + 4 * 11 + 4`.
  - Require `11 < u16(model + 0x816)`.

### m6 (HandPickup line of sight; unverified): every 0x29 face is see-through, so the hand reaches through whatever else is classed 0x29
- **Where:** `HandPickup.cpp:774` (`kMaskSight`); `HeadInWall.h` (0x29 means "the player and enemies only").
- **What is wrong:**
  - The sight mask matches the game's zombie sight, which is right for the desk front in slot 7.
  - But the hand reaches up to 85 cm (`maxReach`), and the game's own search reaches about 50 cm from the body. So any
    fence, gate, grille, window or glass case that File #1 classes 0x29 can now be reached through.
  - J's Bar already has a 1 m opening of 0x29 faces in its lower wall, and section 5 asks whether that is the street door.
- **Scenario:** an item behind a locked gate or a counter grille, within 85 cm on the floor plane, is highlighted and taken
  by hand. Only two rooms were surveyed.
- **Fix:**
  - Survey 0x29 faces in the other File #1 rooms (log the 0x29 faces a refused or passed line crosses).
  - If gates or grilles are 0x29, refuse a 0x29 face whose top is above the search point plus a margin. A counter or desk
    front ends below the item; a gate does not.

### m7 (HandPickup; unverified): an item refused for the wall still shows as takeable
- **Where:** `HandPickup.cpp:788-791`. Only the state machine's `in.target_highlighted` is cleared. `ItemReach`'s
  highlight, which comes from `PickupTarget::highlighted`, is unchanged.
- **Scenario:** in the headset an item behind a wall glows as squeezable. Alex squeezes and nothing happens. The only
  explanation is a log line.
- **Fix:** feed the verdict back to the cue: `ItemReach` drops the highlight for that node while it is walled. Or show a
  one-off toast: "behind a wall".

### m8 (CutsceneStereo; unverified in the headset): the window appears and disappears without the blink everywhere except the 2 -> 4 -> 2 path through first person
- **Where:** `XRCompositor.cpp:11496-11498` (`cut_window` follows `CutsceneActive()` directly); `:11422-11426` (no mode
  switch, so no blink, when the world screen is on both sides); `CameraDriver.cpp:4789-4793` (the 250 ms lapse).
- **What is wrong:** the blink only plays with a screen mode switch. The window is not a mode, so it swaps in sight with
  the world screen in these cases:
  - **A pause (offline):** `Apply` stops, `CutsceneActive` lapses after 250 ms, and the window drops to the world screen
    (curved, its own size), showing the last per-eye off-axis frame. On resume it pops back.
  - **No first person on one side:** for example on obsrv, where first person never arms; with first person refused; after
    a state loaded at status 4 from the world screen; or at a 4 -> 3 or other exit. The window replaces the world screen
    (27-40 deg flat against the usual size and curve) with no blink.
  - **A real-time cutscene's opening:** the first 8 s of J's Bar cutscene 3 show the world screen, because nothing is
    drawn per eye yet (research section 7: "first window frame ... 8 s in"). The window then pops in at 27 deg.
- **Fix:**
  - Treat the window's edges as screen changes. Blink (`HudCards::Blink`, `kBlinkRiseMs`) before the window goes up or
    down, as the mode switch does.
  - Keep the window through the lapse while the VM is paused (`VMState::Paused`), as other state is "held as it is".

### m9 (CutsceneStereo; unverified): the window's field of view comes from a screen matrix that can be stale or one frame off
- **Where:** `CameraDriver.cpp:4126-4128` and `:2808-2819` (the intrinsics are read from 0x2771A0 at the vsync);
  `GSRendererHW.cpp:6551-6566` (each draw builds its eyes from the newest intrinsics); `CutsceneView.cpp:68-80` (the
  window's tangents come from them).
- **What is wrong:**
  - **Movies.** A movie (0x35FFFF == 1, the minimal frame 0x341AF0) does not rebuild 0x2771A0 (the profile comment
    at `SLPM-65428.yaml:277-282`). So its window takes whatever the last 3D frame left: first person's own FOV going in,
    and "FOV 100 -> 40 at the start" in slot 7. The movie's size in the headset is an accident of what came before.
  - **Lens changes.** The intrinsics are published at the vsync on the CPU thread. The GS draws the frame the EE built
    earlier. On a lens change (FOV is keyed per shot, research section 1), one frame's draws can be mapped with the next
    frame's fx. The vertices then come from the old projection while the window's tangents and the projection view come
    from the new one: a one-frame pop in scale. This is the night review's m7 class: eyes per draw, not per frame.
  - Each new FOV also resizes the window in sight. That is by design (one-to-one), and the research already lists the
    window's size as a headset question.
- **Fix:**
  - Read 0x35FFFF in CameraDriver and give a movie a fixed window: the world screen's size, or a fixed FOV.
  - Latch the intrinsics with the frame they belong to, through the same view-matrix match `TrackRenderPose` uses, or
    snapshot them once per GS frame.

### m10 (CutsceneStereo, docs and behaviour): "Read when VR starts" is true of the compositor only; CameraDriver reads the setting every vsync
- **Where:** `Config.h:1606-1613` and `Pcsx2Config.cpp:2050` ("when VR starts"); `CameraDriver.cpp:4126`
  (`cut = in_cut && EmuConfig.VR.CutsceneStereo`, live); `XRCompositor.cpp:10906-10921` (`s.cutscene_stereo`, at
  initialisation).
- **What is wrong:**
  - Turning the setting off at runtime takes effect at once. That is good, and it is checked under "Verified" below.
  - Turning it on during a session does nothing until VR restarts. CameraDriver still logs "(VR) CutsceneStereo: a
    cutscene: the game's camera in a stereo window." at every cutscene, so a session report says it was on.
- **Fix:** say "off at once, on after a VR restart" in `Config.h` and STATUS. Gate the CameraDriver log on the
  compositor's `cutscene_stereo`, or log "(not active this session)".

### m11 (CutsceneStereo, cost): two mutexes per draw outside first person
- **Where:** `GSRendererHW.cpp:6510` (`CutsceneRequest`, which locks `s_cut_mutex` for every draw on the stereo target
  outside first person: menus, the world screen, cutscenes); `:6565` (`SetCutsceneUsed`, a lock and a memcpy for every
  cutscene draw); plus `GameCamera`'s lock.
- **Impact:** uncontended, so small, but per draw on the GS thread. The request only changes at a present, on the same
  thread.
- **Fix:** take the request once per GS frame, for example at the first draw after a present, and record "used" once per
  frame.

---

## Verified (no finding)

### Online safety
- **New guest writes in the diff:**
  - GunRay's block and object block, and cave H's fist word, are unchanged in place. They are now behind
    `RegionFreeOrOurs`, except for m4.
  - `ClearLoadedBlocks` writes zeros only over a block reading our magic, and only after a state load, which the rules
    never allow online.
  - Nothing else writes:
    - `LineBlocked` and `CharacterChest` only read;
    - HandPickup's test only withholds a squeeze;
    - CutsceneStereo's CameraDriver block only reads 0x2771A0;
    - the GS and compositor parts are display only.
- **Another player's character:** nothing here writes a record. `CharacterChest` reads the local record GunRay already
  uses (`record_steady`, `slot_known`). VrPresence now skips the local record whatever its slot byte says
  (`VrPresence.cpp:1058-1060`).
- **Timing, flags, speed, pause:**
  - No new pause, speed, frame-advance or state call.
  - Cutscene durations are the same with the setting on and off: 28.49/28.50 s and 35.39/35.35 s (research).
  - The game counter 0x48BF93 stops in status 4 by itself; nothing new touches it.
- **CutsceneStereo online:** it runs online and on obsrv too. `in_cut` needs only `switched_on`, status 4 and first
  person disarmed, not `net_ok`. That breaks no rule (it changes nothing in the game, and it is not first person), but
  STATUS mentions only co-op ("same online"). Say obsrv explicitly, so the rule "on obsrv first person stays off" is not
  read as covering it.

### Cave I's 1 cm wall-shot origin against the cave rules
- **Install and restore:** no cave word, site or `camera.caves` group changes. The profile diff only adds
  `camera.cutscene`. So all-or-nothing with read-back, sites before bodies, restore before drain, and the drain are all
  untouched.
- **Focus check:** cave I still requires the block's record, the focus 0x3AEF74 and a stamp age under 7. The host writes
  the same debounced local record (`record_steady`, `slot_known`). `CharacterChest` reads that same record.
- **Offline gates:** the wall shot runs only inside `gun`, which needs `offline` (`!EthEnable && !OnlineSafe`) and
  `GunRayAim`. The site stays `offlineOnly`.
- **Free RAM:** checked zero-or-ours before the first write (`GunRay.cpp:653`), apart from m4.
- **Block contents:** the same 0x60-byte block, stamp and magic. Only the matrix and origin values differ, and
  `ShotFrame` keeps the frame orthonormal.
- **WallShot's own checks:** it refuses NaN input, `t` outside (0, 1], and a chest that coincides with the muzzle. When it
  fails, the block is left unwritten (`mapped = false`).
- **Cave O on a walled muzzle:** cave O is not armed then (`obj_ok` requires `!muzzle_walled`). `ClearObject` zeroes it
  through its magic check, so cave O falls back to the game's own gun object. Our hands' flash comes back, because
  `FlashEvidence` needs cave O's count.
- **Verdict:** the 1 cm origin keeps every cave rule except the post-load free-RAM bypass (m4). Its geometry has m3's
  bounds problem.

### Line of sight: refusals and passes
- **The old player filter is unchanged.** `FaceCounts` with `kMaskPlayer` makes the same tests as the old `BlocksPlayer`,
  only reordered: class bit, attribute bits, ignore mask, then the door rule for class 1.
- **The masks against File #1's faces match the research:**
  - 0x2F stops all three masks;
  - 0x29 stops the player mask only;
  - 0x27 stops sight and shots;
  - shut door walls (0x12F/0x22F, class 1) stop, open ones pass.
  The self-tests cover these cases, including the slot 7 desk.
- **The 0x29 furniture faces:** sight and shots pass, as for the game, so the slot 7 Handgun is no longer falsely refused
  (the reverted player-mask try). A pass through a 0x29 opening is what the game's own sight does. See m6 for what else
  0x29 may be.
- **A muzzle exactly on a wall:** `SegmentHitsFace` needs `da >= 0` and `db < 0`, so a muzzle on the plane (`db = 0`) is
  not walled.
  - The normal shot then starts `back` along the VR aim.
  - Aimed into the wall, the capsule meets the face from the front, and the game cuts it there.
  - Aimed into the room, the origin lies behind the plane and the capsule crosses the face from behind, uncut, but it
    heads into the room. That is harmless.
  - A muzzle just past the plane is walled, with `t` near 1.
  - So there is no false pass at the plane.
- **A hand pushed through a wall over an item's volume that extends past the wall:** refused. That is correct: the search
  point is the hand's.
- **A grid that fails to read:** unknown, so the test passes, as designed. See m1 (silent) and m2 (cost).
- **Threads:** `s_lines` has two users, and both run on the CPU thread, so there is no race:
  - `GunRay::Poll` (from `VRInputSource::PollEvents`, through `InputManager::PollSources` in `VSyncOnCPUThread`);
  - `HandPickup::Poll` (from `GameFeedback::Poll`, `VMManager.cpp:2903`).
- **Grid cap:** one total of 65536 entries, counted across cells (`HeadInWall.cpp:347-349`). The self-test covers it. The
  header-fail return now eases the dim (night review m9).

### Cutscene transitions
- **2 -> 4:**
  - The camera guards include status == 2, so first person disarms after `disarmAfterVsyncs: 6`.
  - `in_cut` requires `!armed`, so the window and first person never overlap.
  - The switch to the world screen blinks and anchors at the head (`XRCompositor.cpp:11449-11461`). The window goes up
    under that blink.
- **4 -> 2:**
  - First person arms on the vsync status reads 2.
  - The window holds while `fp_switch_due_ms != 0`. That is set in the same present, because the mode logic runs before
    `cut_window`.
  - For the blink's 40 ms rise, the GS draws with `first_person` (no window map) while the compositor still shows the
    frames through the window with the last eyes. That is under a rising blink and at most 40 ms: acceptable.
- **Movies:**
  - Status 4 covers both kinds.
  - Their frames are flat draws, and at the window's plane the per-eye map is the identity
    (`CutsceneView.cpp:145-162`).
  - Full-target fills stay unmapped, and `vr_stereo` is zeroed in both branches (`GSRendererHW.cpp:6633-6644`), so no old
    world-screen separation leaks in.
  - The size of a movie's window is m9.
- **A state load in the middle of a cutscene:**
  - CameraDriver decides from the loaded status on the next vsync.
  - The request is the compositor's and lives only in host memory; nothing of the window is in game RAM.
  - A load from first person re-anchors through the switch. A load from the world screen puts the window where that
    screen is (File #1's profile has no `followHead`, so the screen is world-locked anyway).
  - GunRay's and PhysicalMelee's load hooks are unaffected (but see m4).
- **The setting turned off at runtime:**
  - `CutsceneActive` goes false on the next vsync, so `cut_window` is false.
  - The request is cleared at once (`SetCutsceneRequest({})`), the used eyes are drained, and `cut_shown_valid` goes
    false.
  - The GS stops mapping on its next draw, and the projection layer falls back to the quads.
  - At most one already-drawn per-eye frame is then shown as plain quads.

### The renderer: resource lifetime and layers
- **No new resources:** no new swapchain, image or GPU resource. The window's projection views reuse the subImages of the
  world screen's two eye quads (`BuildTrueProjection`), so they live exactly as long as the quads do (the existing
  swapchain teardown).
- **First person never inherits the cutscene's eyes:**
  - `present_true.valid` is reset in the first-person branch every present (`XRCompositor.cpp:11618`).
  - `present_cut` is cleared before use every present (`:11501`).
  - `fp_true` is published only with `fp_screen || present_cut` (`:6840`, `:12158`).
- **Kept and deferred sets:**
  - A kept screen (`keep_screen`) carries `prev.fp_true` with `prev`'s own entries, so a kept frame shows with the eyes it
    was drawn for.
  - A deferred set captures `present_cut` when it is built.
  - The pacer's turn is 0 for any set that is not first person.
  - The window is never curved or head-following (`:11677`, `:11700`).
- **Fallbacks:**
  - A mono world screen (one quad for both eyes) makes `BuildTrueProjection` refuse, and the quads go as before.
  - With PacerCopies on, the window is off and says so.
- **The eye hand-off:** the compositor sets the request at each present on the GS thread, so all of a frame's draws share
  one request (apart from m9's intrinsics). `TakeCutsceneUsed` returns the eyes of the latest frame drawn since the last
  take. A repeated frame keeps the previous eyes.

### minorsC
- **`RegionFreeOrOurs`:** a zero magic never counts as ours, and unreadable memory is not free. Its three users check it
  before the first write, apart from m4.
- **`FlashEvidence`:** both threads use the same steady clock, and the 100 ms and 150 ms windows match the description.
- **`ScriptedShotCameraOnly(4)`:** keyed to the one `alsoEquals` guard (0x3AEF75), so it cannot fire on another guard's
  value.
- **The GameText DATE exclusion:** correct for the late review's m7 (the DATE setup belongs to the unmoved box).

---

## Fixed on evfix

Branch `evfix` (from `evreview` 42f53f4), one commit per finding, not pushed. Every finding was checked against the code
first; none was refuted. Self-tests: `vr_selftests`, 63 suites, all PASS (verified (offline)). Offline checks: `run-evfix`,
PINE slot 28140, no OpenXR runtime, normal speed, `HandPickup = true` in that install only, savestates copied from
`D:\Games\PS2\PenguinScreen2\sstates`. Slot 7: the Handgun taken by hand (`pickuptest.py`, cell 0 holds record 3; checked
offline).

| Id | Verdict | Commit | What changed | Evidence |
|---|---|---|---|---|
| m1 | fixed | 30d097a | `LineBlocked` warns once per room and game when the wall grid does not read ("(VR) Line of sight: room N's wall grid does not read ..."); GunRay warns once while the chest does not read with a gun out; `session_report.py` counts both ("line of sight not checked"). | report: synthetic log (checked offline); no false warning in J's Bar in any run below (checked offline) |
| m2 | fixed | 55aa807 | A refused grid keeps its headers, room and CRC and is marked known bad (`CachedVerdict`): read again only when one of them changes or after 120 uses. | self-test (verified (offline)) |
| m3 | fixed | add57c3 | `WallShot` returns the origin itself: 1 cm plus `min(back, 0)` before the wall, at least `kWallShotGap` (10 cm) off the plane along the face's normal (`LineResult::normal`), never behind the chest. `ShotFrame` gets back 0 for a wall shot. | self-test: back 0, positive back, a wall 25 cm away, 10 deg grazing (verified (offline)); slot 5: the walled pose twice no hit (600 -> 600), cave O not armed; head, offset, head hit 600 -> 150 (checked offline) |
| m4 | fixed | 0270a86 | `OnStateLoaded` keeps the gun block, the gun object block and cave H's word armed only while the loaded RAM reads our magic (`PhysicalMelee::ArmedAfterLoad`); otherwise the next write goes through `RegionFreeOrOurs`. | self-test (verified (offline)); slot 5: five loads in one session after shots, every case written and counted, no refusal line (checked offline) |
| m5 | fixed | 5071274 | `CharacterChest` reads through `ChestFrom` (pure; live reader `ReadGuest`, masked and bounded) and needs bone 11 below the model's bone count (`model + 0x816`). | self-test (verified (offline)); slot 5: the chest read in the walled cases (checked offline) |
| m6 | left, documented | 80cd538 | No rule: only two rooms surveyed, no `0x29` gate or grille seen, and the suggested height rule would refuse J's Bar's `0x29` opening that the game's own sight passes. `research/2026-10-22-line-of-sight.md` section 5 says what would settle it. | unverified |
| m7 | fixed | c4904ed | HandPickup feeds the wall verdict back (`ItemReach::SetPickupWalled`); `ApplyWalled` shows that target walled, not highlighted (no squeezable glow, no tick); it stays tested so it returns when the line clears. | self-test (verified (offline)); slot 3 behind the wall: highlighted once, then dropped ("item left") and held down for 9 s of squeeze, cells 0 0 0 0; in front: taken (cell 0 holds record 4) (checked offline) |
| m8 | fixed | 31ca45d | The window's edges are screen changes (`CutsceneView::StepGate`): at once under a blink already black or when its screen is gone, else after a blink of its own; shown only once its frames are there; held while the VM is paused. | self-test (verified (offline)); not seen on a runtime (no headset, no null headset here) |
| m9 | left, documented | c5b434b | A movie's lens and the per-draw intrinsics: needs a profile key and the view-matrix latch. `research/2026-10-22-cutscene-vr.md` section 6. | J's Bar cutscene 3: the first per-eye draw mapped with fx 161.1, first person's lens (checked offline) |
| m10 | fixed | edf44a9 | The compositor publishes whether it took the window on; CameraDriver's line says "not active this session" when it did not. Config.h, Pcsx2Config.cpp and STATUS: off at once, on after a VR restart; STATUS says it runs on obsrv. | J's Bar cutscene 3 without a runtime: "a cutscene (not active this session ...)" (checked offline) |
| m11 | fixed | 41a1758 | A draw outside the window reads one atomic; inside it the request is copied once per new request (a sequence number) and "used" is recorded only when it changed or was taken: about once per frame. | self-test: the hand-off (verified (offline)); J's Bar cutscene 3 with `PCSX2_VR_CUTSCENE_TEST`: draws mapped per eye, status 4 for 37.8 s in the log, first person ARMED as it ended (checked offline) |
