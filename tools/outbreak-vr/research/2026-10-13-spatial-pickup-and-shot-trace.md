# Spatial pickup and shot targeting: static trace (File #1, 2026-10-13)

Static only: disassembly of a File #1 RAM dump taken in J's Bar during the 2026-10-12 UI research (`ram_now.bin`, kept in
the session scratchpad, never committed), plus earlier live findings. No emulator, builds or memory writes for this note.
"Verified" means read from the code or the dump. "Live" means seen running in an earlier offline session. Everything else is
marked as an assumption.

## Correction

`FUN_005C4FB0` and `FUN_005C5560` are **not** the firing routines. Both clear the equipped item's inventory cell through
`0x5C13D0` / `0x5C1240`, so they consume the item: they are throwing (molotov, throwable stick).
- Their reading of aim pitch `+0xBC8` (+`+0xCD4`) into the three-way class `+0xBCD` (level / up / down, about ±15°) is
  real, but it tells us nothing about where a bullet hits.
- **Bullet hit resolution remains an open research question.**

## Shot path: what is known

Verified:
- **The fire request** is the aiming stance handler `FUN_00555210` → `FUN_005562F0`.
  - It checks the equipped item (`+0xC7C`) and the ammo (`FUN_00555F80` reads the item record's `+0x24`), then sets the
    firing state (sub-action `+0x09` = 2).
  - No hit test happens in this chain: the call tree under it reads no heading, pitch class or lock target.
- **The lock target** `+0xCDC` (u8, 0xFF = none):
  - written by `FUN_0062D3B0` (the lock picker) and set when the weapon is raised with an enemy present (live);
  - L1 with a lock calls `0x62D6A0` (target switch) from `FUN_00555210`;
  - the gun-elevation servo `FUN_00556800` reads it and pitches the aim toward that enemy (enemy table `0x3FFCA0`, stride
    `0x1F0`). The profile NOPs that servo while you aim offline.
  - the co-op heartbeat sender `FUN_0064C620` reads it, so partners receive it.
- **The aim-class helper** `0x636E20` returns 1/2/3 by `+0xBCD` while aiming (action 128). Its only caller found is the enemy
  AI `FUN_0068E760` (the enemy type `+0x1F2` is checked against 102/103/112): enemies react to the height you aim at.
- **Heading:** the character heading `+0x92` follows our controller aim (live). The lock does not take it over.

Open: where the shot resolves, almost certainly in the firing state's per-frame handler, reached through the action
dispatcher's function table (`FUN_00635920` / `FUN_0061D1F0`). Next static step: find the code that writes an enemy's HP in
the `0x3FFCA0` table and walk back to that handler. Until then, the laser is not known to show the real hit line.

## Pickup: the shared search and every caller

Verified:
- **Search core:** `FUN_005ABBE0(point*, record, mode, radius)`. It walks the trigger list from u32 `0x23BD90` (next node at
  `+0x28`) and tests a circle of `radius` around `point` against each node's quad. It is called from 13 sites in 9 routines:
  - `0x5A02E0`, `0x5DE430`, `0x5E29E0`, `0x60E2D0`;
  - `0x631760` (three sites);
  - `0x651340`;
  - `0x673DB0`, `0x674490`, `0x6749A0`, `0x674EA0`, `0x675D40`.
  The 0x67xxxx group is likely partner AI and 0x651340 sits near the network code; both are assumptions.
- **Wrapper:** `FUN_00631760(record, kind, mode)` builds the point and calls the core with radius 50:
  - kind 0: the record's own position (`+0x38`/`+0x3C`/`+0x40`);
  - kind 1: position + (sin, cos)(heading `+0x92`) × (46 + `+0x1B4`), the normal Circle search;
  - kind 2: like kind 1 at 46 cm;
  - kind 3: another variant.

  `FUN_005AC0F0` is the kind-2 core. `FUN_00631760` is called from 8 sites in 6 routines: `0x5C1270`, `0x5F60D0`,
  `0x61D1F0` (the action handler), `0x61D9A0` (×2), `0x61DB40` (×2) and `0x61E400`.
- **The Circle pickup handler** is `FUN_0061DB40(record)`:
  - it returns early unless `+0xBA0 & 0x3E8` is clear and HP `+0x544` is non-zero;
  - it calls `FUN_00631760(record, kind 1, or kind 0 when the action byte is 26, mode 0)` at `0x61DB94`, then kind 2 at
    `0x61DBB0` when `+0xBA0 & 0x4000`.
  - It is called from `0x5F61C0` and `0x61DE60`. Assumption: one of those is the local player's input path and the other an
    AI or remote path; not verified.
- **Trigger volumes in J's Bar** (this dump; game cm, Y up; local player at x 8497, z 13027):

  | Item | Floor quad | Height (y) |
  |---|---|---|
  | Handgun (id 0) | a 40 cm square turned about 25° (corners (8503, 12896), (8467, 12913), (8484, 12949), (8520, 12932); bounding box 53 × 53 cm), centre (8493.5, 12922.5) | 120-160 |
  | id 102 | 61 × 101 cm | 117-137 |
  | id 10100 | 40 × 40 cm | 123-163 |

  The volumes are 2-5 times larger than the objects. They are good enough to say which item, too coarse to outline it.
  The item's model position is not found yet. Assumption: an item-object pointer with a position at `+0x200`/`+0x204`,
  seen only in the throw code.

## How a later probe override would be restricted (design; nothing applied)

- **Hook only the call at `0x61DB94`**, the kind-1 search inside the Circle pickup handler. Do not touch `FUN_00631760` or
  `FUN_005ABBE0`: they serve AI, partners, kinds 0/2/3 and every other caller.
- **The hook** is a small code cave that runs the original call unless all of these hold:
  1. the record is the local player (`+0x03` == u8 `0x48BF7D`, the same identity test first person uses);
  2. an "armed" word in a scratch location holds the current request id and has not expired (we write it only on a
     deliberate hand squeeze on a highlighted item);
  3. the kind is 1.

  When they hold, it calls `FUN_005ABBE0(scratch_point, record, 0, radius)` with the hand's game-space point, then clears
  the armed word itself, so it is single use.
- **Cleared by the host as well:**
  - after a timeout (about 300 ms with no matching call);
  - on disarm, a game screen, a state load, or a network-mode change;
  - when the hand leaves the target;
  - whenever first person is off.
- **Offline only to begin with** (the profile's `offlineOnly`). Co-op behaviour is not claimed until a private co-op session
  checks it.
- **To verify before applying:**
  - which of `0x5F61C0` / `0x61DE60` is the local input path;
  - that `FUN_00631760`'s kind-1 return value is the core's result;
  - what the core writes on success (the selected node for the prompt and commit).
