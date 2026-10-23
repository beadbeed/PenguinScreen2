# Late review, 2026-10-22 (`0557a8d..d13607a`, branches `gamecues` and `gunobject`, on `nightly`)

This was a read-only review: no edits, builds, emulator runs or headset. All line numbers refer to the `nightly`
worktree (`D:\dev\PenguinScreen2\.claude\worktrees\nightly`) at `d13607a`.

What was reviewed:
- `[VR] GunObjectAim`, the whole path:
  - cave O and the flash stub, decoded by hand from the profile (`SLPM-65428.yaml:1101-1150`);
  - their two sites, against a disassembly of `FUN_0062CF10` (0x62CF10-0x62D2BC) from the RAM dump `ram5_a.bin`;
  - the host, `GunRay::Poll`, `ClearObject` and `OnStateLoaded`;
  - the profile parse and the online gates.
- The fake object's lifetime: every use of `s0` in `FUN_0062CF10`, plus the callees that receive a pointer into it:
  `FUN_001913A0`, `FUN_00190DB0`, `FUN_0018FAB0`, `FUN_006801A0`, `FUN_0055E400` and `FUN_0055EA60`. Also
  `FUN_005AC8D0`, the flash.
- `[VR] GameText`, both paths in `GSRendererHW::DetermineVSConfig` (TrueStereo's flat plane and the
  vertex move without TrueStereo), and the pickup toast's cancel.

Anything marked **unverified** was reasoned from the code or the dump, not shown at runtime.

**Counts:** critical 0, major 1, minor 9.

---

## Critical

None.

## Major

### M1 (GameText; unverified): "2D text" is classified as any FST draw that does not fill the scissor, so world effects, strip-drawn screen effects and partial fades can be shrunk toward the centre too

**FIXED (2026-10-22, after the review):** both paths now use one test, `EyeProjection::IsGameTextDraw`: a textured
(`TME`) FST sprite at one Z within 15 of the depth format's maximum (the 2D layer: File #1's subtitles and "Obtained"
are Z 0xFFFFFFFD on a Z32 buffer, depth-tested GEQUAL), not sampling the frame or depth buffer it draws into, not
filling the scissor. TrueStereo's flat plane is halved only for those draws; every other flat draw keeps 0.9.
`(VR) GameText scales/leaves` logs each first-seen kind (texture page, PSM, CLUT, Z, rows; `PCSX2_VR_GAMETEXT_LOG=<n>`,
default 24). The font page is logged, not checked (one page, `TBP0 0x2A00` PSMT4, `CBP 0x2A02`, in every run, but the
examine and system lines were not seen). Self-test: 8 new checks (text yes; particle, mixed-depth sprites, triangle
quad, fade, untextured fill, frame-buffer sample, STQ no). Offline (`run-nulltest`, no OpenXR runtime): slot 7's
subtitle (rows 322-342) and "Handgun Obtained" (382-402) scaled; slot 4's subtitle (322-342) scaled with
`PCSX2_VR_TRUESTEREO_TEST`; slot 6, three shots with `GunObjectAim`: no FST draw at all (the flash is not FST, so
neither path touches it now; TrueStereo halved it before). Not run: a door fade or the danger filter.

**Where:**
- `GSRendererHW.cpp:6564-6590`, without TrueStereo. The test is `PRIM->FST`, the multiview target, and
  `!fills`, where fills means at least 95% of `scissor.in`. Nothing else is checked.
- `GSRendererHW.cpp:6498` and `:6525`, with TrueStereo. Every `kind == 0` draw that does not fill gets the
  plane at `0.9 * kGameTextScale`. `kind == 0` covers FST draws, plus non-FST draws without a perspective Q.

**What is wrong:** FST means only "UV coordinates". It does not mean "2D text". The test does not look at:
- `TME`;
- the texture (font page or CLUT format, or the frame buffer itself);
- Z;
- the primitive type;
- whether the draw is part of a multi-draw full-screen pass.

**Failure scenarios** (all display-only, offline and online alike):
- **World sprites move.** Billboard particles that a VU1 renderer emits as FST sprites get pulled halfway toward the
  view centre. That covers blood, sparks, smoke, fire and the game's own muzzle flash.
  - So they no longer line up with the world. The muzzle flash is the effect GunObjectAim now places at the VR muzzle,
    so M1 and GunObjectAim interact directly.
  - With TrueStereo these draws were already on the flat plane (that was already true before this diff). GameText
    halves that plane again.
