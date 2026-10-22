# Native feel: the game's own systems in first person (2026-10-22, branch `nativefeel`)

Alex's aim: "a one-to-one Resident Evil Outbreak VR, not something thrown over the top". Five changes put more of File #1's
own systems (SLPM-65428) under first person instead of our overlays. Each one has its own `[VR]` switch, on by default,
and its own commit. All of them were checked offline and headset-free only (local build `build-native`, portable install
`run-native`, no OpenXR runtime, head and hands scripted over PINE, normal speed). Nothing here has been in a headset yet.

The maps they come from are the 2026-10-22 surveys (sound, collision, HUD and events). Where this work found a survey
wrong, that is said below.

## 1. Scripted camera shots keep first person (`ScriptedShotsFirstPerson`)

**The game.** The room camera's mode byte u8 `0x3AEF75` is read by the dispatcher `FUN_0058E420`:
- 0: the room's cuts;
- 1: a door;
- 2: an arrival;
- 3: grabbed;
- 4: a fixed cut;
- 5: the camera shake.

**Mode 4** (`FUN_0058EB70(cut, frames)`; script op 0x13 and 30 game call sites):
- It stores the cut in u8 `0x3AEF55` and the frames in u16 `0x3AEF56` (0xFFFF means until op 0x15 lets go).
- Each frame the dispatcher counts the frames down and sets the input lock u8 `0x48C019 = 1`.
- The room camera `FUN_0058E7B0` then picks that cut. It goes through the same three cut handlers that first person's
  silence entries already skip, so the VR eye stays the camera.

**Mode 5** (`FUN_0058EC30`; op 0x16 and 5 sites) is a camera shake, not a look-at. The task `FUN_005A94E0` adds a
table's offsets to the eye's and the target's y (the stores at `0x5A95E4` and `0x5A95F0`).

**Script waits.** Op 0x14 (`0x65B560`) and op 0x17 only test the mode byte, which the game still sets and clears
itself, so they behave as before.

**The change:**
- The camera guard on `0x3AEF75` gains `alsoEquals: [4, 5]` with `alsoWhenSetting: ScriptedShotsFirstPerson`. This is a
  new guard field (VRProfileDB, CameraDriver `GuardAlsoPasses`).
- The shake's two stores are NOPed while first person is armed (offline).
- Modes 1-3 and cutscenes (status 4) keep the world screen.
- The log says `scripted shot (... reads 4) keeps first person` and then `scripted shot 4 over`.

