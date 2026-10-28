# Melee power: how the zombie reacts to a hit, and swing power on the game's own reactions (2026-10-27)

Branch `meleepower` (from `nightly` 3df2946). Follows `2026-10-14-vr-melee.md` (contact mode, caves D-H; sections 1, 7,
12, 13). Static work on the slot-5 dumps (`ram5_a.bin`, `ram5_b.bin`); live offline runs of `run-mpower` (PINE slot
28170), headset-free, normal speed. Scripts and results in the session scratchpad (`mpower/live/mp_test.py`,
`res_run*.json`; nothing committed).

Evidence words: **verified (code)** read in the disassembly; **verified (memory)** read in a dump; **checked offline**
seen live headset-free; **assumed** reasoned, not measured.

Why: in Alex's 2026-10-26 session, pistol-whips landed 9 of 9 at 2.0-3.6 m/s, each 11-16 HP at reaction class 0 (a
flinch), so the zombies kept shuffling in and grabbed him twice in 150 s.

## 1. How the zombie (enemy type 1, update `FUN_00664290`) reacts to a hit

All **verified (code)** unless marked.

**Entry.** The update zeroes `+0x32` (`0x6642E4`: hit-testable unless a state sets it again), decays the down meter
(`FUN_0060EB00` at `0x6642F8`, below), then if the hit counter `+0x13F` is set (the hit test of the last frame) runs the
damage state `FUN_00678750` (`0x66430C`), then the state handler from `0x6E49E0[+0x08]` (0 init `0x664600`, 1 walk
`0x664D00`, 2 attack `0x66A950`, 3 hurt `0x66D840`, 4 down/dead `0x66E140`, 5 dormant `0x66F600`).

**`FUN_00678750`, in order:**
1. The attacker (`FUN_00605D80` -> `+0x11C`) and which side it is on: `+0xB1F` = 0 in front of the zombie's heading,
   1 behind (`0x6787C4`-`0x6787E8`: (bearing - heading + 0x4000) >> 15). Everything below picks a front or back motion by
   it; the swing's direction is never read.
2. The hit effect by `+0x141` (`0x6F74F0`), then **`FUN_0060F0C0(z, attacker, 1000)`**: HP `+0x544` -= `+0x138`; on the
   room's master, HP <= 0 returns 1 (dead); alive, **`FUN_0060EA80` adds to the down meter `+0xC2E`** and a meter at or
   over 1000 returns 2.
3. By the zombie's state:
   - **walking (1), dormant, anything but 2-4:** dead or meter full -> `FUN_00679700` (the fall); else
     **`FUN_006795C0` (hurt)** for every class: state 3 and by class `+0x140`:
     - **0: a flinch**, `FUN_00679490`: a motion picked at random from those the model has, front {301, 311} or back
       {302, 310} (`gp-0x6E28`, `gp-0x6E20`; `FUN_0060E0D0` + `FUN_0055D050`, a table lookup, not a space probe), sub 0;
     - **1: a stagger**, sub 1: motion 33 (front) or 34 (back) (hurt sub 1 `0x66D970`; plays as motion 314);
     - **2: a knockdown**, `FUN_00679510`: motion 320 (front) or 321 (back), sub 4 (`0x66DCD0`), `+0xBA3` 1/2, then
       motion 37/38 and the down state 4; if the model has neither, back to walking.
   - **attacking (2)**: the lunges (subs 3, 6, 9, 12) are interrupted into hurt as above; a hold (subs 0/1 before phase
     6, 2 before 4, 8 before 5) lets the victim go (`+0xB1E` = 6 on both) and is hurt; later in a hold only the light
     per-bone jolt (`FUN_005777B0`), except class 2, or class 1 when `+0xCEF` is set (the "flinch the first time" rule
     of `2026-10-14-vr-melee.md` section 1 is this hold case only).
   - **hurt (3)**: `FUN_0067A5E0` -> the light jolt only; `FUN_0067A560` -> hurt again; dead -> the fall.
   - **down (4)**: its own rules (`FUN_0067A650`, `FUN_0067A6B0`).
