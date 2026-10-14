# The game's weapon models in the VR hand: how the game attaches them (2026-10-14)

Branch `handmodels`. `[VR] GameWeaponModels` (on by default) draws the right hand's weapon, the holstered weapon and the
slung long gun with the game's own model (`VR/GameModels`, decoded from the disc at VR start, memory only), and both hands
with the CC0 hand model (`VR/HandMesh`), through `VR/MeshRaster` into the impostor images `HandModel` already makes.
Off, everything is drawn exactly as before (a self-test compares the images).

This note holds no game data beyond addresses, layouts and a few measured numbers. Nothing extracted was committed; the
image dumps stayed in `D:\dev\PenguinScreen2-build\run-models\handdump`.

Evidence words: **verified** = read from the game's memory or code, or measured; **checked offline** = seen in a
headset-free run of the dev build; **assumed** = reasoned, not measured.

## Short answer

- **The game attaches every held weapon to the right hand bone with a per-weapon-type entry of a table.** It does not
  fit or animate the hold: the held object's world matrix is `(vertex * R + grip) * hand`, in the game's row-vector
  convention, where `hand` is the character's right-hand node matrix, `grip` a point in that bone's frame and `R` a turn.
- **Where:**
  - the local record's u16 `+0xCA8` is the equipped weapon's **type**. The game writes it on equip from the scenario's
    item table: `0x5C1240` -> `0x584800` reads the item record's byte `+0x2B` (an index `k`) and returns the s8 at
    `0x71698F + 24 * k`, i.e. the 24-byte record at `0x716980 + 24 * k` has the type at `+0x0F` and the item id at
    `+0x12` (**verified**, code and memory);
  - the **weapon-type table** is at `0x6E5944` (GAME overlay data), `0x44` bytes an entry, its byte `+2` its own index
    (**verified** for the 60 entries present):

    | Offset | What |
    |---|---|
    | `+0x0C` | f32 x 3: `grip`, the model's origin in the hand bone's frame (game units, in the skeleton's scaled frame) |
    | `+0x18` | f32 x 3: a second grip (variant B, see below) |
    | `+0x24` | s16 x 3: `R` as Euler angles X, Y, Z, `0x10000` = 360 degrees, `R = Rx * Ry * Rz` (X applied first) |
    | `+0x2C` | s16 x 3: a second turn (variant B) |
    | `+0x34` | f32 x 3: a gun's muzzle in the model's own space (zero for melee weapons and some guns) |

  - the hand bone is node 19 (the wrist) / 28 (the hand group, same transform) of the 36-node character skeleton
    (`romdata.afs` `P00_00.nbd` for Kevin). Its frame in the bind pose: `-X` from the wrist toward the fingers, `-Y` the
    palm, `+Z` the thumb's side. The skeleton's own scale (Kevin 1.05, Mark 1.10, in the node matrices) applies to
    `grip`; the weapon is drawn at its own size, 1 unit = 1 cm.
- **Checked:**
  - **twelve File #1 savestates** (Alex's slots 1-9 and three backups, extracted to the scratchpad): every held weapon
    (Kevin's 45 Auto, Mark's Handgun, Mark's Pesticide Spray) sits exactly at its entry's grip and turn on the owner's
    hand node, offsets to 0.01 cm, all lowered stances. The spray's turn, (101.8, -23.6, -77.7) degrees, reproduces the
    held matrix to 0.001 only as X then Y then Z (all 48 orders and signs tried);
  - **live, weapon raised** (slot 6, headset-free, scripted holster squeeze, 23 samples over 6 s, stance byte 2): Kevin's
    45 Auto's matrix matched variant A on the wrist node to 0.00-0.05 cm and 0.002 in rotation in every sample;
    variant B was 2.9 cm off;
  - **first person keeps computing it**: the samples above were in first person, so the held object's matrix is
    updated even while the near-cull patch skips drawing it.
- **Variant B** (`+0x18` / `+0x2C`) is not what the game used in any sample (lowered or raised). Unknown: maybe another
  stance or body type. Not used.
- **Melee and the rest (assumed from the geometry):** with variant A every melee shaft passes 1.1-1.4 cm from a point in
  the closed fist (8 cm out from the wrist, 2.5 cm to the palm side) and runs along the bone's Z axis, across the fist,
  thumb end to little-finger end, as a stick held in a fist does; cans stand in the fist, the molotov is held by its
  body. Not checked live: equipping another weapon needs a menu confirm.

## In VR

The bone frame goes onto the hand model (`HandModel.cpp`, "GameWeaponModels" section):

- **Axes:** the bone's `-X` (fingers) on the pointing frame's `-Z`, `+Z` (thumb) on `+Y`, `-Y` (palm) on `-X`: the hand
  model's own layout, a proper rotation (`(x, y, z) -> (y, z, x)`).
- **Origin:** the bone's origin (the wrist) on the hand model's wrist anchor (`HandMesh::GetAnchors`, the pistol pose for
  guns, the fist for the rest). `grip` is scaled by Kevin's 1.05; his hand is then 9.9 cm wrist to knuckles, as the CC0
  hand is.
- **Guns aim down the controller:** a gun's barrel (the model's `-X` after `R`) is turned onto `-Z` about the grip
  position, so the laser, the game's shot and the barrel agree. For the pistols `R` is none and nothing moves; File #1's
  long-gun entries are 5.8 deg (assault rifle), 7.6 deg (submachine gun), 10.9 deg (shotgun) and 31.6 deg (grenade
  launchers) off the bone (the game shoulders them with the hand turned). Melee weapons, cans and things to throw keep
  the game's turn.
- **Result for Kevin's 45 Auto:** the model's origin lands at (-0.1, 2.8, -6.4) cm in the pointing frame, 2 cm from the
  placeholder pistol's trigger (0, 2, -4.6); the grip runs into the fist. Melee weapons now run **through the fist along
  the controller's handle** (up out of the thumb side, as the game holds them), not along the aim ray as the
  placeholder did: a 1 m pipe stands 63 cm up and 37 cm down from the hand, the 1.4 m scrub brush 82 cm up with its head
  on top. **This is the biggest visible change and needs Alex's eye.**
- **Laser:** from the entry's muzzle where it has one (`ModelLaserFrom`), else the placeholder's muzzle.
- **Holster:** guns hang muzzle down as before; cans and the molotov upright; a melee weapon hangs shaft down,
  15 deg back, moved down its shaft until its top end is at most 12 cm over the holster's middle (held at its grip a
  1.4 m broom would reach the head). Long guns sling over the shoulder as before.
