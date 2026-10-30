# Native hands: the game draws your hands and the weapon it holds (2026-10-30)

Branch `nativehands` (from nightly 78fe8ca). Evidence words: **verified (code)** = read from the game's code (disassembled
from a live RAM read of File #1, slot 7) or from this repo's source; **checked offline** = seen in a headset-free run of the
dev build (scripted head and hands over PINE, no OpenXR runtime, normal speed); **not checked** = not done. Nothing here has
been in a headset.

## The problem

The VR hands and the held weapon were pictures: `HandModel` rasterises them on the CPU into a 256 x 256 image and
`XRCompositor::BuildHandLayers` submits each as an `XrCompositionLayerQuad` with `eyeVisibility BOTH`, so both eyes see the
same flat image, there is no stereo depth on the weapon and nothing in the room can hide it. And with `VrBody` +
`VrBodySelf` the game also drew Kevin's own forearms and hands (posed to the controllers by `VrBody.cpp`, through TrueStereo,
so real 3D in stereo, lit by the game and depth-tested): two sets of hands. The design here: let the game draw the hands AND
the weapon, and stop building the overlay for what the game draws.

## 1. How the game puts the held weapon on the hand (verified (code); disassembly of 0x6313A0 and 0x560B60 from slot 7)

Three separate things decide where you see a held weapon, all in the player's tick or the draw:

1. **Attach, per tick, in the world build.** `FUN_00346810` (the characters' world build, at `0x5CDEB0`) runs for each record
   after the bone worlds are rebuilt: `FUN_006313A0(record, item slot byte, bone)` with the item slot at record `+0xC7C`
   (the right hand, bone **19**, the right wrist of arm 16-19) and `+0xC7D` (bone **15**, the left wrist). It:
   - returns when the slot byte is 0, the item has no object (`*(0x3967EC + 60 * item)` is the item's weapon object) or the
     bone's struct is missing;
   - **copies the bone's world matrix** (bone struct `+0x00`, 0x40 bytes) to the object's matrix `+0x1C0` and normalises its
     three axes (`FUN_00191750`, so the character's scale 1.05 drops out);
   - reads the item's weapon type (`FUN_005C1210`), the type table entry `0x6E5940 + 0x44 * type`, and a variant by the
     record byte `+0xB99` (0 = A, 1 = B): the **grip** f32 x 3 at entry `+0x10 + 12 * variant` (times the character's scale
     `+0xB4 / 0xB8 / 0xBC`) and the **turn** s16 x 3 at entry `+0x28 + 8 * variant` (Euler, `0x10000` = a turn), makes the
     matrix R with `FUN_0018FCA0`, puts the grip in row 3, mirrors it for the left hand (bone 15), and sets
     **object `+0x1C0` = (R with grip) x (hand world)** with the game's multiply `FUN_00190C50` (out = left x right, row
     vectors), then the position `+0x200` from the matrix's row 3 (`FUN_0018FAB0`);
   - a special case for `record +0x54F == 0x80` (type class 0xC in a raised stance) and a different Euler path when the
     entry is not the usual one. Not needed here.
   So the held object's matrix is **the bone's world, turned and offset by a per-type entry**: this agrees with the
   2026-10-14 note (grip and turn, measured to 0.01 cm on twelve savestates and live).
2. **Used by the shot** (`FUN_0062CF10`, the player's tick, after the world build): the object's `+0x1C0` and `+0x200` give
   the bullet's frame and the muzzle (`2026-10-14-vr-melee.md` section 13). That is why cave O fakes an object there.
3. **Drawn later**, in the draw: the character bodies (`FUN_00345210`, skinned from the bones' WORLD matrices at the draw),
   then the held and attached objects (`FUN_00560B60`, object list `gp-0x5EB8`, flag `0x20000` at `+0x23C`, owner byte
   `+0x240`): for each it takes the owner record `0x476DD0 + 0x10E0 * k` and, for the camera's **focus character** (record
   `+3` equals the focus byte `0x3AEF74`, the local player), the profile's near-cull patch **skips the object**:
   `0x560C74` is `beq v1, v0, <next object>` instead of `beq v1, v0, 0x560C98` (draw). So in first person your own
   held weapon is **not drawn at all** today by the game; only the overlay picture shows it.

### What `VrBody` did with these

- `GameShoulders` (cave U, locals): the posed bones feed the game's own world build, so the item sits in the posed hand and
  the hit spheres move: not draw-only, and still skipped at `0x560C74`.
- `Forearms` / `Arms` (anchored, since 10-27): the slot is **draw-only** (cave V's world entries: flags self + collapse,
  never cave U's). Cave V's flag 1 (the attach: it calls `FUN_006313A0` again after the draw-time worlds, its first word
  checked) was **not** set for it, so the item's matrix stayed the animation's (and, skipped at `0x560C74`, was never drawn).

### The answer for the design

A draw-only held weapon needs exactly two things, both small:

- **Matrix**: cave V's own attach (flag 1) on the anchored slot. It runs `FUN_006313A0` with the posed wrist bone 19 world
  that cave V has just written at the draw, i.e. the game's own derivation from the *posed* right-hand bone, with the
  game's attach offset. It rewrites the object's `+0x1C0` / `+0x200` at the draw; the next tick's world build rewrites
  them from the animation before anything reads them (the shot, the effects and the hit tests run in the tick after the
  world build; the draw runs after the tick). So the hit tests and the game's own shot never see the draw-time matrix.
- **Draw**: let `FUN_00560B60` draw the focus's held object while the host says native hands are live: the `beq` at
  `0x560C74` becomes a `j` to a small cave (in the free tail of cave V's range, `0xFD378`) that takes the old skip unless
  the host block's slot 1 names this record with a new flag bit (`0x10`, "weapon native").

Step 2 (next) builds exactly this. Free RAM: only VrBody's own `0xFD000-0xFDFFF` (the cave's tail `0xFD374-0xFD3C4`) and
its host block `0xFF300` (flag bit `0x10` of slot 1's flags byte): nothing from the spare ranges.

## 2. The draw-only held weapon (built; checked offline)

- **The matrix.** The anchored self slot (`Forearms` / `Arms`) now sets cave V's flag 1 (the attach: `FUN_006313A0` on the
  record's item slot `+0xC7C` and the posed wrist bone 19, at the draw, after cave V wrote the posed worlds; the left item's
  slot is attached only when the left arm is drawn). Without NativeHands the attach offsets are zeroed, as the draw-only
  slot never attached.
- **The draw.** `FUN_00560B60` skips the focus character's held object at `0x560C74` (`beq v1,v0,<next object>` instead of
  `beq v1,v0,0x560C98`). That site is now `j 0xFD378` (cave **W**, 19 words in the free tail of cave V's range
  `0xFD374-0xFD3C4`, so cave V's all-or-nothing install, read-back, drain and "sites after the body" rules cover it: the
  range is now 241 words): not the focus -> the old flow (`0x560C7C`, the cull test); the focus -> `0x560C98` (draw) only
  while the host block's slot 1 names this record and its flags byte has the new bit **0x10**, else `0x560D38` (the old
  skip). Registers used: t0-t4 only (dead at the site); sp and s0 untouched (an interpreted self-test checks).
- **Profile.** The new entry (`whenSetting: NativeHands`, offline only, `off` = the skip's word) sits BEFORE the old skip
  entry: the old entry patches the game's word first, the new one takes the word over while its cave is in and gives it
  back when the cave goes, so a disarm restores W's site, then the skip's own restore puts the game's word back (checked
  live: `560C74` reads `0803F4DE` armed and `10620008` after the game's own cut disarmed first person, with the block
  zeroed). `body.selfSite` / `weaponSite` (and their `On` words) let the host check both sites before it trusts them.
- **Free RAM.** Only VrBody's own `0xFD000-0xFDFFF` (cave W's 19 words at `0xFD378`) and its host block `0xFF300` (bit
  0x10 of slot 1's flags). Nothing from the spare ranges.
- **Draw-only.** The draw-time matrix stays in the object until the next tick's world build rewrites it from the
  animation; the shot (`FUN_0062CF10`), the effects and the hit tests run after that build in the tick, the draw after the
  tick. Checked: at the vsync the held object's matrix was the posed one, and the game's own attach maths on the posed
  wrist bone reproduced it (below).

### Checked offline (run-nhands, local non-LTO build, PINE slot 28200, normal speed, no OpenXR runtime)

`nativehands_test.py <install> sweep [slot]`: the scripted head at 1.2 m, the right hand through 7 poses (aim forward, low at
the lap, a gun near the face, at the face turned 40 left and 20 up, out to the right turned 60, far forward pitched down 35,
raised high pitched up 50), `PCSX2_VR_NATIVE_LOG=1`. The weapon is Kevin's 45 Auto (item 87, type 7). At each pose the host
reads the held object's matrix at the vsync after the draw:

| Pose | game's attach maths on the posed wrist vs the drawn matrix | drawn weapon vs the controller's own placement (origin / axes / muzzle) | drawn barrel vs the controller's forward |
|---|---|---|---|
| all 7, slots 7 and 9 | 0.00 cm / 0.00 deg | within 0.32 cm / 0.02 deg / 0.32 cm | within 0.02 deg |

- Before the wrist's swing limit was widened (75 -> 90 deg for your own anchored hand only, `kSelfWristSwingDeg`) the gun
  held near the face came out **2.47 cm and 5.73 deg** off the controller (the pose needs about 81 deg); the elbow and the
  wrist target themselves were on target to 0.0 cm. With 90 deg that pose is 0.00 cm / 0.00 deg.
- The block's flags read `0x17` (self, collapse, attach, weapon) and `560C74` read `0803F4DE` in every sample; the first
  sample after a load (before the pose settled) showed the animation's matrix, 40 cm from the controller, as expected.
- "Kevin's `45 Auto` (item 87) in slots 7 and 9" are the same item; there is no pipe in slot 5 or in the other slots here, so
  a drawn melee weapon is **not checked** (the maths are the same function; the spray's turn is in the self-test).

## 3. One set of hands (built)

- **Decision** (`VrBody::NativeHandsUpdate`, shared state, called from Poll each vsync and again from `BuildHandLayers` each
  present): the host publishes per VR hand whether the game draws it (the anchored arm solved and drawn, `VrBodySelf` live,
  the two sites and the attach function in place) and whether the right hand's weapon is the game's. A hand is "native"
  only when that raw state is fresh (at most 150 ms), the gate holds (first person armed, no menu: `SpatialControls::MenuMode`)
  and it has stayed live for **0.2 s**; the moment the raw state or the gate drops, the hand is the overlay's again **that
  frame** (no frame without a hand); a pose that flaps every 100 ms never takes over (self-tested).
- **Compositor.** In `BuildHandLayers` a native hand's `HandState` is not built (so not rastered, not placed, no quad, no
  hand attach for the pacer), counted in `(VR) NativeHands: N overlay hand frames/s left out ...` next to the Hands line.
  The laser, the holster and sling markers and the menu pointer are other cards and stay (the beam now starts at the drawn
  muzzle, see 4). `ApplyModelLook` loads the weapon's attach for the laser even with `GameWeaponModels` off.
- **Config.** `[VR] NativeHands` (default true, ini only). The `Config.h` comment on `VrBodySelf` no longer claims the
  arms replace the hand models by themselves: with `NativeHands` they do; without it both are drawn.
- **Checked offline** (`nativehands_test.py <install> dropout`): the left and right hands become native 0.2 s after the pose
  is live (`(VR) NativeHands: the right hand is the game's own (the overlay hand is no longer submitted).`), the right
  controller untracked for 1 s three times: the overlay hand is back at the first poll without a pose (`... is the overlay hand
  again (the native pose dropped out).`), the native hand again 1.2 s after the controller returned (0.2 s hold plus the
  harness's own timing); at the end both dropped at once. A cutscene drop (the game's cut in slot 6, ~25 s) took the sites,
  the block and first person away together (above).
- **Not checked.** The compositor's own skip and the stat line, because a headset-free run has no compositor (the decision and
  the log lines run from Poll); the hand frames/s would show in a session.

## 4. Pose and gameplay agree (measured, then fixed)

What the gameplay used before this, against what the game draws (`PCSX2_VR_NATIVE_LOG=1`, the gun ray's own
`LastShotRay`, 7 poses, slots 7 and 9, the 45):

| Gameplay side | start of the shot vs the drawn muzzle | direction vs the drawn barrel |
|---|---|---|
| Before: `GameFromVr` of the overlay model's muzzle | **9.5-13.3 cm** | 0.0 deg (pistol) |
| - of which the eye's shift | 10.0 cm: the game's eye is `eye_forward` (10 cm) ahead of `GameFromVr(head)` along the view, so everything the eye sees is drawn that far on; the anchored arms add it, the gun ray never did (7 cm sideways for a shot 45 deg off the view) | |
| - of which the overlay hand's anchor | 2.21 cm (the 45's origin, overlay placement against the posed wrist 6.3 cm behind the grip point; self-test) | |
| - of which the clamp | 2.5 cm / 5.7 deg when the wrist swing limit (75 deg) clamps the hand: the controller no longer says where the drawn gun points | |
| **After** | **0.00 cm** in all 7 poses (0.30 when the hand is stretched far forward and the arm stops short: the same resolved pose) | **0.00 deg** |