4. **The fall `FUN_00679700`** (dead, or the meter full while alive): class 0 -> hurt sub 2 (motion 41, a collapse ->
   state 4), class 1 -> sub 3 (the stagger, then motion 41 -> state 4), class 2 -> the knockdown.

**The down meter (the game's stun meter, accumulating).** `FUN_0060EA80`: a hit of **41 HP or more** adds
`0x6F6EE0[16 * (difficulty u8 0x48C01A & 3) + 2 * zombie +0xC28]`; `+0xC28` comes from the spawn row (`0x689D98`).
Slot 5's zombies read `+0xC28` = 6 and difficulty 0 (**verified (memory)**): **526 a hit**, so two such hits close
together put the zombie down. A hit under 41 HP adds nothing. `FUN_0060EB00` takes 1 off every other game frame
(**checked offline**: about 30 a second); the down states (`0x66E568`, `0x66EA14`, `0x66EFB4`, `0x66F584`) clear it.

**What pushes it back.** No velocity and no knockback: the knock vector `+0x12C` is read only by the per-bone jolt
(section 8 of the vr-melee note). The push is the hurt motion's own root motion, front or back by `+0xB1F`
(**checked offline**, section 3: a stagger 69-73 cm, a knockdown 228 cm, a flinch 0-7 cm, always away from Kevin, who
is in front).

**The game's strongest melee hits.** Classes come from `0x707390[type]` by attack id and distance band (vr-melee section 1):
knives 0, pipes/brush/pole 1, spear 2; the critical (`FUN_00636990`, m = 2 at the weapon's chance) multiplies damage and
raises the class by one (`0x578568`-`0x57858C`), so a pipe's critical knocks down and a knife's staggers. No kick or
shove reaches a zombie with more (bare hands' id 0 is class 1). So the strongest stagger is class 1 and the knockdown
class 2; the meter's fall is the game's way to put a zombie down with lighter blows.

## 2. What starts a grab (for `vrgrab`)

All **verified (code)**, the timing **checked offline**.
- **Where:** every update of the walk states (`0x6662D4`/`0x6662E4`, `0x666464`/`0x666474`, ...): `FUN_00679F30`
  (eligible: a target `+0x24`, its awareness `+0xBF8[...]` >= 4000, the zombie not in state 0 or 2, the target within the
  height window, its `+0xBA0 & 0x22000178` clear and not grabbed already (stance 5)) then **`FUN_0067A010`** (the
  room's master only).
- **Range and angle:** by the height difference dy and the dice: dy <= 25 and >= -25: 150 cm, sub 12 / 9 / 6 by random
  (20 / 20 / 60 %, or 50/50 12 / 9 when `FUN_00579C60` says so); dy > 25: 150, sub 12; dy < -25: 100, sub 3; a crawler
  (`+0x174` = 1): 150 sub 12 or 100 sub 3. **`FUN_00603640(z, target, 0x10000, range)`**: the distance (`FUN_005794A0`)
  within range and `FUN_006055F0` with angle 0x10000 (all round for the normal zombie; subtype 9 uses 60 deg). Then
  `FUN_00678180(z, 2, sub)`: state 2 (the attack).
- **No cooldown.** The test runs on every walking update.
- **A hit blocks it exactly while the hurt state runs.** State 3 runs only the hurt handlers. At the end of the hurt
  motion `FUN_00678100`: with the alert lock (`+0xC94` bit 0x20) and the attacker in sight (`FUN_00605BB0`) it goes
  straight to the next-action chooser `FUN_00677260` (which can start the attack at once); otherwise walk (1/0), and the
  test runs again on the next update. Measured: the attack started 0.12-0.14 s after a flinch and 0.14-0.37 s after a
  stagger ended.
- The attack's subs: in these runs sub 6 grabbed Kevin (stance 5) or missed, subs 9 and 12 hit him (stance 3).

## 3. Built: `[VR] MeleePower` (on)

**Power (pure, `PhysicalMelee::SwingPower`; BetterVR's model, MIT, `src/hooking/weapon.cpp`):**
- speed power = (tip speed - the strike's own speed) / (fullSpeed - it), 0..1;
- times the committed stroke: the far hit point's path while it moved at half the strike speed or more (reset after
  0.12 s slower): strokeMin 0.6 at 10 cm or less, 1 from 35 cm;
- times 1.25 with two hands (weapons and knives: the left grip squeezed past 0.5 within 30 cm of the right grip and
  moving 1 m/s or faster);
- kept to 0.15..1.
- **Damage multiplier** = (0.20 + 1.15 smoothstep(power)) x damage (kind); **least class** 1 from power 0.45, 2 from
  0.85, at most maxClass; **hit-stop** 2 + 2 x power display frames; **haptic** 0.45 + 0.55 p for 25 + 45 p ms (both
  hands on a two-handed swing).

| Kind | fullSpeed (m/s) | Damage | maxClass | Range on the game's damage |
|---|---|---|---|---|
| Weapon (pipe, pole, brush, spear) | 6.5 | 1.0 | 2 | 0.2-1.35 x (a pipe 12-81) |
| Knife (types 32, 33) | 4.5 | 1.0 | 1 | 0.2-1.35 x |
| Fist | 4.5 | 0.55 | 1 | 6-41 of the knife's 55 (was 11-22) |
| Whip (a gun's butt) | 4.5 | 0.70 | 1 | 7-52 of the knife's 55 (was 11-22) |

Why these ranges: 41 HP is the least hit the down meter counts, so a committed whip or a full punch counts toward the
game's own knockdown like a pipe; the whip stays under the knife's 55 and the punch under the whip; neither knocks down
by itself (the game's strongest light-weapon hit, a knife's critical, staggers). A committed 3.6 m/s whip (Alex's top
speed) has power 0.55: a stagger; a flick of it does not.

**Cave P** (`0xFA000`-`0xFA114`, 70 words, `whenSetting: MeleePower` = contact and the switch; its own `camera.caves`
range, all-or-nothing with read-back): the four callers of the damage `FUN_00578300` in the hit testers (capsule
`FUN_00549EA0` at `0x54A2D8`, `0x54A64C`; sphere `FUN_0054AB30` at `0x54AE14`, `0x54B224`; in all four s4 is the node,
a1 the attacker, a3 the class byte) jal cave P instead (each site checks P's first and last words; sites listed before
the body). For the **live contact strike only** (block `0xFF600` "PWR1", the melee block "MEL1", the node's object =
melee block `+0x14`, the attacker = its record) it runs the game's damage at multiplier 1 (no critical, no promotion),
scales it by `+0x08` (at least 1, at most 32767) and raises the class to `+0x04` when the game's is lower (never lower);
it notes the game's damage and class (`+0x18`, `+0x1C`), what it made (`+0x10`, `+0x14`) and counts (`+0x0C`). Every
other hit is the game's own. With power on the host leaves cave E's strength at 1.

**Host:** at each strike (`PollContact`), power from the strike's speed (the hold's peak), the stroke and the hands;
the block written (armed only while zero or ours; cleared when contact stops); the buzz; the hit-stop
(`StartHitStop`: the right hand's pose held for 2-4 display frames, applied in the compositor's two hand paths
`BuildHandLayers` and `LocateHandsNow`; never the game, its hit test or the gun ray). A weapon change resets the tip's
history (an equip is not a swing; found live: a poked equip read as a 30 m/s strike). Logs `(VR) Melee: ... power P,
damage xD, least class C, hit-stop N frames.` and `power P shaped the game's hit (cave P, 1): damage a -> b, class c -> d.`

**Kept:** one strike per swing per enemy; enemies with `+0x32` non-zero never struck; fists and whips strictly offline;
online cave P only where the contact caves are (the room's master with `PhysicalMeleeOnline`: the class and damage then
leave as the game's own enemy messages); only free-RAM blocks are written, never a record.

## 4. Checked offline (run-mpower, slot 5, zombie 49)

The whip: the held 45's butt moved straight through the belly sphere; the pipe: a poked iron pipe (vr-melee section 3)
held upright and moved straight through it. Speeds as the build measured them.

| Case | Strike | HP | Class | Zombie | Push | Next attack |
|---|---|---|---|---|---|---|
| Whip light, power on (2.7 m/s, power 0.15) | 1 | 600 -> 590 | 0 | flinch 3/0, motion 301, 0.56 s | -1 cm | 0.12 s after |
| Whip hard, power on (5.3-8.5 m/s, power 1.0; 3 runs) | 3 | 600 -> 549 (55 -> 51) | 0 -> 1 | stagger 3/1, motion 314, 2.27-2.28 s | 72 cm | 0.36-0.37 s after; no grab in 4.5 s |
| Whip hard, power off (5.3 m/s, strength 0.36; 3 runs) | 3 | 600 -> 580/581 | 0 | flinch 3/0, 0.7 s (301) or 2.0 s (311) | -5 to -8 cm | 0.12-0.14 s after; **Kevin grabbed at 2.56-2.58 s in 2 of 3** (and after a light whip's flinch, 2.54 s) |
| Pipe medium, power on (3.5 m/s, power 0.31) | 1 | 600 -> 572 (60 -> 28) | 1 | stagger 3/1, 2.21 s | 73 cm | 0.16 s after |
| Pipe medium, power off (3.9 m/s, strength 1.0) | 1 | 600 -> 540 | 1 | stagger, 2.17 s | 73 cm | 0.14 s after |
| Pipe hard, power on (9.3 m/s, power 1.0) | 1 | 600 -> 520 (60 -> 80) | 1 -> 2 | knockdown 3/4, motion 320, 3.14 s, then down (4/0) | 228 cm | none in 4.5 s |
| Pipe hard, power off (6.4 m/s, strength 1.5) | 1 | 600 -> 510 | 2 | knockdown, 3.12 s, down | 228 cm | none in 4.5 s |
| Control (no strike) | 0 | 600 | - | walks up, attacks (2/6) 1.09 s after the load | - | - |

- Cave P went in and read back on every load (`cave 0xFA000-0xFA118 in (70 words, read back)`), and every power hit
  logged its shaping (`damage 55 -> 51, class 0 -> 1`; `60 -> 80, class 1 -> 2`; `60 -> 16`; `55 -> 10, class 0 -> 0`).
  With the switch off the block stayed zero and the hits were today's.
- Two pipe sweeps in an earlier run (light 1.8 m/s: 60 -> 16, class 1; hard 8.7 m/s: 60 -> 80, class 2 -> knockdown
  with the meter at 1012 after two qualifying hits) agree.
- **HP scales with power:** whip 10 (0.15) / 43 (0.72) / 51 (1.0); pipe 16 (0.15) / 28 (0.31) / 67 (0.72) / 80 (1.0).
- **A staggered zombie does not attack during the stagger** (none in 2.2-2.3 s; the code has no other path), and can
  attack 0.14-0.37 s after it. The stagger's 70 cm push plus its length is what keeps it off: with power off the same
  whip's flinch let the zombie grab Kevin 2.6 s after the hit.
- Scripted timing inflates the measured speed (a nominal 6.0 m/s move read 8.5 once); the light whip read 2.7 m/s in one
  run and 4.1 in another (then power 0.72, a stagger).

**Not checked:** the hit-stop and the haptics (no compositor or controllers headset-free; `ApplyHitStop` is
self-tested), two hands live, a knife, other enemy types (their own damage states), `MeleeHitJolt` together with it,
online, the headset.

**Self-tests:** `SelfTestPower` (the curve, the thresholds, the stroke and two-hand terms, the kinds' caps, Alex's
3.6 m/s whip, junk input, the hit-stop), the profile entries, cave P interpreted (ours, light, the clamps, a knife's
stagger, anyone else's hit untouched); 68 of 68 suites.

## 5. Tuning on the headset

- `melee.power.<kind>.stagger` (0.45) / `knockdown` (0.85) / `fullSpeed`: where the reactions start.
- `whip.damage` / `fist.damage`: the caps.
- `[VR] MeleePower = false` brings back the contact strengths of `2026-10-14-vr-melee.md`.
