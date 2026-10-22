# Night review, 2026-10-22 (`13e5427..bb5a6c3`, branch `nightly`)

This was a read-only review: no edits, builds, emulator runs or headset. All line numbers refer to the `nightly`
worktree (`D:\dev\PenguinScreen2\.claude\worktrees\nightly`). Budget was about 60 minutes for 61 files and 13k lines,
so I spent the time where the risk was:
- every new guest-memory write and every new cave (HandPickup, GunRay I, Fists H, SpatialAudio, VrBody U/V, FadeDim,
  the scripted-shot NOPs), decoded by hand from the profile;
- the HandPickup state machine and its presses;
- VrPresence's socket, parser and gating;
- the online gate for every new `whenSetting`;
- the lifecycle (VM change, state load, drain).

TrueStereo, the compositor, HeadInWall's maths and the IK got a lighter pass. Anything marked **unverified** was
reasoned from the code but not shown at runtime.

**Counts:** critical 1, major 3, minor 12.

---

## Critical

### C1 (critical, rule; the in-game effect is unverified): in private co-op, VR body's cave U poses the partner's character before the game's hit tests, so their hurt spheres and held weapon follow network input
- **Where:**
  - profile `SLPM-65428.yaml:1157`: the cave U site 0x346884, `whenSetting: VrBody`, no `offlineOnly`;
  - cave U at 0xFD290-0xFD370 (decoded below; it writes `bone+0x40`, the local matrix, for the slot's record);
  - `VRProfileDB.cpp:5870-5873`: `SilenceSettingCoopLive("VrBody")` returns true;
  - `VrBody.cpp:1105-1106` (the default flags are `kFlagLocals`; `PCSX2_VR_BODY_DRAWTIME` is the only other path);
  - the partner record comes from `VrPresence.cpp:987-1041`.
- **What is wrong:**
  - The co-op gate is justified in code as: "cave V only rewrites the bone matrices ... after the update and the hit
    tests, before VU1, never a record or the game's traffic" (`VRProfileDB.cpp:5870`; also the profile at `:1143-1144`).
  - That held for the first design (V at the draw). The default now is cave U, in the tick's world build. The profile's
    own text (`:1147-1152`) and the research note (`2026-10-21-vr-body.md` §7, "Hit spheres and the weapon follow the
    pose") say it runs **before** the world matrices, the held items (FUN_006313A0) and the hit tests.
  - So on each console the partner's character has its hurt spheres, its weapon's position and anything built from them
    moved by the partner's VR pose, which arrives over the UDP channel.
  - On the console that runs the room's enemies, those hit tests decide outcomes for the partner's character: a
    zombie's bite or grab landing on them, and their own attack objects if they are simulated here.
  - That is writing another player's character in the sense that matters. The rule says "never another player's
    character", and the stated reason the gate is open is false for the default path.
  - The pose also comes from an unauthenticated LAN packet (see m5).
- **Failure scenario:**
  1. Private co-op with VrBody on, and Alex's console is the room's master.
  2. His brother raises his VR arm over his head.
  3. On Alex's console the brother's arm hurt-sphere moves by tens of cm, so a zombie swipe that the brother's own
     console shows missing lands (or the reverse). The master's verdict is the one sent.
  4. Damage, grab or down state diverge from what the partner saw.
- **Suggested fix:**
  - Online (adapter on), never let cave U name a partner record: use the draw-time path (cave V world matrices only,
    after the hit tests) for slot 0 whenever `!NetworkOffline()`, and keep cave U for the offline self and loopback.
  - Or make the cave U site `offlineOnly`, or split it into its own `whenSetting` that `SilenceSettingCoopLive` refuses.
  - Fix the comment in `VRProfileDB.cpp:5870`.
  - Ask Alex before VR body goes on online at all, since it puts the partner's pose into the game's simulation.

Cave U decode (for the record):
1. It calls FUN_005A6720 as before (v0 and f0 are untouched afterwards).
2. It checks `0xFF300 == 'VBD1'`.
3. For each of the two slots at 0xFF310 / 0xFF350, it requires `slot.record == a0` and `flags & 8`.
4. It reads `model = *(a0+0x34)` and `count = model+0x816`, and requires `bone id < count` and `ptr = model+0x820+4*id`
   to be non-zero.
5. It copies 16 words to `ptr+0x40`, then stamps `+0x38`.

The branch offsets check out, and there is no focus check: it acts for whatever record the slot names.

---

## Major

### M1 (major, never-confirm; the trigger is unverified): HandPickup's arming Circle can reach a screen that is opening, or a take-it prompt from a pickup already in progress
- **Where:**
  - `HandPickup.cpp:519` (`screen_up = overlay(0x720AA0) == 1 || MenuMode()`);
  - `:132-165` (Idle → Armed: a 100 ms Circle press, `StartPress` at `:595`);
  - `GameFeedback.cpp:766` (`MenuMode` is pauseWhen 0x720AA0);
  - compare `PhysicalMelee.cpp:3120` (melee now also reads `melee.screenFlag` 0x48BF7E for this reason; review d3).
- **What is wrong:**
  - 0x720AA0 rises 0.55-0.65 s after 0x48BF7E on the item screen and the map (profile `:1421`). HandPickup reads only
    0x720AA0.
  - Nothing checks whether the character is already busy with a pickup the player started with their own Circle (the
    reach animation is about 1.2-1.5 s with no screen flag up).
- **Scenario 1:** the player presses Start or Triangle and squeezes on a highlighted herb within about 0.6 s. Circle is
  held for 100 ms while the item screen opens, where Circle selects or uses the item under the cursor.
- **Scenario 2:** the player's own Circle starts the take-it reach for item X, and the hand squeezes item Y during the
  reach.
  - The take-it prompt for X opens inside the 100 ms Circle hold.
  - If the prompt reads a held or new Circle that frame, X is confirmed automatically.
  - HandPickup itself then only logs "a game screen came up before the search" (`:181`).
- **Fix:**
  - Read `melee.screenFlag` (0x48BF7E) as part of `screen_up`.
  - Require the screen flags to be clear for about 200 ms before arming.
  - Refuse to arm while the record's action is not idle/walk (a pickup or examine in progress), or within about 1.5 s of
    the player's own Circle.
  - Drop the Circle press the moment either flag rises: have `CurrentPress` check a "screen rising" atomic.

### M2 (major, never-confirm; unverified): the confirm press does not hold off the menu pointer's D-pad, so the cursor can leave the checked free cell while Circle is down
- **Where:**
  - `HandPickup.cpp:217-268` (Prompt: the cursor cell is checked, then `press_circle`);
  - `:530-536` (`player_buttons` counts A/B/X/Y, trigger and the controller's own D-pad, but not the right-stick menu
    pointer's D-pad, `SpatialControls::SetPointerDpad`);
  - `VRInputSource.cpp:683-711` (ApplyPickup overrides the D-pad binds only while *steering*; during the Circle press
    the pointer's D-pad goes through).
- **What is wrong:** the free-cell check is made at vsync N, and the 100 ms Circle press spans about 3 game pad reads.
  If the stick-driven pointer D-pad moves the cursor in that window, Circle lands where the cursor went:
  - a full cell, which starts the game's swap flow (explicitly forbidden);
  - or the panel to the right (the partner's panel, per `DpadTowardFree`'s comment).
- **Scenario:** a seated player holding the grip nudges the right stick to look round as the prompt settles (300 ms).
  The confirm goes onto a full cell.
- **Fix:**
  - Count the pointer D-pad and any stick deflection as `player_buttons`.
  - While HandPickup presses Circle, zero every D-pad bind, as the steering path already does.
  - Require the cursor cell to be unchanged for the last ~100 ms before the press.

### M3 (major, memory of the next game): VR body never forgets its block across a VM change, then zeroes 3 KB in whatever runs next
- **Where:**
  - `VrBody.cpp:893-898` (`Shutdown` has no caller anywhere; `grep VrBody::` finds only `Poll` at `VMManager.cpp:2898`);
  - `:917-927` (`clear()` runs `ClearBlock(s_written_block, s_written_tables)` whenever `s_written && HasValidVM()`);
  - `:698-703` (0x100 bytes at 0xFF300 plus `2 * 0x600` at 0xFD400, with no CRC or magic check).
- **What is wrong:** `s_written` is still set when the VM stops. On the next VM's first vsync, `Poll` finds no File #1
  body block (`!bp`) or the switch off, calls `clear()`, and writes zeros into the new VM's RAM at 0xFD400-0xFDFFF and
  0xFF300-0xFF3FF. That is BIOS or ELF-loader time, or another game.
- **Scenario:**
  1. Play File #1 offline with VrBody and VrBodySelf on (so `s_written` is true every vsync).
  2. Close the game.
  3. Boot File #2 or any other disc: 3 KB of its RAM is zeroed once, at boot. That causes a crash or corruption,
     depending on what lives there.
- **Fix:**
  - Clear `s_written` (without writing) on VM shutdown, reset and CRC change: call `Shutdown` from the VM teardown, and
    note that it must not write.
  - Make `ClearBlock` conditional on `memRead32(block) == kBlockMagic` and the CRC it was written under (as
    `GunRay::Clear` and SpatialAudio already do).

---

## Minor

- **m1 (FadeDim strands its patch).**
  - **Where:** profile `:338` (`0x1B9DCC`, `when: [..., {0x308898 == 0}]`); `CameraDriver.cpp:1512-1516` (a failing
    `when` forgets the patch) and `:1568-1574` (restore skips untracked words).
  - **What is wrong:** the fade colour is runtime state, but it sits in `when`, which means "the code is gone".
  - **Scenario:** a non-black fade starts while armed. The word stays `beq zero,zero` untracked. Until the next black
    fade while armed, every fade quad is skipped, including outside first person (cutscenes, the world screen, door
    fades without SeamlessDoors).
  - **Fix:** use `activeWhen` (or another "pass, else restore" list) for the colour, keeping only the code check in
    `when`.
- **m2 (the cave drain timing is weaker than it looks).**
  - **Where:** `CameraDriver.cpp:4552` (`drain_counter = 0` on a state load); `:4853` (counter difference `& 0xFF`).
  - **What is wrong:**
    - 0x48BF93 counts 0-59 (research `2026-10-14-melee.md:154`; the caves add 0x3C on wrap). Near a wrap (start 57-59),
      `(now - start) & 0xFF` is ≥3 after one or two frames.
    - After a load, the comparison is with 0, so a cave that is not wanted (disarmed, or the setting off in this
      session) is zeroed on the first vsync where pc and ra are outside.
  - **Scenario:** a return address into the cave may still be on the stack, for example SpatialAudio's
    `jal 0x191A10`. Offline only, **unverified**.
  - **Fix:** seed `drain_counter` with the current counter, and take the difference mod 60.
- **m3 (VR body has no state-load hook).**
  - **Where:** `VrBody.cpp:938-944`, `1197-1201`.
  - **Scenario:** a state saved with 'VBD1' and live slots is loaded while VrBody is on but nobody is posed now, so
    `s_written` is false. `RegionOurs` accepts the magic, `any` is false, and `clear()` writes nothing. Cave U keeps
    applying the state's stale local matrices to that record every tick: frozen arms, with their hit spheres and held
    weapon.
  - **Fix:** an `OnStateLoaded` that zeroes a block reading our magic (as HandPickup does).
- **m4 (GunRay and fist words are written without a free-RAM check).**
  - **Where:** GunRay's block 0xFEB80-0xFEBDF (`GunRay.cpp:432-453`) and the fist word 0xFEBF0 (`PhysicalMelee` cave H
    word) are written on first use.
  - **What is wrong:** they are not checked to be zero or ours first, unlike HandPickup (`BlockOurs`), VrBody
    (`RegionOurs`) and SpatialAudio.
  - **Fix:** the same zero-or-magic check before the first write.
- **m5 (VrPresence exposure).**
  - **Where:** `VrPresence.cpp:386`, `:442`, `:870`.
  - **What is wrong:**
    - A non-loopback peer binds `INADDR_ANY`, which is every interface, public Wi-Fi included.
    - Only the source IP is checked, not the port.
    - The key is an FNV-1a hash of an optional word, so an empty key is a fixed public value. Any LAN host that spoofs
      the peer's IP can drive the partner's pose, and with C1 their hit spheres.
  - **Impact:** the parser itself is safe. It is fixed-size, checksummed, checks slot ≤ 3 and the flags mask, requires
    finite values under 1e6, and limits reach to `kMaxReachCm`.
  - **Also:** binding `0.0.0.0` raises the Windows Firewall dialog on first use, possibly while Alex is in the headset.
  - **Fix:** bind the local address that routes to the peer, check the source port, require a non-empty key of 8 or more
    characters (better, an HMAC over the packet), and document the firewall prompt.
- **m6 (the partner search does not exclude the local record).**
  - **Where:** `VrPresence.cpp:1003-1023`.
  - **What is wrong:** it matches by slot byte and `used` only. `in.slot == local_slot` refuses the obvious case, but
    `r != base` is not checked, so a stale or duplicate slot byte (a record mid-change) could name the local record.
  - **Fix:** add `r != base`.
- **m7 (TrueStereo: the eyes are computed twice and can disagree).**
  - **Where:** `GSRendererHW.cpp:6436-6500` (per draw) and `XRCompositor.cpp` present (`present_true`).
  - **What is wrong:**
    - The GS builds the eyes on every draw. Roll comes from `StereoState::HeadRoll` at draw time, plus `GetEyeInfo`.
    - The compositor rebuilds them at present. Roll there comes from the frame's render-pose quaternion.
    - Draws inside one frame can use different rolls, and the projection layer's declared pose and fov can differ from
      the mapping used. The result is swim or a seam on roll. **Unverified**: the null headset has no roll.
    - It also costs a mutex, eight `tan()` calls and a `Build` on every draw.
  - **Fix:** snapshot one eye set per frame (at the frame's first draw, or at vsync), and hand that same set to the
    compositor with the frame.
- **m8 (TrueStereo: vertex map edge cases).**
  - **Where:** `tfx.glsl:105`.
  - **What is wrong:**
    - `we = max(-pe.z, 0.5)` with no clip: a vertex at or behind an eye is pushed far off, so a triangle near a wall
      stretches across the view.
    - A flat (FST) draw covering less than 95% of the scissor goes on the 1.5 m plane, which would break a sub-rect
      post-process pass that samples the target. **Unverified**: no such pass has been seen in File #1.
  - **Fix:** clip or cull triangles with any `pe.z > -near`; keep flat draws that sample the RT unmapped.
- **m9 (HeadInWall read costs).**
  - **Where:** `HeadInWall.cpp:272-293`, `:362-363`.
  - **What is wrong:**
    - `ReadGrid` can make up to 32×32×4096 guest reads on a malformed grid before the 8192 cap applies. It runs on the
      CPU thread at vsync, and again every 120 vsyncs.
    - When the grid header read fails, the function returns without easing the fade, so the wall dim can stick.
  - **Fix:** cap the total reads inside the loop, and call `ease_to(0)` on that return.
- **m10 (HandPickup has no line of sight).**
  - **Where:** the cave's `FUN_005ABBE0(point, record, 0)` with r 50 at the hand (profile `:1125-1130`) is gated only by
    `maxReach` 85 cm on the floor plane.
  - **Scenario:** an item behind a thin wall, counter or gate within 85 cm can be taken. Also, if the arming Circle
    finds nothing, the game's other Circle actions (a door) may run. That is deliberate input, but not what was meant.
  - **Fix:** a host-side segment test, character head to item, against HeadInWall's wall copy.
- **m11 (the SpatialAudio cave forces `w = +1`).**
  - **Where:** profile `:0xFC114`.
  - **What is wrong:** if FUN_001B7B50 uses the projected w for anything besides the behind mirror (a culling or volume
    branch), that changes too. **Unverified**: the note says volume stays distance-based.
  - **Fix:** check FUN_001B7B50's other uses of `out[3]`.
- **m12 (ScriptedShotsFirstPerson in mode 4).**
  - **Where:** `CameraDriver.cpp:1012-1017`.
  - **What is wrong:** in a mode-4 fixed cut the game holds the player's input (0x48C019 = 1), but first person keeps
    writing the record's heading and aim (body follow), in private co-op as well. Scripted moves that expect the
    character's facing may see a different one. **Unverified.**
  - **Fix:** go camera only (as GrabFirstPerson does) while 0x3AEF75 is 4 or 5.

---

## Checked and found sound

### HandPickup's confirm (apart from M1 and M2)
- It confirms only when all of these hold: the take-it prompt is up (0x720AA0 = 1, which = 1, sub = 2); the prompt's
  own item struct has record index = the floor item under the hand and item id = its id; the local record's search
  result is that record; there is a free cell; the cursor is on a free cell of your own grid; the grip has been held
  since the squeeze; there is no player button; and the prompt has settled for 300 ms.
- These are re-checked every vsync up to the press.
- A partner's hand-over cannot match: items are unique records, and the search result has to name the floor record.
- A full inventory refuses.
- It presses at most 3 closed-loop D-pad presses, never right of column 1 and never down from the bottom row.
- The state machine is cleared on a state load, the switch going off, the network, and first person disarming.

### The caves
| Cave | Range | What was checked |
|---|---|---|
| HandPickup | 0xFEC00-0xFEC94 | Decoded by hand: kind == 1, 'HPK1', `a0 == block+8`, `a0+3 ==` focus 0x3AEF74, stamp age < 19 (mod 60), clears the word, tail-jumps both ways (pc and ra never left inside) |
| I (GunRayAim) | 0xFEA00-0xFEA7C | Decoded by hand: 'GUN1', record, focus, age < 7, tail-jump to 0x5521B0 |
| H (fists) | 0xFE7E0-0xFE8EC | Reached only through cave D's spawn; refuses outside the record's room; cave D finds the new object through the list head, not v0 |
| G (MeleeHitJolt) | moved to 0xFE900 | Its site guard and `melee.jolt.hook` were updated; no absolute jumps into the old range |
| SpatialAudio | 0xFC000-0xFC12C | Saves ra, s0 and s1; v0 is preserved; the view-space row and column offsets are right for row vectors; branch targets right |
| V / SELF / U (VrBody) | 0xFD000-0xFD374 | Branch targets right; the SELF return keeps the delay-slot `ld ra` and a balanced sp |

- No cave ranges overlap. The GUN block (0xFEB80-0xFEBDF) and the FST word (0xFEBF0) sit outside every cave group.
- All seven groups are in `camera.caves` (all or nothing, read back), and each site's `when` reads its cave's first and
  last words.

### Online gates
| Setting | Online behaviour | Why |
|---|---|---|
| HandPickup | closed | `SilenceSettingCoopLive` says no, `offlineOnly`, and the host checks `NetworkOffline` |
| GunRayAim | closed | same three, and the host requires `!EthEnable && !OnlineSafe` |
| PhysicalMeleeFists | closed | its word is written only in `Mode::Offline` |
| FadeDim, ScriptedShots NOPs, SpatialAudio | closed | `offlineOnly` and not CoopLive; the SpatialAudio host also checks `!EthEnable` |
| VrBodySelf | closed | its site's `whenSetting` is not CoopLive, and the host checks `NetworkOffline` |
| VrPresence | live only in private co-op with first person armed | sends nothing otherwise, writes no game memory |
| VrBody | live in private co-op | see C1 |

### Other new code
- **No pause, speed or state calls:** none of the new code adds a pause, speed, frame-advance, or state load/save call.
- **GameRumbleEvents:** reads only, and its reads are bounded.
- **DoorVeil:** display only.
- **GrabFirstPerson:** camera only, and it withholds `LocalRecord` so melee, reload and the pickup stand down.
- **HeadInWall:** reads only, and its guest reads are bounded and aligned.
- **Shader uniform layout:** the new VS uniforms are appended at the end on Vulkan and Metal, and the Metal size
  `static_assert` covers it. DX11 and GL ignore the tail.
- **IK:** the two-bone solve guards zero lengths, NaN, and the reach clamp. The pose count is capped at 24, which fits
  the slot's bytes before the +0x38 stamp.

---

## Fixed (2026-10-22, branch `nightly`)

Each finding was checked against the code first; none was refuted. Build: local LTO release, `vr_selftests` 61 of 61
(58 before, plus "VR body online gate", "VR body clears only its own block" and "FadeDim"), `vrtest.py --check-profile`
passes for File #1 and File #2.

| Id | Status | Commit | What changed |
|---|---|---|---|
| C1 | fixed | f1e9e24 | Confirmed: cave U's site 0x346884 was `whenSetting: VrBody`, which `SilenceSettingCoopLive` opened, and the host set flag 8 (`kFlagLocals`) for the partner slot online. The site now has its own `whenSetting: VrBodyLocals` (never co-op live) plus `offlineOnly`, and the host sets flag 8 for no slot unless strictly offline (`VrBody::UseLocals`: net mode Offline, no adapter, not the draw-time fallback), the local one included. Online the partner is posed at the draw only (cave V, after the hit tests). The co-op gate comment (`VRProfileDB.cpp`), the profile text and the README are corrected. Self-test: UseLocals / SlotFlags for both slots, VrBodyLocals never co-op live with VrBody and the adapter on, the one site entering cave U is VrBodyLocals + offlineOnly. Not run online. |
| M1 | fixed | f6b94cd | The squeeze arms only with `melee.screenFlag` 0x48BF7E clear as well as the overlay and MenuMode, after all of them read clear for 200 ms, with the action byte +0x54F idle, walk or run, and not within 1.5 s of the player's own Circle (A or the right trigger). While armed, a screen flag rising clears the word and lets Circle go; `CurrentPress` re-reads the flags at every input poll and drops the arming Circle the moment one rises. **Offline** (run-nulltest, slot 7, `pickuptest.py --screen-first y`): Triangle 0.15 s before the squeeze gave 0x48BF7E = 1 with the overlay still 0; logged "squeeze while a game screen is opening (the screen flag is up): nothing armed"; the item screen opened 0.5 s later, nothing pressed, cells unchanged. |
| M2 | fixed | f6b94cd | While the take-it prompt is checked or steered, and while a confirm or steering press is out, every control of every PS2-pad instance reads 0 except our press (the menu pointer's D-pad, the grab breakout, reload, melee, zones). Player input counts every button, bumper, menu, view, stick clicks and a stick past half. The cursor struct (grid, node, index) is tracked: any change after the prompt's first 100 ms that is not the one cell our D-pad press sends it to aborts, and it must be still 100 ms before the confirm. `CurrentPress` re-checks at every poll that the prompt is up, the cursor is on the checked node and index of your own grid, that cell is still free and the player pressed nothing; otherwise the press is dropped and the take ends. Self-tests for each abort (82 checks). Same commit as M1 (one state machine). |
| M3 | fixed | dc50665 | `VrBody::Shutdown` now only forgets (never writes) and is called from `VMManager::Shutdown`, `Reset` and `HandleELFChange`; `ClearBlock` writes only with a VM, the disc CRC the block was written under and 'VBD1' still at the block (`VrBody::MayClear`), and a CRC change since the write drops the block first. |
| m3 | fixed | dc50665 | `VrBody::OnStateLoaded` (state-load hook): the next Poll zeroes a block reading our magic, then poses afresh. |
| m5 | fixed | 7a8ad8e | The socket binds the local address a UDP `connect()` to the peer picks (127.0.0.1 for a loopback peer; refused if none), packets must come from the peer's address and port, and an empty or blank `VrPresenceKey` keeps presence off. The loopback test scripts set a key; README notes the firewall prompt. HMAC not done. |
| m1 | fixed | a194123 | New `camera.silence` key `activeGuards` (guest-memory guards on runtime state: patched only while they pass, put back at once when they fail); FadeDim's colour check moved there, `when` keeps the code check. |
| m2 | fixed | a194123 | `CaveDrainDone` counts frames mod 60 while both readings are below 60 (mod 256 otherwise); after a state load the drain start is seeded from the loaded counter at the first drain vsync. |
| m4, m6-m12 | not done (time) | | |

**Offline re-check** (run-nulltest with this build, resources the nightly worktree, no headset, normal speed):
- slot 5, 20 s: 254.6 counts/s of 0x48BF93 (as the morning's 255.6), all seven caves in and read back (A-C 120 words,
  D-F+H 444, G 54, I 31, hand pickup 37, spatial audio 75, VR body 221), and drained 3 vsyncs after their sites at the end;
- slot 7, `pickuptest.py ... 7 0 --vr 0.063,-0.149,-0.78` (the hand 4.5 cm in front of the Handgun's volume): armed,
  the cave took the word, prompt (record 3, item 0) 0.69 s later, confirmed, Handgun in cell 0. Once taken, the tail of
  the confirm press was dropped as the prompt closed (logged, harmless);
- the M1 test above.

Deployed to `run-playtest-models` (previous exe kept as `pcsx2-qt.prev-bb5a6c3.exe`).
