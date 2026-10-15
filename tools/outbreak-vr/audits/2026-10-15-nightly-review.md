# Nightly review, 2026-10-15 (`headset1..nightly`, 48e8f52)

This was a read-only review: no edits, builds, emulator runs or SteamVR. All line numbers refer to the `nightly` worktree.
The coordinator set a time budget of about 40 minutes and an order of priorities. I spent the review where the risk
was, and did not read all 23k lines:
- every guest-memory write (only `CameraDriver.cpp` and `PhysicalMelee.cpp` add any);
- the silence and cave machinery;
- the melee host block and the strike/jolt paths;
- the DepthWarp pacer path.

Not reviewed in depth: MeshRaster/HandModel parsing, MenuPanel's GS split, and most of DepthExport. Anything marked
**unverified** was reasoned from the code but not shown at runtime.

**Counts:** critical 0, major 4, minor 7. Lead 2 was refuted.

---

## Leads from the first pass

### Lead 1: `PhysicalMelee.cpp:2411` reads `0x48BF7D` directly (confirmed, becomes M1)
`slot` is read in the same vsync handler as `CameraDriver::Apply`'s `GetBase` (Counters.cpp:417 Apply, then :424
PollInputOnCPUThread). The EE does not run in between, so `slot` and `record` always agree. If one has a partner's
value, so does the other, which makes it worse rather than safer. See M1.

### Lead 2: the `s_local_record_valid` / `s_local_record_base` race (refuted)
- **Writer:** `CameraDriver::Apply` (VMManager.cpp:2888, `VSyncOnCPUThread`).
- **Readers:** `PhysicalMelee::Poll`, `ReloadDriver`, `BeltPouches`, `GameFeedback` and `HudCards`. All of them run on
  the CPU thread: from `PollInputOnCPUThread` / `IdlePollUpdate`, or right after Apply in `VSyncOnCPUThread`.
- **Result:** no cross-thread access, so no torn read. Only the PINE telemetry reader is off-thread, and it reads
  `GetTelemetry()`, not these statics.

---

## Major

### M1 (major): the offline local-slot flicker can make a partner's record "local" for a vsync, and melee then writes for it
- **Where:**
  - `pcsx2/VR/CameraDriver.cpp:805-880` (`GetBase`, indexed base);
  - `pcsx2/VR/CameraDriver.cpp:3488-3490` (published as `LocalRecord`);
  - `pcsx2/VR/PhysicalMelee.cpp:2411-2414`, `:2541-2543`, `:2711` (block +0x08 = record);
  - `pcsx2/VR/PhysicalMelee.cpp:1525-1534` (contact strike);
  - profile `SLPM-65428.yaml:242` (`idOffset: 0x03`).
- **What is wrong:**
  - Offline, `0x588C30` sets `0x48BF7D` to 1, then 2, while it runs the AI partners' task lists, then puts 0 back
    (`research/2026-10-13-near-cull.md`, about 0.3% of reads).
  - The identity check `record +0x03 == idx` is circular. Partner record k carries id k, so it passes when the index
    byte itself reads k. That research note already flags this as an open risk.
  - In the same vsync, melee's own `slot` read agrees with the partner. The owner check (`owner == slot + 1`) therefore
    passes whenever that partner holds a melee weapon.
- **Failure scenario (offline):** the vsync lands inside `0x588C30` while partner 1 (Mark/George) holds a pipe.
  1. The host block names partner 1's record.
  2. Caves A/B/C act on that record's attack object: its pose becomes the VR weapon's, and its hit window opens at frame 2.
  3. In contact mode, a strike that poll makes cave D spawn the attack as the partner.
  4. CameraDriver's own camera and record ops also write the partner's record for that vsync. That part is older than
     this branch, but it is the same root cause.
  - It is rare (about 1 in 300 vsyncs), but it defeats the only guard the "local record only" design relies on.
- **Online:** the research says `0x588C30` runs only while `0x1AF4D0()` reports offline, so the other-player case is
  not reached. That is a static claim, **unverified live**.
- **Suggested fix (any one):**
  - Offline, accept only idx 0 (the local slot is 0 offline; `0x588C30` restores 0).
  - Debounce: accept an index only after it reads the same on 2 or more consecutive vsyncs, and re-read the index byte
    after the record checks.
  - In `PhysicalMelee::Poll`, refuse to write the block or a strike when `record` differs from the previous poll's
    record.