**Checked offline** (slot 4; the shot's own stores poked, `FUN_0058EB70` with cut 0 for 150 frames):
- First person stayed armed for the whole shot.
- The view followed scripted head yaws of +40 and -40 deg.
- The game ended mode 4 itself after 5.0 s (150 frames at 30 per second), cleared the cut to 255 and released its input
  lock.
- Mode 5 poked: first person stayed armed and the eye's y held at 155 (the stores read NOPed).
- No natural mode 4 or 5 turned up in the savestates in 25 s each (slots 1, 2, 3, 4, 8, 9; see the result at the end).

## 2. Head in wall (`HeadInWall`)

**What it is.** A read-only host copy of the player collision (`VR/HeadInWall.cpp`). It writes nothing to the game except
the eye that first person writes anyway.

**The room's faces:**
- Wall grid A (header `0x3938A8`): every face of every cell once, read again when the room or the header changes and
  every 2 s.
- The moving faces (`0x70E750`, count `0x23BEC4`), read every frame.

**Which faces block.** `FUN_005E6050` with the player's mask 0x0001FFFF:
- attribute bit 0x1 set;
- not in the ignore mask `0x3938E8`;
- class 0-15;
- a door wall (class 1) lets you through while `0x4747C1 + slot * 0x60` reads 2 and attribute 0x40 is clear.

**The test.** Like `FUN_005491E0`: one-sided, from the face's front across its plane, the crossing inside the triangle.
- It runs from the head point (the character's position plus the eye height of 155) to the VR eye.
- The eye stops 9 cm before the nearest hit.
- A 10 cm sphere then keeps it off any face it is in front of.
- The line is checked again after that push.
- The floor under it (grid B, the highest upward face below the head point) keeps it 20 cm up.

**The dim.** Past 15 cm of push the view darkens: smoothstep to 0.85 at 35 cm, eased 0.1 s, through `HudCards::SetViewDim`.
That draws with the comfort blink's head-locked black, and the darkest of the blink, the wall and the fade wins.

**The frame.** It is still placed at the real head pose (CameraDriver records the stopped eye as written), so pushing
into a wall feels like the world sliding with you, not a see-through.

**Checked offline** (slot 7, at the counter):
- Room 1 read as a 6 x 6 grid of 300 cm cells: 143 wall faces, 21 floor faces, and 16 moving faces (159 blocking).
- Leaning 0, 20 and 40 cm put the eye 10, 30 and 50 cm ahead of Kevin.
- At 60 and 80 cm it stopped at 54.2 cm (the counter's proxy face is about 63 cm ahead).
- The fade read 0.84 at the 80 cm lean.
- It logged "free again" on the way back.

**Limits:**
- Furniture is the game's vertical box proxies with no tops, so you cannot lean over the counter: the eye stops at its
  front.
- The sphere is only against faces in front of the eye.

## 3. Spatial audio (`SpatialAudio`)

**The game.** `FUN_001B7B50(pos, table, &vol, &pan)`:
- the listener is the camera (in first person, your head);
- volume comes from distance;
- pan comes from the emitter's projected screen x (`FUN_00191A10`: SPR `0x70003F40` times the screen matrix `0x2771E0`),
  clamped 0-640, mirrored when w < 0, divided by 5.0394.

So only sounds inside the view panned at all.

**The cave** (75 words at `0xFC000`, all or nothing in `camera.caves`; the site is the `jal 0x191A10` at `0x1B7C28`):
- It calls the game's projection as before.
- While the host block `0xFF400` reads `'SPA1'`, it replaces x with `322.5 + 320 * xv / sqrt(xv^2 + zv^2)`. That is
  sin(azimuth) in the view `0x3062E0`, where the camera looks down -z. It also writes w = +1.
- It keeps the game's x and w, its own x, xv, zv and a count in the block, for the log.

`VR::SpatialAudio` writes the block offline while first person is armed, and clears it only while it reads its magic.
It never writes it if the RAM there is not free. The site is guarded by `FUN_001B7B50`'s first word.

**Checked offline** (slot 5, the head scripted to yaw 0, 90, 180, -90):
- The cave went in (75 words, read back) and came out 4 vsyncs after its site when the grab disarmed first person.
- For 337 of 337 sounds in front, the game's x and our view-space x agreed on the side. So SPR `0x3F40` holds the view
  `0x3062E0` when `0x1B7B50` runs, which was the survey's open question.
- Sounds behind now pan to their side:

| Azimuth | Our pan | The game's pan |
|---|---|---|
| -130 deg | 15 | 38 |
| -152 deg | 34 | 59 |
| 153 deg | 92 | 92 |

Not done: the ADX streams' pan, the IOP pan law, and the eye lookup at `0x1B7B88` (the listener already follows the head
camera).

## 4. The game's rumble (`GameRumbleEvents`)

**The player.** At `0x2F87D0`:
- +0: the pad can rumble;
- +1: playing;
- +0xC: frames left in the step;
- +0xE: the next step;
- +0x10: the pattern.

A step is 8 bytes: s16 frames, then a pointer to the motor bytes that `FUN_001B8F10` sends (small on, bit 0; large 0-255).
The 9 patterns are at `0x217220`.

**Reading it.** Every vsync, read only:
- large / 255 is the amplitude, and small alone is 0.2;
- a pattern that starts within 300 ms of a shot (our ammo-drop detector) goes to the gun hand only, the rest to both;
- the pad copy (`GameRumble`) stands aside while this is live.

**Our own cues.** Our hurt pulse and a weapon's kick sit out once the game has been seen to rumble for them (a hit, or that
item's shot). The heartbeat, the reload ticks and the dry click stay ours, since the game has none of them.

**Checked offline:**
- The pad is actuator-capable (`0x292D60` = 1, `0x2F87D0` = 1) and the vibration option `0x3065BF` = 1.
- Slot 5's grab played pattern 1 (15 frames, small motor) five times, 1.2 s apart, and logged "both hands".
- Slot 6's 45 Auto fired 3 rounds (7 to 4) with no game rumble at all, so its kick stays ours.

Not felt in a headset. The hurt learning needs a session, because the HP cues run only in one.

## 5. Fades (`FadeDim`)

**The survey was wrong here.** u8 `0x3065C0` with `FUN_001BAB90` is the brightness option, not a fade:
- 128 is neutral;
- the options screen at `0x6BA130` steps it up to 163;
- above 128 is a white quad and brighter, below 128 a black one.

It is left alone.

**The real fade** is `FUN_001B9DA0`'s quad, drawn every frame from the 2D passes (`0x341B08`, `0x341E80`):
- it is drawn while u32 `0x308894` is non-zero;
- its alpha is 255 x f32 `0x30889C`, with 0x80 opaque;
- its colour is u32 `0x308898`;
- the starters are `FUN_001B9BA0` (to the colour) and `FUN_001B9C30` (back), stepped by the task `FUN_001B9A60`.

**The change:**
- While first person is armed and the fade is black, the view is dimmed by `min(1, 255 x level / 128)`: head-locked,
  eased over about 40 ms, through the same view dim as the wall.
- The quad's on check (`0x1B9DCC`) is patched to skip the draw while first person is armed and the colour reads black
  (offline).

**Checked offline** (slot 4): a fade to black and back, stepped as the fade task does (30 game frames each way):
- first person stayed armed;
- the quad's on check read patched;
- the build logged the fade from level 0.03 to darkest 1.00, and its end.

The dim itself cannot be seen without a session. Its mapping and the darkest-wins rule are self-tested.

**DoorVeil** (branch `grabdoor`) also goes through `HudCards::CurrentBlink`. With both merged, the darkest wins.

## Self-tests

45 of 45 locally. New:
- Head in wall: 20 checks (segment, one-sidedness, closest point, the stop, the sphere, a corner, the floor, the fade,
  the filter and door rule, a synthetic grid with shared faces and three refusals).
- Spatial audio: the pan law.
- Feedback: the rumble mapping.
- HUD: the fade mapping and the view dims.
- Scripted shots: the profile's guard and the shake entries.
