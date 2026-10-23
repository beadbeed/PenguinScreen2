# Line of sight from the game's own wall classes (branch `losfix`, 2026-10-22)

Night review m10 (HandPickup takes an item through a wall) and late review m6 (GunObjectAim spawns the shell and
`FUN_006801A0`'s object at a VR muzzle beyond a wall). The first try (2c41f81, 6a260a1, reverted in 54d2fdc) tested
against the player collision (attribute 0x1) and refused the Handgun on J's Bar desk, because the player's collision
fences furniture at full height. This note finds what the game itself treats as an occluder and builds on that.

Labels: **verified (code)** = read in the disassembly of `ram_now.bin` (J's Bar, GAME overlay resident);
**verified (memory)** = read in the nine File #1 savestates' EE RAM (`truevr/f1_01`..`f1_09`, extracted earlier);
**verified (offline)** = seen in a headset-free run; **assumed** = inferred.

## 1. The face filter and its masks

`FUN_005E6050(face, maskA, maskB, door_flag)` decides whether a face counts for a ray (verified (code)):
- the face's class (its byte `+0`) must have its bit in maskA's low half (one bit per class 0-15);
- the face's attribute bits (flags `& 0xFFFF0000`) must meet maskA's high half;
- maskB's classes and attributes exclude, then the room-wide ignore mask `0x3938E8`;
- a class-1 face (a door wall) passes when its door slot `(attr >> 8) & 0xF` reads 2 at `0x4747C1 + 0x60 * slot`,
  unless the attribute has 0x40.

Every caller of the ray `FUN_005494C0` (53 sites) and of the any-hit ray `FUN_00549860` (25 sites) passes maskB 0 and
one of a few maskA values (verified (code), the `lui`/`ori` pairs before each `jal`). The ones that matter:

| maskA | Who | What it is for |
|---|---|---|
| `0x0001FFFF` | movement (`rec+0x190`) | the player's collision (what HeadInWall copies) |
| `0x0002FFFF` | the attack capsule's wall test, `FUN_005AAE60` at `0x5AB154`; also `0x5AA704` and the line shape `0x5AB5BC` | **shots and swings stop here** |
| `0x0004FFFF` | the perception cache `FUN_006A3B60` at `0x6A3F04`; also `0x67CEEC`, `0x68ADA0`, `0x68ADF0`, `0x68AEAC`, `0x68AEFC` | **the zombies' line of sight** |
| `0x0020FFFF` | enemy movement and enemy probes (`0x60E448`, `0x602BE8`, ...) | enemies' collision |
| `-1` | the character action probes (`0x629C30` ... `0x62B880`, `0x6A0010` ...) | everything |

### The zombies' sight (verified (code))

`FUN_006A3B60` (once a tick) fills the cell `0x385490 + 400 * character + 8 * enemy`. Its `+6` byte "blocked" is 1 past
3000 cm, else `FUN_00549860(chest, enemy point, 0x0004FFFF, 0, &material)`: the start is the character's bone 11 (the
chest: the model at record `+0x34`, the bone matrix pointer at model `+0x84C` = `+0x820 + 11 * 4`, its `+0x30`), the end the
enemy's bone `+0x551`. The senses `0x604D40` skip sight (hearing only) when that byte is set. So the game's own line of
sight is attribute bit 0x4.

### The shot (verified (code), and the table verified (memory))

The gun's attack (a type-1 object, `FUN_005521B0` from `FUN_0062CF10`) is tested by the capsule tester `FUN_005AAE60`:
- it builds the segment (P to P + D x length through the object's matrix);
- if the node's `+0x19` has bit 0x10, it casts `FUN_005494C0(start, end, node+0x1C, normal, 0x0002FFFF, 0)`;
- on a hit it **moves the segment's end to the hit point** (`0x5AB1F0`-`0x5AB208`) and sets node `+0x1B`;
- then the props (`FUN_0054D420`) and the enemies (`FUN_00549EA0`) are tested on the shortened segment.

The node's `+0x19` is copied from the attack params' `+0x0C` (`FUN_005A9960` at `0x5A9A7C`, table `0x6E7C90 + 32 * id` for
kind 0). In memory ids 0-26 (the guns among them) and 29-39 (31-39 are the melee weapons) read `+0x0C = 0x12`: bit 0x10
set. Ids 27 and 28 read 0x02. **So a bullet stops at faces with attribute bit 0x2.**

### The camera

No collision call is reachable from the room camera (the RE atlas, section 2f). Nothing to learn there.

### The second grid

`0x3938C8` is the floor grid B (upward faces; `FUN_005E5B40`), not an occluder set.

## 2. The face classes in File #1 (verified (memory), all nine savestates)

| Attribute | Bits | Faces | What they are (top-down plot of J's Bar and the bathroom) |
|---|---|---|---|
| `0x2F` | 0x1 0x2 0x4 0x8 0x20 | J's Bar 71 + 2 class-1; bathroom 31 + 2 class-1 | the room's walls and the corridors' walls |
| `0x12F`, `0x22F` | the same, door slots 1 and 2 | J's Bar 2 + 2, class 1 | the door walls (shut: their slots read 0) |
| `0x29` | 0x1 0x8 0x20 | J's Bar 66, bathroom 27 | furniture: the counter, the tables, the piano, the bathroom's sinks and stall fronts |
| `0x27` | 0x1 0x2 0x4 0x20 | the 16 moving faces in J's Bar (none in the bathroom) | two boxes of 8 faces at the lower wall (assumed barricades or props) |

So the game's own classification is exactly what we need: room walls stop sight and shots, furniture does not.

One more `0x29` place (verified (memory), J's Bar, every slot): the lower wall z 13650 has a 1 m stretch, x 7950-8050 from
the floor to y 225, whose two triangles are `0x29`; the wall above it and to both sides is `0x2F`. The player cannot pass it,
sight and bullets can (assumed: the entrance's glass door or a window). A line through it is clear for both checks, as it
is for the zombies.

The desk face that refused the Handgun in slot 7 (flags `0x00290000`, normal +Z) is furniture: it counts for the player
mask and for neither sight nor shot.

A replica of the filter and the one-sided ray on the savestates' grids (scratch scripts, not committed) gives, from the
local character's head (position + 155) to every floor item's volume centre in all nine savestates (5 items in slot 1, 4
in slots 2-7, 2 in slot 8, 1 in slot 9): the player mask is blocked every time, always by a `0x29` face; the sight mask
never. In slot 7, head to the Handgun's search point (8493.8, 140.0, 12939.9): player mask blocked at 73.7 % of the line
by `0x290000` (the reverted try's face, 23 cm before the point), sight and shot masks clear. In slot 5, Kevin's chest
(bone 11, (7944.8, 128.8, 13601.1)) to a point 20 cm past the wall behind him (z 13650): blocked by `0x2F0000` at 71 % for
all three masks; 10 cm short of the wall: clear.

## 3. Built

- `HeadInWall::FaceCounts(flags, maskA, ignore, door_open)`: `FUN_005E6050` with maskB 0, any maskA (BlocksPlayer is now
  `FaceCounts(..., kMaskPlayer, ...)`). `kMaskPlayer 0x0001FFFF`, `kMaskShot 0x0002FFFF`, `kMaskSight 0x0004FFFF`.
- `HeadInWall::SegmentBlockedBy(faces, maskA, ...)`: the nearest face the segment crosses that counts (pure).
- `HeadInWall::LineBlocked(crc, a, b, maskA)` (CPU thread): its own copy of grid A (the same reader and refresh as
  HeadInWall's eye clamp, independent of the HeadInWall switch) plus the moving faces each call. Unknown when not File
  #1, not in game or the grid does not read.
- `HeadInWall::CharacterChest(crc, record, out)`: bone 11, where the game's sight starts.
- **HandPickup** (`[VR] HandPickup`): the line from the character's head point (the camera mapping's position plus eye
  height) to the search point, sight mask. Blocked: the item is not highlighted for the squeeze, logged once per item
  (`(VR) Hand pickup: item N (...) is behind a wall from the character's head ...`).
- **GunRayAim / GunObjectAim** (the gun, the same test for both): the line from the chest to the VR muzzle, shot mask.
  Blocked:
  - cave I's shot starts on the chest's side of the wall, 1 cm before where that line meets it, and goes along the line
    into the wall (`GunRay::WallShot`, then the usual `ShotFrame` with the game's back offset). The faces are one-sided
    and face the room, so the game's own capsule wall test (`0x5AB154`) now meets the wall from its front and cuts the
    shot there. A shot from behind the wall would not have been cut;
  - cave O's object block is not written, so the game's own gun object takes the flash, shell and `FUN_006801A0`; our
    own flash stays (the flash evidence needs cave O's count);
  - logged on entry and exit (`(VR) Gun ray: the VR muzzle ... is beyond a wall ...`).
  Cave I's other rules are unchanged: the same block, focus and stamp checks, the same offline gates; a wall shot that
  cannot be built leaves the block unwritten (the game's own shot).
- No new setting: both checks live inside the existing switches. With `PCSX2_VR_PICKUP_DEBUG` HandPickup also logs the
  line's ends and verdict once a second (2ccb684).
- Self-test (in the HeadInWall suite): the masks on 0x2F, 0x29, 0x27 and a door wall; the slot 7 desk case (seen past the
  furniture, the player mask would have stopped it); an item behind a room wall; a muzzle past the wall and short of it;
  one-sided, NaN, round the wall's end.

## 4. Checked offline (run-losfix, headset-free, no OpenXR runtime, normal speed, `HandPickup = true` in that install only)

Self-tests: `vr_selftests`, 62 suites, all PASS (verified (offline)).

The four checks below (slot 7, slot 3 behind and in front of the wall, the slot 5 gun cases) were run again at about
21:25 on the final build (21b0de1's code) with run-losfix on its own PINE slot 28123, after another agent's script may have
reached the shared default slot 28011 around 21:04. Each run's log shows one PINE client and only its own state loads;
the results are the same as listed.

| Case | Result |
|---|---|
| Slot 7, `pickuptest.py ... 7 0 --vr 0.063,-0.149,-0.78` | Highlighted, armed, the cave searched at the hand, take-it prompt confirmed: **cell 0 holds record 3, Handgun**. No wall line. |
| Slot 3, the First Aid Spray's trigger (node 0x3B6158) moved to x 7930-7970, z 13655-13685 (5-35 cm behind the room wall z 13650 behind Kevin), the hand in it, squeezed 6 s | `item 309 ... is behind a wall from the character's head: not taken by hand (face 0x002F0000 at 79% of the line from (7948.2, 154.9, 13600.0) to the search point (7940.0, 120.0, 13662.5))`; nothing armed, **cells stay 0 0 0 0**. |
| Slot 3, the same trigger moved in front of that wall (z 13615-13645) | line clear, armed, **taken (cell 0 holds record 4)**: the refusal above is the wall, not the move. |
| Slot 5, the same move behind the wall | line **clear**: Kevin stood at x 7956 there, and the line crossed the wall at x 7951, inside a 1 m opening (x 7950-8050, y up to 225) whose faces are `0x29`. The game's own sight and bullets pass that opening too (see section 2). The take did not happen anyway: a zombie grabbed Kevin first. |
| Slot 5, GunObjectAim, `head` and `offset` (the gunobj cases), twice | **hit, 600 -> 150** every time; cave O counted every shot; the game's flash at the VR muzzle, 0.00 apart. |
| Slot 5, the hand 0.9 m behind and left (muzzle (7909.2, 132.8, 13689.6), 40 cm past the solid wall) | `the VR muzzle ... is beyond a wall from the character's chest (7950.1, 127.3, 13582.7) (face 0x002F0000 at 62% of the line); the game's own gun object until it is back`, then `back on the character's side` after. **Cave O did not take the shot** (the object block cleared, its count 0): the game's own object did. Before the cave I change below, cave I's bullet still hit (600 -> 150) from beyond the wall. |
| Slot 5, the same pose after the cave I change, twice | the fallback line, cave I's shot counted from (7948.7, 126.2, 13620.8) along the chest-to-muzzle line (-0.58, 0.06, 0.81): **no hit, 600 -> 600** both times. The `head` and `offset` shots in the same run: **hit, 600 -> 150** (three shots). |
| Slot 5, the muzzle 11 cm past the wall at x 8052 (the line through the opening) | not walled, cave O took the shot (game-consistent: the opening is `0x29`). |

While the hand was on the moved trigger in slot 5, the gun check also fired with the hand's pose: `the VR muzzle (7936.2,
126.7, 13674.6) is beyond a wall ... (face 0x002F0000 at 66%)`.

## 5. Open

- The moving faces' meaning (0x27) and attribute bit 0x8 (on walls and furniture, not on the moving faces) are not known.
- An item or muzzle seen through a `0x29` opening (J's Bar's lower wall, x 7950-8050) is allowed, as the game's own sight
  allows it. Whether that is the door to the street was not checked.
- Other rooms than J's Bar and the bathroom were not surveyed. The code reads whatever the room's grid holds.