- **Hands:** the CC0 hand's pistol pose round a gun, its fist round anything else and round a long gun's fore-end, and
  round the controller the pistol pose blended toward the fist as the trigger is pulled. The left hand's fore-end grip
  point is still the placeholder's per-gun table, not the model's pump.
- **Quads:** a pole seen side on needs more room; the largest quad is 1.6 m (the placeholder's 1.3 m), and a weapon too
  big for it keeps the hand in the quad and loses its far end at the image edge.
- **Fallback:** a weapon whose model did not decode or whose entry could not be read is drawn exactly as before (glove and
  placeholder), logged once per item (`(VR) Hands: item N (...) keeps the placeholder shape: ...`).

## Which weapons

- **Decoded on File #1:** 37 weapons in 130-153 ms; missing ids 2, 5, 6, 7, 12, 13, 15, 16, 17, 18, 24, 26, 30, 107, 109
  (not on File #1's discs at all).
- **The J's Bar scenario's item table** (slot 6) lists 21 weapons with an entry: 0, 4, 11, 27, 28, 29, 31, 102-106,
  110-114, 400, 401, 407, 408. All 21 drew in the image dump. The other scenarios have their own item tables at the same
  address (assumed from the code path; only J's Bar savestates exist).
- **Live equipped read:** slots 6 and 9 log `the equipped item 400 (Kevin's 45 Auto) is weapon type 7 (the item table
  says 7)`.

## Cost (measured)

- **Self-test, synthetic hand and pistol image, 6.2 k px, this PC:** 0.46 ms with MeshRaster at 2 x 2 samples (the hand
  alone 0.21 ms); 0.17 ms at 1 x 1; 1.5 ms at 4 x 4; the placeholder's 0.14 ms.
- **The real models** (image dump, CPU thread, one image each, game running): hand and pistol 0.5-0.7 ms (the first
  image 1.0 ms, cold caches), shotgun 0.9 ms, grenade launcher 1.0 ms, knives 0.3-0.4 ms, poles 0.2 ms, cans 0.5 ms.
- **When:** only when an image is drawn again (the existing rules: the view drifted 2 deg, grip or trigger moved, a shot,
  500 ms age), on the GS thread, never on the VR pacer. Worst case, both hands moving every GS frame: about 0.7-1.2 ms
  of GS-thread time a frame, against about 0.3 ms before. The pacer thread does no more work. The `(VR) Hands:` 5 s line
  now adds `mesh images N/s mean X max Y ms` in a session, to check this in the headset.
- `PCSX2_VR_MESH_SAMPLES=1` (or 4) changes the sample grid, for a comparison.

## Test aids

- `PCSX2_VR_DUMP_HANDS=<folder>`: once in game (a session or the headset-free scripted input), the models load if they
  have not, and each weapon in the scenario's item table is drawn in the hand (from the eye and from the side), in the
  holster or sling, and from 1.8 m, into PNG files there. **They show the game's own models: keep them on the PC.**
- `vr_selftests`: 26 of 26 suites (a new one checks File #1's `feedback.weapon.hold`). The hands self-test now has 98
  checks: the entry decode, the turn order (the spray's axis), the placement (the 45's
  origin at the wrist plus the grip, the barrel on -Z, a 10.6 deg gun turned onto it, a melee weapon not), the fallback
  image identical to today's, the mesh image inside its quad, the holstered gun, a 1.3 m pole keeping the hand in its
  quad and hanging from the belt, the laser from the model's muzzle, and NeedsReraster on the look and type.

## Not checked

- **The headset**, and any XR session: the GS-thread drawing path, the layer placement and the cost line are built and
  reviewed only (the null headset was off limits this session).
- **Weapons other than Kevin's 45 Auto equipped live:** equipping needs a menu confirm. Their entries were read from
  memory and drawn in the dump.
- **Other scenarios' item tables**, File #2 (no `hold` key in its profile, so the placeholder stays there).
- **Variant B's purpose.**