More than the brief's 2 cm / 3 deg, so both now derive from the same **resolved grip pose** (the posed wrist plus the
game's attach entry): `VrBody::NativeWeaponNow` (published by Poll from the SOLVED, clamped wrist frame through
`AttachWeapon`, the same maths the game runs, read here from the type table: grip `+0x10 + 12 * variant`, turn `+0x28 + 8 *
variant`, muzzle `+0x38`, variant by record `+0xB99`; File #1) in game coordinates with the eye's shift, and in VR space.
- **GunRay** (cave I's shot frame and cave O's fake object): `og` / `dg` are the drawn muzzle and barrel when the hand is
  native (the wall test, the back offset and everything after are unchanged). `PCSX2_VR_NATIVE_NOSHIFT=1` keeps the old start.
- **PhysicalMelee**: the strike's weapon shape (the override block's matrix) is the drawn object's origin and axes; the fists
  (no weapon, a gun's butt) use the controller's own point plus the same shift.
- **Laser**: the beam leaves the drawn muzzle along the drawn barrel (`HandModel` has a `native` placement:
  `PlaceModel(..., native)`, the wrist `kNativeWristBackCm` behind the grip point, no turn onto -Z).
- **Long guns.** The game holds them a few degrees off the bone (SMG 7.7 deg, shotgun 10.9 deg, grenade launchers more: the
  attach entry's turn; the self-test checks the shotgun's 10.9). The drawn barrel is the game's, so the shot and the beam now
  follow the barrel, not the controller's forward, for those. Pistols (turn 0) are unaffected. Not checked in the game
  (the only weapon equipped in the saves is the 45).
- The overlay path (`NativeHands` off, or the pose dropped out) is unchanged, including the eye-shift error of the shot start
  (found here, not fixed there: it only makes a shot up to 7 cm sideways of the laser).

## 5. Hit-stop

`PhysicalMelee::StartHitStop` is skipped while the right hand is native (`(VR) Melee: no hit-stop on the hand picture (the
game draws the hand: NativeHands); the overlay path keeps it.`, once) and `BuildHandLayers` / `HandsNow` no longer apply an
old hit-stop to a native hand; the overlay path keeps it. Checked offline: a melee run logged the line and the strikes still
landed.

## 6. Checked offline: the game still works (slot 5, the 45 Auto, `NativeHands` on)

`nativehands_test.py <install> shots|melee|close 5`:
- **Shots along the laser**: the head turned to the zombie, the controller aimed from its own position at the enemy's chest: the
  shot at enemy 49 (0.55 m, head turned -131 deg) **hit, HP 600 -> 150**, three runs out of three; the resolved pose was
  0.00 cm / 0.01 deg from the controller's own placement there; the shot at enemy 48 missed, **as it does with `NativeHands`
  off** (the same run, same numbers; enemy 48's drawn-this-frame byte read 0 in 80 of 80 samples: it is not on screen), and
  the 50 deg over shots missed (2 of 2 in each of the three runs).
- **A melee strike still lands**: a fast gun-butt swing (`PhysicalMeleeFists`) through the zombie: hits at 3.6-6.7 m/s,
  `Melee: hit, enemy 49 HP 90 -> 60`, `enemy 46 HP 680 -> 629`, ... (3 of 6 swings in the first run, 4 in the last; the misses
  were swings made before the zombie was within reach, as with the overlay). The shape is the controller's point plus the eye's
  shift (the fists) or the drawn weapon's matrix.
- **A close zombie still draws**: enemy 49 at 0.30-0.8 m: its drawn-this-frame byte (`+0x198`) set in 80 of 80 samples
  (50 ms apart) with the hands native.

## 7. Self-tests (69 of 69 suites; `VR body` now 79 checks)

The hold (200 ms, not 199; a drop is immediate; 200 ms again after it; a 100 ms flap never takes over; each hand its own; a
bad hand or no state never), the update (fresh but not yet held; both hands and the weapon after 0.2 s; the gate or a
cutscene gives the overlay back that frame; a host that stopped publishing is no native hand; only the right hand tracked;
nothing live at the end), the slot flags (weapon only on the local draw-only slot, never with locals, never the partner's;
a free bit), the attach maths (the 45's origin and barrel and muzzle; the shotgun's barrel 10.9 deg off the bone; the
spray's axes a rotation) and **VrBody's attach agreeing with HandModel's native placement** for the 45, shotgun and spray
(origin 0.0000 cm, axes under 0.05 deg, muzzle and barrel the same), cave W interpreted from the profile's own words
(the focus + flag draws, not the focus keeps the old flow, no flag / another record / the partner's flag / no magic skip,
sp and s0 untouched; the anchored self slot's V world write, one attach on bone 19 with the item slot byte, then the draw),
the profile's cave range (241 words, usable) and sites (`body.selfSite`, `weaponSite`).

## 8. Not checked, risks

- **The headset**, and every look: how Kevin's own hand and weapon read in stereo (the gun's depth against the counter, the
  hand's size and angle), the 90 deg wrist, the laser starting at the game's muzzle, and whether the game's own muzzle flash
  and the slide animation show (the game's weapon object has no slide: the overlay pistol's slide cycling is gone with the
  overlay, and the game's recoil is the hidden character's arm).
- The compositor's own skip and its stat line (no compositor headset-free); a long gun, a melee weapon (no pipe in the saves),
  a grenade or can in the hand; the weapon lowered; a left-hand item; co-op (NativeHands is offline only, like `VrBodySelf`).
- The overlay hands remain the answer to every state the game is not drawing: a menu, a cutscene, a grab that takes first
  person away, an untracked hand; the 0.2 s hold applies when they come back.