### M2 (major, unverified trigger): a code cave can be entered half-written
- **Where:**
  - `bin/resources/vr-profiles/SLPM-65428.yaml:330-344` (sites; guard = only the cave's **first** word);
  - the cave-body entries from `:345` (and caves C to G after them);
  - `pcsx2/VR/CameraDriver.cpp:1185-1232` (`ApplySilence`);
  - `pcsx2/VR/VRProfileDB.cpp:1411-1421` (bad entries are dropped one by one);
  - `pcsx2/VR/PhysicalMelee.cpp:2683-2716` (block writes).
- **What is wrong:**
  - Each cave word is an independent silence entry. A word holding neither its `off` value (0) nor its `on` value is
    "left alone".
  - A site is patched once its cave's first word reads `on`, whatever the rest of the cave holds.
  - The parser drops a single malformed or unknown entry and keeps the rest of the group.
  - Offline there is no `RegionZero` check before the caves or the 0xFF000 block go in. Only the co-op path checks it,
    at `PhysicalMelee.cpp:2449-2461`.
- **Failure scenario:** something leaves a non-zero word in 0xFE000-0xFE8C0, for example:
  - a game revision or overlay that uses that RAM;
  - a savestate made by another build with a different cave layout;
  - a profile edit that drops one cave line.

  The site's `jal`/`j` then goes into a cave with a foreign word in it. The EE runs garbage on the next melee frame:
  a crash, a hang, or a wrong branch into game code. Disarm restores sites and caves together, so the risk is only
  while armed.
- **Suggested fix:**
  - Treat a cave and its sites as one group: patch the sites only when **every** word of the cave reads `on`.
  - Restore the whole group if any cave word stops matching.
  - Run `RegionZero` over the caves and both blocks offline too, before the first install.
  - Make the parser drop the whole `whenSetting` group if one of its entries is rejected.

### M3 (major, unverified at runtime): DepthWarp, eye swapchains destroyed while a pacer warp pass may still render into them
- **Where:**
  - `pcsx2/VR/XRCompositor.cpp:1167-1199` (`DestroySwapchains`: `WaitAllFences()` waits only the GS's `s.fences`);
  - `:1111-1134` (`DestroySwapchainSynced` takes `submit_mutex` but does not wait for GPU work);
  - `:9280-9295` (the warp pass submits with `wp.fence[ci]` and leaves under `submit_mutex` with the GPU work in flight).
- **What is wrong:** with DepthWarp on, the pacer renders into the eye swapchain images. When the GS recreates those
  swapchains (`EnsureSwapchains` on a frame-size change), it waits for its own fences and for `submit_mutex`. It does
  not wait for `wp.fence[]`, so `xrDestroySwapchain` can free VkImages the GPU is still writing.
- **Failure scenario:**
  1. The GS output size changes: the game switches display mode, or the user changes upscaling with DepthWarp on.
  2. A warp pass was submitted one display period earlier.
  3. The runtime destroys the images under it, causing `VK_ERROR_DEVICE_LOST` or a driver crash.
- **Suggested fix:** in `DestroySwapchains` (and `DestroySwapchainSynced` for eye chains), wait on every in-flight
  `wp.fence[]`, or call `vkQueueWaitIdle` under the queue lock. Do it while holding `submit_mutex`, and drop the
  matching WarpTargets (M4) there.

### M4 (major, unverified: depends on the runtime reusing handles): WarpTarget is keyed on the XrSwapchain handle value and never invalidated
- **Where:**
  - `pcsx2/VR/XRCompositor.cpp:8802-8806` (lookup by `swapchain`, `width`, `height`);
  - `:9326-9336` (Screen entries kept when a target with that handle has `released`);
  - `WarpDestroyTarget` is called only from `WarpTargetFor` and `WarpDestroyAll`.
- **What is wrong:** after `DestroySwapchains` and `CreateChain`, the old WarpTarget keeps:
  - its `images` and `fbs` (views of destroyed VkImages);
  - its `acquired` / `waited` / `released` flags.

  If the runtime hands back the same handle value at the same size (SteamVR's handles are pointers, so reuse after
  free is plausible), `WarpTargetFor` returns the stale target.
- **Failure scenario:**
  - A framebuffer over a destroyed image is drawn into, which can crash.
  - `acquired == true` skips the acquire and `LockedRelease` releases an image never acquired
    (`XR_ERROR_CALL_ORDER_INVALID`).
  - `released == true` sends the new swapchain in `xrEndFrame` before any release, which is a layer error and dropped
    frames.
- **Suggested fix:** in the eye-chain destroy path, under `submit_mutex`, call `WarpDestroyTarget` on every target
  whose `swapchain` matches. Alternatively, keep a generation counter in the chain and the target, and compare it as
  well as the handle.

---

## Minor

### m1 (minor): Press mode holds Circle without re-checking that the weapon is still raised
- **Where:** `pcsx2/VR/PhysicalMelee.cpp:245-260` (Pressing phase) and `:290`
  (`hold_attack = Pressing && phase_s < press_s`); `usable` at `:2571` counts the lowered ("normal") stance as usable.
- **What is wrong:** Pressing starts only when the weapon is raised and in aim idle. If the player lowers it inside
  `press_s` (they raised it with their own bumper, so `we_raise` is false), Circle stays held in the normal stance.
  In that stance Circle is search, pick up, or open door.
- **Scenario:**
  - A swing just as Alex lets go of the bumper, next to an item, starts the pickup and its "Will you take it?" prompt.
  - It cannot confirm the prompt: the press began before the prompt, and the screen check ends the hold.
    **Unverified** that the game needs a fresh press.
  - It is still an unrequested action.
- **Fix:** in Pressing, `finish(Result::Lost)` when `!in.raised`, and gate `hold_attack` on `raised`.

### m2 (minor): the co-op cave gate lags the room-master decision by one vsync
- **Where:** `CameraDriver.cpp:1172-1183` (`SilenceSettingGateOpen` → `SilenceSettingCoopLive` →
  `PhysicalMelee::CoopOverrideLive()` = `s_coop_live`), which is set in `PhysicalMelee.cpp:2463`. `Apply` runs before
  the input poll (Counters.cpp:417 then :424).
- **What is wrong:**
  - When the master moves to another console, the caves stay in for one more vsync.
  - If `VRInputSource` stops reaching melee (`m_instances.empty()` return, `VRInputSource.cpp:847`), `s_coop_live`
    keeps its last value indefinitely.
  - The stale stamp (more than 6 frames) makes the caves fall back to stock code, and they act for the local record
    only. So this is not a rule break, just one or more frames of a non-master console applying the VR hit locally.
- **Fix:** clear `s_coop_live` in `Apply` whenever the mode inputs are not fresh: use a poll timestamp, and treat it
  as false when the last poll is older than 2 vsyncs.

### m3 (minor, unverified): the partner-swing end message can be lost
- **Where:** `PhysicalMelee.cpp:2697-2698` (`b + 0x30` frames) and cave F (profile).
- **What is wrong:** cave F sends the swing's start and, 0.5 s later, its end. If the caves are restored inside that
  0.5 s (disarm, a door, master change, switch off), nothing sends the end.
- **Scenario:** partners see Alex's character stuck in the swing pose until the game's next state message.
  Cosmetic, online only.
- **Fix:** have the host send or schedule the end itself, or delay the cave restore until `b+0x30` reads 0.

### m4 (minor): the warp pass can release an image it never drew
- **Where:** `XRCompositor.cpp:9283-9292` ("Released even when nothing went in").
- **What is wrong:** when recording or submission fails (`ok == false`), the acquired index is released anyway. The
  runtime then shows whatever that image last held: an older frame, which looks like a backwards jump.
- **Fix:** on failure, record a clear or a copy of the previous frame, or keep the image acquired and retry.

### m5 (minor): the warp depth attachment is shared across up to 3 in-flight passes with no dependency between them
- **Where:** `DepthWarp.cpp:439-446` (UNDEFINED → CLEAR, no external subpass dependency); one `t.depth` per target.
- **What is wrong:** successive passes on the queue write the same depth image without synchronisation, a
  write-after-write hazard. The sync validation layer reports it; in practice it is likely harmless.
- **Fix:** add an external subpass dependency for early/late fragment tests, or one depth image per command buffer.

### m6 (minor): `WarpTargetFor` destroys framebuffers after a bounded wait
- **Where:** `XRCompositor.cpp:8820-8823`.
- **What is wrong:** it waits at most 100 ms per fence, ignores a timeout, and then destroys views and framebuffers
  that may still be in use.
- **Fix:** check the result and give up (return `nullptr`) on a timeout.

### m7 (minor, behaviour and docs): SeamlessDoors' arrival words stay patched whenever VR and HeadCamera are on, not only while first person is armed
- **Where:** `CameraDriver.cpp:3290-3291` (`ApplyKeptSilence(cam, crc, switched_on)`, where
  `switched_on` = VR enabled, HeadCamera on and not split screen).
- **What is wrong:** the arrival shot is suppressed for every room entry offline, even when first person is disarmed
  for long periods (a cutscene arrival, or a scripted shot that keeps it off). The STATUS wording "first person is
  turned off" means the switch, not the armed state.
- **Fix:** document it as is, or gate it on "armed within the last N seconds or loading" if the stock arrival shot
  should come back when first person is not in use.

---

## Checked and found sound

### Online and safety gates
- **Pause, speed and states:** the diff adds no pause, speed, frame-advance, or load/save-state call.
- **Who writes guest memory:** only `CameraDriver.cpp` (silence apply/restore) and `PhysicalMelee.cpp` (block, strike,
  jolt) add writes. ItemReach / HandTargeting, MenuPanel, ReloadDriver, GameFeedback, ZoneMarkers, BeltPouches,
  MeshRaster, HandModel, DepthExport, DepthWarp and XRCompositor add none.
- **MenuPanel:** it presses no confirm.
- **ReloadDriver:** it drops L1 when a screen comes up (self-test at `ReloadDriver.cpp:612-621`).
- **`whenSetting` gate (`CameraDriver.cpp:1172-1183`):**
  - the switch must be on and online-safe off;
  - with the adapter on, only private co-op, and only for `PhysicalMelee` / `PhysicalMeleeContact` while this console
    is the room's master;
  - `SeamlessDoors` and `MeleeHitJolt` are never allowed online (`VRProfileDB.cpp:5155-5182`);
  - an unknown `whenSetting` name drops the entry at parse time (`VRProfileDB.cpp:1411-1421`).
- **Melee `Decide` (`PhysicalMelee.cpp:1660-1667`):**
  - Off unless the switch is on;
  - online, Off unless `PhysicalMeleeOnline` is on, the session is private co-op and online-safe is off;
  - caves only as the master and only with the free RAM read zero.
- **Hit jolt:** strictly offline (`PhysicalMelee.cpp:2721-2724`), and its block is cleared otherwise.
- **Partner swing:** sent only in `CoopFull`, or with the offline test switch.

### Restore paths
- **Silence restore** (`RestoreSilence`, `ApplySilence` inactive branch):
  - only words that read `on` and that we patched (or that are under repair after a load);
  - a `when` failure (overlay gone) forgets the patch;
  - sites come before caves in the profile, so the same pass restores both.
- **`keepThroughDisarm`** (`ApplyKeptSilence`): restored on switch off, adapter on, online-safe, VR or HeadCamera off,
  and on a savestate load (repair). Forgotten on overlay change, and cleared on VM stop (`:3226`).
- **Host block and jolt block:**
  - the caves ignore the block unless magic, record and a stamp within 6 frames all match;
  - the block is zeroed whenever it is not written (`ClearBlock`);
  - after a state load the host re-arms from zero.

### Parsing, bounds and pacer use
- **Melee reads:** go through `GuestReader` with main-RAM bounds; table indices are capped (`enemy_count`, `type < 0x100`,
  sphere `zone < count && < 8`, `bone < jolt_bone_count`).
- **Jolt parser:** rejects bones past the count, lists of different lengths, and a misaligned or out-of-RAM block or hook.
- **Depth to metres** (`DepthExport.cpp:42,67`, `DepthWarp.cpp:34`): guarded against q ≤ 0, NaN and beta = 0.
- **DepthWarp ring:**
  - a slot is never the published, deferred or pacer-claimed one;
  - GS ring copies and pacer passes share one queue, and the GS barrier (ALL_COMMANDS → TRANSFER) orders a slot's
    rewrite after earlier reads;
  - render pass final layout COLOR_ATTACHMENT_OPTIMAL;
  - acquire/wait/release on the eye chains happens only on the pacer under `submit_mutex`;
  - a wait timeout keeps the image acquired and retries the wait next frame (as the spec requires);
  - entries never released are dropped from the frame.
- **16-swapchain fallback (12e76d9):** one retry only; the fill's `live` flag is cleared before
  `DestroySwapchainSynced`, and the callers (hands, panel) are not under `submit_mutex`, so there is no self-deadlock.
- **Switch defaults** (`Config.h`) match the STATUS table: MenuPanel off, MenuPanelRoom Freeze at 0.55, SeamlessDoors
  off, PhysicalMelee off, PhysicalMeleeOnline off, PhysicalMeleeMode Contact, MeleeHitJolt off, BeltPouches off,
  HandTargeting off, ReloadGesture on, GameWeaponModels on, DepthLayer off, DepthWarp off. Load and save are symmetric.

## Addendum: sub-review of MenuPanel input, ReloadDriver, hand targeting, models (arrived late; 0 critical, 9 minor)

- **a1 (minor, verified):** R1 comes back without a release-first latch after a reload ends (`ReloadDriver.cpp:122-131`, `:231-235`; `VRInputSource.cpp:638`), including 50 ms into a screen that just opened. Fix: latch the R1 controls release-first on a Screen/Lost end, or keep R1 held back while `MenuMode()`.
- **a2 (minor):** L1 can reach the first 1-3 frames of a screen opened mid-reload (`ReloadDriver.cpp:1076`, `:184-187`; MenuMode lags 0x720AA0). Fix: end the hold on the player's own Start/Triangle/Circle this poll, or on 0x720B80 rising.
- **a3 (minor):** `ReloadDriver` has no `OnStateLoaded` (offline only). Fix: add it, call from `GameFeedback::OnStateLoaded`.
- **a4 (minor, gameplay):** R1 cannot abort a reload for up to 6 s (`ReloadDriver.cpp:232`). Fix: a fresh R1 edge from the player ends the attempt.
- **a5 (minor, cosmetic):** `s_screen_up` / `s_room_frozen` stay set after the VM stops (`MenuPanel.cpp:89-97`, `:971-976`). Fix: clear in `MenuPanel::Destroy()` and the VM shutdown.
- **a6 (minor, perf, unverified):** MenuPanel snapshots on every depth clear / full-frame blend after the room even with no screen up; a failed TakeSnapshot retries per draw (`MenuPanel.cpp:229-236`, `:744-777`).
- **a7 (minor, misdirected confirm, not automatic):** the panel pointer ignores the display rectangle offset (`GSRendererHW.cpp:456-465` publishes u0..v1; `XRCompositor.cpp:11188-11210` ignores it). Fix: remap through u0..v1 or refuse the panel unless it is the full picture.
- **a8 (minor, UB):** MeshRaster guard-band codes computed before the near clip, s32/float-to-int overflow in snapping (`MeshRaster.cpp:535-538`, `:852`, `:880-882`, `:1268-1270`, `:1299-1300`; also `ZoneMarkers.cpp RasterBox`). Memory-safe (clamped), garbage triangles possible.
- **a9 (minor) = m1:** Press mode Circle with the weapon lowered (`PhysicalMelee.cpp:245-260`, `:290`).

## Addendum 2: sub-review of CameraDriver silence / whenSetting / keepThroughDisarm, MenuPanelRoom, VRProfileDB, lifecycle (arrived late; 0 critical, 2 major, 7 minor)

- **b1 (MAJOR, verified):** a profile reload or a missing profile strands patches untracked, including whenSetting / keepThroughDisarm words. Tracking is by list index and `s_*_patched.assign(size, 0)` runs whenever `cam.silence.size()` changes (`CameraDriver.cpp:1187-1188`, `1237-1238`, `1278-1282`); the `!profile || !profile->camera` early return (3264-3288) comes before `ApplyKeptSilence` (3291). Scenario: offline with SeamlessDoors / PhysicalMelee patched, a user copy of SLPM-65428.yaml is edited or dropped and any settings change runs `ProfileDB::ReloadIfChanged()` (VRManager.cpp:434): the old words stay patched; switch-off and adapter-on no longer restore them. Fix: before dropping the old tracking, restore every old-list word that reads value_on while its `when` passes (keep a copy of the old entries, or key tracking by (address, on, off)); run the kept/armed restore in the no-profile path from the last known list.
- **b2 (MAJOR) = M2, more detail:** entries with on == off (the cave NOP slots, e.g. 0xFE014, 0xFE020) are skipped, so never checked to be zero; a cave word reading neither value is left as is while the rest is written (`CameraDriver.cpp:1201-1224`, `1289`); hook `when`s check only one cave word. Online, `PhysicalMelee.cpp:2432-2463` checks `RegionZero` once and keeps `s_free == Clear`, so a reinstall after losing/regaining room master is not re-checked. Fix: treat each cave as a group (whole range, NOP slots included, reads all-off or all-on before any write); hooks only when the driver has seen the whole group in; re-run RegionZero before every online install.
- **b3 (minor):** cave words in static RAM (0xFE000) guarded by the overlay guard `0x540004 == 3` are orphaned if the guard fails while they are in (`CameraDriver.cpp:1196-1200`, `1249-1255`); online that leaves RegionZero "in use". Fix: guard static-RAM entries on their own region, or keep the patched flag and restore once the guard passes.
- **b4 (minor):** MenuPanelRoom Freeze can start from a stale pose: `s_last_cam_pose` has no age check and is cleared only on state load (`MenuPanel.cpp:1072`, `CameraDriver.cpp:3003`, `3656`, `3682`, `3771`). Fix: use it only if fresh (~100 ms) and from this armed run; clear on disarm and in the !vm_live path.
- **b5 (minor):** `ApplyKeptSilence` (3291) reads last vsync's net mode (ResolveNetMode at 3330-3336). No effect today. Fix: call after the net-mode update.
- **b6 (minor) = m2.** **b7 (minor) ~ m7:** the arrival NOPs may also suppress non-door arrival shots (`FUN_00550220` not proven door-only).
- **b8 (minor):** parser leniency: `parseGuardList` (VRProfileDB.cpp:441-470) drops individual bad guards (a `when` gets less strict) — drop the whole silence entry instead; silence addresses not checked for 4-byte alignment (1383); `melee.override.block` not cross-checked against the caves' hardcoded 0xFF000.
- **b9 (minor, docs):** `VRProfileDB.h:285-286` known whenSetting names / co-op wording stale; `CameraDriver.h:158` GetMapping comment; profile README melee section lacks `melee.jolt` and `whenSetting: MeleeHitJolt`.
- Also raised: Press-mode Circle into a take-it prompt opening mid-hold (≈ m1/a9; check against never-confirm).

## Addendum 3: sub-review of DepthExport / DepthWarp / XRCompositor / GS hooks (arrived late; 1 CRITICAL, 3 major, 12 minor)

- **c1 (CRITICAL, verified by me too):** the VR pacer thread uses the GS's VMA allocator, created with `VMA_ALLOCATOR_CREATE_EXTERNALLY_SYNCHRONIZED_BIT` (`GSDeviceVK.cpp:858`), i.e. no internal locking: `WarpTargetFor` `vmaCreateImage` (XRCompositor.cpp:8847), `WarpDestroyTarget` `vmaDestroyImage` (8795), the dump aid `vmaCreateBuffer` (9235) / `vmaInvalidateAllocation` (9308), while the GS thread allocates constantly. Undefined behaviour on every DepthWarp session (only with DepthWarp on; it is off by default). Fix: do every VMA call on the GS thread (create the per-eye D16 depth images with the ring in WarpCopyFrame / WarpEnsureSlots; the pacer only creates views/framebuffers, which is thread-safe; allocate the dump buffer on the GS thread too).
- **c2 (major) = M3:** resize destroys eye swapchains while the warp pass may still write them; wait `wp.fence[]` / vkQueueWaitIdle under the queue lock before destroying, and clear the matching `wp.target[]` there (= M4/m2).
- **c3 (major):** when the warp pass doesn't run (busy, image late within the 2 ms budget, WarpTargetFor failing), the newly published Screen entries keep the new quad pose with the previous warped image: a one-frame jolt (`WarpPass` 9018, filter 9320). Fix: per WarpTarget record the serial and quad pose of the last released image and use that pose when the pass doesn't run (as keep_screen does).
- **c4 (major, unverified at runtime):** DepthLayer's two depth swapchains (`CreateDepthChain` 1242) are not counted in the 16-swapchain fallback (12e76d9): with everything on plus DepthLayer the hands or the menu panel can still hit LIMIT_REACHED and stay disabled for the session. Fix: count them, or let the panel/hands yield the depth layer; at least log the swapchain count on a limit failure.
- **c-minor:** m1 deferred publish + failed depth release keeps a stale depth image in the published set (6545-6553, 6640); m3 no subpass dependency on the warp's depth image (DepthWarp.cpp:439-446); m4 images released with nothing written on record/submit failure (9291, CopyPanel 3019); m5 fence waits with ignored timeouts before destroys (8823, YieldEdgeFillSwapchain 5145, EnsurePanelResources); m6 view leak in WarpFramebuffer (8874); m7 depth chains not destroyed in Shutdown / on DepthLayerOff; m8 split-screen first-person viewports dropped with DepthLayer (6809); m9 depth export bounds hardening in GSRendererHW VRBeforeEndOfFrame; m10 SetCompositorWants stays true after the warp turns itself off; m11 MenuPanel::SkipDraw returns before texture-cache bookkeeping (unverified); m12 a swapchain stuck on repeated non-timeout acquire/wait errors.

## Addendum 4: sub-review of PhysicalMelee (arrived late; 1 critical-if-online, 3 major, 9 minor)

- **d1 (CRITICAL if 0x48BF7D ever flips online; MAJOR offline) = M1, more detail:** for a poll where the vsync caught the AI loop, record and slot are both the partner's (`PhysicalMelee.cpp:2366-2367`, `:2413-2414`, `:2711`; `CameraDriver.cpp:806-873`), so caves A/B/C/D act for the partner and cave F clears its +0x116 (yaml:692) and, online in CoopFull, writes its +0x08 / +0xBA0 and sends a state message as that record (yaml:605-606). Fix: same idx for N vsyncs; cross-check idx against the camera focus 0x3AEF74; in caves A/B/C/D/F compare record+3 with the focus byte, not block+8 alone.
- **d2 (MAJOR, crash):** caves are zeroed in the same vsync their sites are restored (`CameraDriver.cpp:1209-1216`, `1235-1268`, disarm 3436), while a game function a cave called may still be running (cave F jal 0x5A9DB0 / 0x64C620 / 0x6337C0 / 0x62CDA0, cave G jal 0x577870 / 0x55C450, cave D jal 0x5521B0, cave E's trampoline): it returns into zeros and runs into 0x100000. Fix: restore sites at once; zero cave bodies only after the game counter 0x48BF93 moved 2-3 frames and with cpuRegs.pc and $ra outside 0xFE000-0xFF000; or keep caves resident and clear them only on overlay change / VM reset / setting off, with that delay.
- **d3 (MAJOR, never-confirm rule):** presses can reach an opening item screen or map: MenuMode is pauseWhen 0x720AA0, which rises 0.55-0.65 s after 0x48BF7E (yaml:818-826); a swing, or Raising->Pressing (R1 then Circle, up to 0.9 s), presses Circle (the confirm) into it (`PhysicalMelee.cpp:2570`, Step 214-260, 2753); online CoopPress has a one-frame race on a partner's take-it prompt. Fix: block presses while 0x48BF7E != 0; require the screen flags clear for ~150-200 ms before a press starts; drop the press the moment either rises; consider no presses online.
- **d4 (MINOR-MAJOR):** VM reset leaves s_block_armed / s_jolt_armed set, so the next VM gets 0x90 zero bytes at 0xFF000 and 0x70 at 0xFF100 (`PhysicalMelee.cpp:1076-1084`, `1167-1175`). Fix: clear the flags without writing on VM reset/shutdown/ELF change, or check CRC + magic first.
- **d5-d13 (minor):** d5 state load leaves stale block contents (set armed=true on load so the next poll clears); d6 = b3 (caves stay after overlay swap); d7 = M2/b2; d8 cave D can give the strike object life 0 if the block is cleared mid-call (save the window on the stack); d9 cave E matches only the object pointer, add a0 == block+8; d10 cave G: v0/v1 clobbered, kind-2 entries never finish, bone index not checked against the model's real count; d11 cave A/B assumptions only checkable by test; d12 partner-swing message restores +0xBA0/+0x08 whole; d13 CoopOverrideLive can go stale if Poll stops (add a "Poll ran this vsync" stamp).

---

## Fix status (branch `reviewfix`, 2026-10-15)

Each item was checked against the code before fixing; none was refuted. Self-tests 41 of 41 (39 before, plus
`SelfTestStableSlot` and `SelfTestCaves`; a Press-mode driver check was added to the melee driver suite). The offline
checks used `run-reviewfix` with no headset: slot 5, the test-only poked pipe, the right hand scripted over PINE,
normal speed (`review_test.py` in the session scratchpad).

| Id | Status | Commit | Evidence |
|---|---|---|---|
| M1 / d1 | fixed, host side | 43b005f | `CameraDriver::StableSlot`: the slot byte is sampled once per running vsync, and a value is taken only after 8 vsyncs in a row. `GetBase`, PhysicalMelee (owner and room-master checks), the GameFeedback weapon reader and the item owner check (pouches, reload) use `CameraDriver::LocalSlot`. Melee writes its block only for a record that was also local on the previous poll. **Offline:** 254,538 PINE reads over 20 s saw the raw byte non-zero 1,824 times, and the block named only Kevin's record 0x476DD0 (128,316 reads). Contact hits still land (600 -> 540 / 510). |
| d1, cave side | **not done** | | The caves still compare only block+8. Adding a `record+3 == 0x3AEF74` check means reassembling caves A/B/C/D/F and verifying them live. Left for a separate change. The host never names a non-local record (above). |
| d3 | fixed | 512542f | New `melee.screenFlag` (0x48BF7E). No press while it or MenuMode is up, or until both have been clear for 0.2 s, and a press in flight is dropped the moment either rises. Nothing is pressed online (CoopPress or CoopFull). Static and self-tests only: the timing into a real opening screen is not run live. |
| M2 / b2 | fixed | 3d13353 | New `camera.caves` (3 caves, entry counts). Install is all or nothing: the whole range (NOP slots and gaps included) must be zero or exactly our words. The body is written and read back before any site is patched, and a site is patched only while its cave is in. **Offline:** 0x12345678 written at 0xFE09C (a gap in A-C) while the cave was in gave the log "changed under us". Site 0x552D8C read stock 0xC5020200, the cave's 70 words were zeroed, the foreign word was left, and the install was "refused" (logged once). When the word was cleared, the cave went in again. |
| d2 | fixed | 3d13353 | Sites are put back at once. A body is zeroed only after the counter at 0x48BF93 has moved 3 frames (and at least 3 vsyncs) with the EE's pc and $ra outside the cave, and any site still reading ours is put back first. Offline the zeroing ran 3-4 vsyncs after the sites. Online, every install re-checks the range (b2). |
| b1 | fixed | 31bf47a | A copy of the tracked list is kept. On a list change in the same game, or when the profile goes away, the tracked words go back before the tracking is dropped. Cave bodies stay resident. Static and self-tests only: a live profile reload was not run. |
| d4 / d5 | fixed | 6380394 | The block and jolt clears write only while the block reads our magic in the same game (CRC). After a load, a block saved armed is cleared, and its strike fields start from zero. |
| m1 / a9 | fixed | cb9236f | Pressing ends with Lost when the weapon is lowered, and `hold_attack` requires the raised stance (driver self-test). |
| m2, m3, m7 | not done (time) | | m7 stays as documented: the arrival words are patched while the switch is on, not only while armed. |
| a1-a5, a8, b3-b5, b8, b9, d6-d13 | not done (time) | | b3 is partly covered: a cave whose overlay guard fails now drains instead of staying in, so its words are no longer orphaned. |

## Fixed on branch `nightfix` (2026-10-15)

Each item was checked against the code before the fix; none was refuted. All are built locally, 39 of 39 self-tests
passed, and DepthWarp-off behaviour is unchanged (no WarpTarget exists then; a7 is a no-op when the panel is the
whole picture).

- **c1** (2f9f4e6). The CRITICAL is confirmed: `GSDeviceVK.cpp:858` sets `VMA_ALLOCATOR_CREATE_EXTERNALLY_SYNCHRONIZED_BIT`.
  - The warp's two D16 depth images and the dump buffer are now made and freed with the ring, on the GS thread
    (`WarpEnsureSlots` / `WarpDestroySlots`). The dump buffer is host-coherent, so the pacer no longer calls
    `vmaInvalidateAllocation`.
  - The pacer only makes views and framebuffers (`WarpTargetDepthView`, `WarpFramebuffer`).
  - A ring re-make (under `submit_mutex`, queue idle) drops the pacer's views first and keeps each target's swapchain
    and acquire state.
  - No VMA call is left on the pacer.
  - Includes **m6** (`WarpTargetFor` gives up on a fence timeout instead of destroying views) and addendum 3's m6 (the
    colour view leak in `WarpFramebuffer`).
- **M3 / M4 = c2** (d940b9e). `DestroySwapchainSynced` (with `submit_mutex` held) calls `WarpForgetSwapchain`. For an
  eye swapchain a WarpTarget draws into, it waits on every in-flight `wp.fence[]` (1 s, logged on a timeout) and
  destroys the target, before `xrDestroySwapchain`.
- **m5** (7584f5f). A memory barrier at the start of each warp pass: earlier passes' depth writes, then ALL_COMMANDS,
  so the render pass's UNDEFINED transition and clear are ordered after them.
- **m4** (202c9ba). On a failed record or submit, the eye images stay acquired and waited for the next pass instead of
  being released stale.
- **a7** (3a149a1). The published pointer transform is the whole picture laid out around the panel through u0..v1.

**Null-headset check** (SteamVR 2.17.10 null HMD, started before the emulator; run-nightfix, slot 7, DepthWarp on,
offline):
- the frame-size change ran: eye swapchains 1920x1344 re-made at 1536x1344, the ring re-made, and the pacer
  re-targeting both eyes;
- then 300 frames warped every 5 s, with no pass busy, no image not back, and 0.062 ms GPU mean;
- no Vulkan or XR error in the log;
- restored afterwards: `null_driver.py off` (settings md5 identical to the backup, no `forcedDriver`), and SteamVR's
  processes closed.

**Second pass (2026-10-15, same branch; built, 39 of 39 self-tests; no null-headset run, because another pcsx2-qt.exe
was running):**
- **c3** (403bac4). The pacer keeps a copy of the set its eye images were last drawn for. When the pass does not run
  (busy, an image late, no target, a failed record), that set's Screen entries, head and turn clock go out instead of
  the newer set's, as keep_screen does, and the turn is worked out again.
  - Only when both sets have the same number of Screen entries.
  - The copy is dropped when one of its swapchains is destroyed.
  - The Warp line counts these frames ("at the poses last drawn for").
- **c4** (943f154).
  - Every swapchain create and destroy is counted, and a LIMIT_REACHED logs how many the compositor holds.
  - `CreateSwapchainYielding`, after the edge fill, now gives up DepthLayer's two depth swapchains: the depth layer
    goes off for the session, the GS's copies are waited first, and the published depth is dropped. Then it retries.
- **m10** (c02eb94). The compositor's want follows what is still on: the warp's pipeline failure keeps the want for
  the depth layer, and `DepthLayerOff` keeps it for the warp.
- **m1** (dc3898e). A depth release that fails also invalidates `s.published.fp_depth` when it names that swapchain
  (the deferred set was published before the releases).
- **m7** (e5bbffa), partly. Shutdown destroys the depth swapchains. `DepthLayerOff` still leaves them to the next
  resize or Shutdown, because it runs inside copy paths.

**Still open:** addendum 3's m8, m9, m11, m12. None was refuted.