- **Strip effects break up.** A full-screen effect drawn as several draws gets each strip shrunk about the centre,
  which leaves a half-size ghost of the picture in the middle. Examples are a blur or feedback pass, the danger or
  poison filter, and a flash, issued in 64-px strips with one TEX0 per strip, so each draw covers less than 95%.
- **Fades shrink.**
  - A fade or darkening rectangle that covers the picture but not 95% of the scissor gets shrunk to the centre half.
    That happens when the scissor is taller than the 448-line picture, or when a fade covers the 3D view only.
  - The research note measured the picture at 512 x 448. The scissor was not measured.
- **Text leaves its box (no TrueStereo).** If a text window's background is untextured or STQ, it is not FST and stays
  put while its FST text moves half-way to the centre.

**Fix:**
- Narrow the test to the 2D layer:
  - `PRIM->TME && PRIM->FST`;
  - a sprite primitive;
  - constant Z equal to the 2D layer's Z (`m_vt.m_eq.z`, with the value recorded in research);
  - the texture is not the current frame buffer (`TEX0.TBP0 != FRAME.FBP`);
  - ideally the font texture's page or PSM/CLUT, as seen for the subtitles and "Obtained".
- Log each first-seen (TBP0, PSM, Z, prim) that GameText scales, so a headset run shows what was caught.
- Until that is done, check a shot (muzzle flash and blood), a door fade, and the danger filter with GameText on and
  then off.

---

## Minor

### m1 (GunObjectAim): the hands stop drawing their flash whenever the block is armed, not when the game's flash actually came from the VR gun; and both switches default to true in code
- **Where:** `XRCompositor.cpp:2700` (`!GunRay::GameFlashOnVrGun()`), `GunRay.cpp:610` (`s_obj_live` true on every armed
  poll), `Config.h:1409` (`GunObjectAim = true`), `Config.h:1465` (`GameText = true`).
- **What is wrong:** our flash is suppressed even when the game's flash is not there. That happens when:
  - cave O fell back to the game's object (stale stamp, focus elsewhere);
  - the flash is skipped (`FUN_005AC8D0` runs only in the aiming stance, `0x62D238-0x62D264`);
  - the game's flash is not drawn in first person. Section 13 calls this inconclusive.
  - M1 moves the flash.
- **Scenario:** in the headset a shot shows no muzzle flash at all. STATUS says the playtest ini has `GunObjectAim`
  false, but a fresh ini, or any other machine, gets it on.
- **Fix:**
  - Suppress our flash only for a shot whose flash-stub count (`+0x10`) went up within a couple of frames.
  - Or default `GunObjectAim` to false in `Config.h` until the headset check.

### m2 (GunObjectAim; unverified): the stamp is a mod-60 window, not a timeout, and a state load does not clear a saved GOB1 block unless this session armed one
- **Where:** cave O `0xFB044-0xFB064` (`age = counter - stamp`, plus 0x3C if negative, `< 7`). `GunRay.cpp:421-428`
  (`s_obj_armed = s_obj_addr != 0`). `GunRay.cpp:353-363` (`ClearObject` zeroes only when `s_obj_armed`).
- **What is wrong:**
  - A block nobody refreshes passes the freshness test again each time the counter comes round to `stamp..stamp+6`.
    That is 7 of every 60 counts, several times a second at the measured ~255 counts/s.
  - So the stamp alone never expires a stale pose. Only `ClearObject` does.
  - After the first state load of a session, `s_obj_addr` is 0, so a block saved armed in that state (magic, the same
    local record) is never zeroed. It is zeroed only after the host arms once.
- **Scenario:** load a state saved with GunObjectAim armed, then shoot in a poll where the host does not write. That
  can be the first poll after a record change, a failed `ModelLaserFrom`/`GetMapping`, or the right hand untracked. In
  about 1 shot in 9 the game's frame, flash and shell come from the pose at save time.
  - Offline only, and the window is narrow: once the gun is in a tracked hand the host rewrites the block every poll.
  - Cave I (`GUN1`) and HandPickup share the stamp design.
