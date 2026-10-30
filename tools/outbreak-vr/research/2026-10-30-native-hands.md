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

