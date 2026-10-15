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