- **Fix:**
  - On a state load, zero a block that reads `GOB1` (and the GUN block), whether or not this session armed it, as
    HandPickup does.
  - Better, stamp with a counter that does not wrap within seconds.

### m3 (GunObjectAim): the block's counters are written before the RAM is known to be free or ours
- **Where:** `GunRay.cpp:591-597` (shots and flashes zeroed on (re)arm), then the matrix, record, stamp and magic.
- **What is wrong:** this is the same class as the night review's m4. There is no zero-or-ours check like
  `BlockOurs`/`RegionOurs`. The research note says 0xFF500-0xFF5FF reads zero in every savestate and dump, so the risk
  is low.
- **Fix:** refuse to arm if any word of 0xFF500-0xFF58F is neither zero nor already ours.

### m4 (flash stub): it checks only the record, so its "the flash came from the VR muzzle" evidence can be wrong
- **Where:** stub `0xFB084-0xFB0BC`, `GunRay.cpp:621-636` (the log).
- **What is wrong:** the stub copies `a2` and counts for `a0 == block+0x08`. It has no magic, stamp or "cave O swapped
  this shot" check. When cave O fell back (stale stamp), the flash comes from the hidden character's gun, but it is
  still counted and logged with a distance to the VR muzzle. This is diagnostics only: the game's call always goes on
  to `FUN_005AC8D0`, and `t1`/`t3` are dead at a call boundary.
- **Fix:** have cave O set a "swapped" word that the stub checks and clears, or compare the shots and flashes counts in
  the log.

### m5 (GunObjectAim): the grabbed-shot branch also takes the fake object
- **Where:** `0x62D070-0x62D138`, stance 5. `s0` has already been swapped at `0x62CF98`, so `FUN_006801A0`, the flash
  at `0x62D110` and the shell at `0x62D124` all use the VR gun's pose.
- **What is wrong:** section 13 says this branch "is not stubbed", which can be read as untouched. Only the flash stub
  skips it. The behaviour is probably wanted, but it is undocumented and untested.
- **Fix:** state it in section 13, and test one grabbed shot (slot 5).

### m6 (GunObjectAim; unverified): the effects follow the VR muzzle through walls
- **Where:** `FUN_006801A0(rec, s0+0x200)` (spawns an object at the position through `FUN_00543800`, template
  `0x5A7B30`) and the shell `FUN_0055E400`. Both now take the VR muzzle.
- **Scenario:** a hand pushed through a wall or door, which `HeadInWall` does not stop for hands, spawns the shell and
  that object on the far side. If `FUN_006801A0`'s object is a light or a noise source, it now sits beyond the wall.
  Cave I's bullet origin already behaves this way. Offline only.
- **Fix:** clamp the muzzle to the head-to-hand segment against the room's walls (HeadInWall's collision copy), or
  accept it and note it.

### m7 (GameText; unverified): the moved vertices leave the draw's bookkeeping on the old box
- **Where:** `GSRendererHW.cpp:6570-6581`. The vertices move after `m_r` and `m_vt` are computed and after
  `EmulateDATEGetConfig` (`:9554`). Only `drawarea` is widened (`:9607`).
- **What is wrong:**
  - The target's drawn/valid tracking and any later region copy still use the old `m_r`.
  - A DATE setup is built for the old box.
  - Text inside the picture is probably unaffected, because the frame buffer is already fully valid. A text draw with
    DATE, or a region readback (MenuPanel split, which is excluded now), could clip it.
- **Fix:** recompute `m_r` from the moved vertices before the texture-cache and DATE steps (move the GameText block
  earlier), or skip DATE draws.

### m8 (GameText; unverified): the first frames of a menu can be drawn at half size
- **Where:** `GSRendererHW.cpp:6525`, `:6565` (`SpatialControls::MenuMode()`, set from the EE-side detection).
- **Scenario:** the item screen or a prompt draws a frame or two before `MenuMode` turns on, so it flickers at half
  size, then full.
- **Fix:** none needed unless it is seen. Otherwise hold GameText off for a frame after any game-screen flag changes.

### m9 (docs): the 45 Auto's model muzzle is given as (-9.5, 0, 5.0) in section 13 and as 5.1 in the profile comment and the self-test
- **Where:** `research/2026-10-14-vr-melee.md` section 13, `SLPM-65428.yaml:1709` comment, `GunRay.cpp` self-test.
- **Impact:** none. The host reads the value from RAM.

---

## Verified (no finding)

**Online gating.**
- All 50 GunObjectAim entries (2 sites and 48 cave words) are `offlineOnly: true`.
- `SilenceSettingOn("GunObjectAim")` is `GunRayAim && GunObjectAim`.
- `SilenceSettingCoopLive` returns false for it (`VRProfileDB.cpp:5905-5919`).
- The host writes the block only inside the `gun` path, which needs `offline` (`!EthEnable && !OnlineSafe`) and
  `GunRayAim`.
- The patch engine refuses `offlineOnly` entries under `OnlineSafeNow()`.

**Another player's record.**
- Cave O writes only the block (`+0x0C`) and the register `s0`.
- It acts only when `s2` is the block's record and `s2+0x03` equals the focus `0x3AEF74`.
- The stub writes only `+0x10` to `+0x1C`, and nothing writes a record.
- GameText writes no game memory.

**Menus.** Nothing is confirmed. The toast cancel only shortens our own toast, and only the serial it named
(`ItemReach.cpp:866-872`, `HudCards::CancelToast`).

**Cave O decode** (0xFB000-0xFB080):
- It redoes `addu v1,v1,a0` and `lw s0,0(v1)`. In the site's delay slot, the `lw` reads `0x3967EC`: aligned and
  harmless.
- `beq s0,zero` goes to `jr ra` with `lui t3` in its delay slot.
- The magic is `GOB1` (`lui/ori 0x474F4231`), the record is `0xFF508`, and the focus is `0x3AEF74`
  (`lui 0x3B` + `-0x108C`).
- The counter is `0x48BF93` (`lui 0x49` + `-0x406D`), with the stamp at `lbu 0xFF504`. The wrap adds 0x3C, and the
  test is `sltiu < 7`.
- The shots count is at `+0x0C`, and `s0 = 0xFF380` (`addiu s0,t3,-0xC80`).
- All five branch targets land on `0xFB07C` (`jr ra`; nop slot).
- It is a leaf: no calls, no stack. Its `t1-t3` are dead at `0x62CF94`: they are next written before any read, and
  `t0` is reloaded at `0x62D140`.
- The site's `jal` overwrites `ra`, which is safe because `FUN_0062CF10` saved `ra` at `0x62CF18` and reloads it at
  `0x62D2A8`.
- The return lands at `0x62CF9C`, `beq s0,zero` on the cave's `s0`.

**Flash stub decode** (0xFB084-0xFB0BC):
- The site's delay slot (`addiu a2,sp,208`) runs before the stub.
- The stub tail-jumps `j 0x5AC8D0`, so `ra` stays `0x62D29C` and pc and ra never stay inside the cave.

**Encodings.**
- `0x0C03EC00` is `jal 0xFB000`, and `0x0C03EC21` is `jal 0xFB084`.
- The `off` words match the dump: `addu v1,v1,a0` and `jal 0x5AC8D0`.
- The engine writes `on` only over `off`.
- No other patch touches 0x62CF10-0x62D2BC, except cave I's site at 0x62D1CC.

**Install and restore.**
- Cave O is its own `camera.caves` group (0xFB000-0xFB0C0, 48 words, all or nothing, read back), and it overlaps no
  other cave.
- Both sites come before the words and check the cave's first word and its `j`.
- When the setting goes off, the sites are restored, and the cave is drained and zeroed.
- The host's `ClearObject` zeroes the block when the magic is ours.

**The fake object's lifetime.** It cannot dangle (checked statically on `ram5_a.bin`).
- In `FUN_0062CF10`, `s0` is only:
  - tested for zero;
  - read at `+0x1C0` (as a pointer to the matrix) and at `+0x200..+0x208`;
  - restored from the stack at `0x62D2B4`.
  The object pointer itself is never passed or stored.
- The callees that get `s0+0x1C0` or `s0+0x200`:
  - `FUN_001913A0`, `FUN_00190DB0` and `FUN_0018FAB0` are leaf VU0/FPU helpers that read it and write to their
    `a0` (stack).
  - `FUN_006801A0` copies the 3 floats to its stack before `FUN_00543800`.
  - `FUN_0055E400` reads the matrix through `FUN_0018FAB0`, `FUN_001913A0` and `FUN_0055EA60`, and stores only `rec`
    (`+0x24`) and its own `+0x50` (`+0x1F0`) in the new effect.
  - `FUN_005AC8D0` gets the caller's stack frame and muzzle, as in the unpatched game.
- So nothing keeps a pointer into 0xFF380-0xFF58F after the shot.
- The matrix (0xFF540) and the position (0xFF580) are 16-byte aligned for `lqc2`. The profile parse requires
  `block % 16 == 0`.

**The overlapping fake base.** The fake object's base 0xFF380 lies inside VrBody's block (0xFF300-0xFF3FF) and
SpatialAudio's (0xFF400). That is harmless only because nothing reads the object outside `+0x1C0..+0x20B`, which was
checked above. A new caller of `FUN_0062CF10`'s object, or a future cave, must keep to that.

**Two smaller points.**
- With cave O, the game's own frame at `sp+0x40` is already cave I's frame F. So if cave I's stamp test failed in the
  same shot, the bullet would still follow the VR ray. The two caves cannot disagree in a harmful way.
- The toast serial logic is race-free under `s_toast_mutex`. A later toast is never cancelled.

## Fixed on minorsC (2026-10-22)

Each item was checked against the code first. Built locally (`build-minorsC`, no LTO), `vr_selftests` 62 of 62 (8 new
checks in GunRay's suite). No headset; GunObjectAim not run live on this branch.

| Id | Status | Commit | What changed |
|---|---|---|---|
| m1 | fixed (suppression); defaults kept | 8e2a30d | Confirmed: `GameFlashOnVrGun` was true on every armed poll. New `GunRay::FlashEvidence`: a flash counts as the VR gun's only when cave O's shot count went up in the same poll or within 100 ms before the flash stub's count; the hands then leave their flash out for 150 ms, and draw it otherwise (a fallen-back shot, no flash when not aiming, no shot yet). If GunRay's poll sees the flash a poll after the shot, the hands' flash shows for one frame first. `GunObjectAim` and `GameText` still default to true in `Config.h` (not changed without the headset check). |
| m2 | fixed (state load) | 7d1dd12 | Confirmed. The next poll after a state load, live or not, looks up the profile and zeroes the GUN1 and GOB1 blocks when they read our magic, whether or not this session armed them. The stamp stays mod 60: a longer counter needs caves I and O reassembled. |
| m3 | fixed | c664196 | Confirmed. Cave O's block arms only when `RegionFreeOrOurs` passes over all 0x90 bytes (logged once otherwise); the game's object is used then. |
| m4 | fixed | 8e2a30d | Confirmed. The log compares the counts (FlashEvidence): a flash without cave O's shot is logged as the game's own and is not compared with the VR muzzle. The stub itself is unchanged (no cave work). |
| m5 | fixed (docs) | b9b519d | Confirmed (`0x62D070-0x62D138` after the swap at `0x62CF98`). Section 13 now says the grabbed branch's `FUN_006801A0` object, flash and shell come from the VR gun's pose too. The grabbed-shot test (slot 5) is not done. |
| m6 | left (tried, reverted) | 6a260a1, reverted in 54d2fdc; e5c33a0 | A fallback to the game's object when the head-to-muzzle line crosses HeadInWall's player collision. Taken out: in slot 7 that collision fences the desk at full height (see the night review's m10 row), so it would drop the VR flash and shell near any furniture. Noted in section 13. |
| m7 | fixed in part | cd46a71 | Confirmed. A DATE draw is not GameText (its DATE setup was built for the unmoved box). `m_r` and the target's valid tracking still use the old box; text inside the picture lands in an already valid frame buffer, so that part is left (unverified; recomputing `m_r` means moving the block in GSRendererHW, the renderer branch's area). |
| m8 | left (by design) | | The review's own fix is "none needed unless it is seen". |
| m9 | fixed (docs) | b9b519d | `ram5_a.bin` has (-9.504, 0, 5.095) for the 45 Auto (types 2 and 7), so 5.1 is right; section 13 said 5.0 and now matches. |
