# File #1 UI flags and "in reach" research (2026-10-12)

Offline, headset-free PINE research on SLPM-65428 (English patch), Kevin, local slot 0. VR test input
(`PCSX2_VR_TEST_INPUT=1`, no OpenXR runtime, game camera, first person not armed), keyboard for Select
(pause). Normal speed throughout, network adapter off, no code changed. Evidence: full 32 MB RAM dumps (63
labelled dumps), MemWatch read/write watches with the writer PC, disassembly of the code those PCs point at,
continuous 30 ms polls, and window captures. Dumps, captures and scripts are in the session scratchpad
(`.../scratchpad/ui/`), not in this folder.

## Short answer

| What | Address | Width | Values | Confidence |
|---|---|---|---|---|
| A. Any game overlay up: item screen, map, pause, and the "Will you take it?" prompt for a floor pickup and for a partner's hand-over | `0x720AA0` | u32 (u8 works) | 1 up, 0 otherwise | High |
| A. Same, a guard byte set and cleared with it | `0x723F95` | u8 | 1 / 0 | High |
| A. Which overlay | `0x720BE4` | u8 | 0 none, 1 item screen or take-it prompt, 2 map, 4 pause | High |
| A. Sub-mode while the item overlay is up | `0x720BE2` | u8 | 0 item screen, 1 command submenu, 2 take-it prompt (stale after close) | Medium-high |
| A. Examine text and some scenario texts on screen (not a window, play goes on) | `0x48BF9A` | u8 | 1 / 0 | Medium (partial coverage) |
| B. "Circle would pick something up": no game flag exists | (none) | | the game searches only on the press | High |
| B. Read-only replica of the game's press-time search | trigger list at `*(u32*)0x23BD90` | | item record index, item id | High (25/25 live checks) |
| B. A partner is holding out an item for you | partner record `+0x54F` | u8 | 21 | Medium |
| Not B: wall/obstacle ahead | local record `+0x540` | u8 | 1 when a collision face is ahead | High (it is NOT an item flag) |

The menu byte the profile uses today, `0x48BF7E`, is never set by the take-it prompts, which is why they
stayed on the big first-person screen. It also has a co-op problem (below).

## A. Overlay flags

### 0x720AA0 (u32): "an overlay is up"

- `0x720AA0` is the first word of the menu module's state struct (the item cursor struct `0x720B80` is
  this struct + 0xE0; the submenu flag `0x720BA0` is + 0x100; `0x720BE4` is + 0x144).
- Code: `FUN_006A5A10(a0)` is its only non-clearing writer: `if (*(u32*)0x720AB8 == 0) a0 = 0;
  *(u32*)0x720AA0 = a0;` (`0x720AB8` is a buffer pointer, non-zero in every state seen). It is called with 1
  from `FUN_006B95C0` (the common "open overlay" routine: `if (!*(u8*)0x723F95) { FUN_006A5A10(1);
  *(u8*)0x723F95 = 1; ... }`, callers 0x6AE70C, 0x6AE7E4, 0x6AE8BC, 0x6B1F24, 0x6B39FC, 0x6B3A50, 0x6B9D9C,
  0x6BA678) and with 0 from the common close path at 0x6AEA10, which also clears `0x48BF7E`. MemWatch (write)
  confirmed the same open/close PCs for the take-it prompt, the item screen, the map and pause.
- Live values (63 dumps and continuous polls):
  - 1 in: item screen (also the SPECIAL and FILE tabs, the command submenu, the REQUEST submenu on a
    partner's item), map, pause, the take-it prompt after Circle on a floor item (J's Bar pistol, J's Bar
    butcher knife, bathroom Green Herb), the take-it prompt for a partner's hand-over (Mark's First Aid
    Spray after a REQUEST). Polled every 30 ms for 2-3 s in each: 1 in every sample (430 samples), 0 within
    a frame of closing.
  - 0 in: idle, walking, running, aiming, shooting (two shots), ad-lib shouts (Square, right-stick
    up/down), the pickup reach animation before the prompt, the "HANDGUN Obtained" / "GREEN HERB Obtained"
    line, partner speech subtitles, a partner walking over and holding out an item, examine text, a locked
    door's text, a door transition (door animation, "Men's bathroom" title card, scenario status 3), the
    opening cutscene (status 4), "NOW LOADING", being grabbed by a zombie (stance 5). Polled continuously
    through one long walk/run/aim/shoot/ad-lib/cutscene sequence and one door transition: 1255 samples, all 0.
- Global, not per slot: the open path stores the constant 1, so it reads 1 whichever slot the local player
  has. It is the local console's own UI, so in co-op it is your own screen.
- Rises with or without 0x48BF7E:
  - Item screen and map: `0x48BF7E` rises first (on the press), `0x720AA0` 0.55-0.65 s later. In that gap
    the picture stays on gameplay and the game frame counter `0x48BF93` all but stops (the menu is being
    set up); `0x720AA0` rises as the menu is drawn. Both fall in the same routine on close, same poll.
  - Pause: both are set by the same opener (0x6AE840 stores the menu byte, `FUN_006B95C0` the overlay
    word); the gap was not timed.
  - Take-it prompts (pickup and partner hand-over): `0x720AA0` rises alone; `0x48BF7E` stays 0.
- Order inside the take-it prompt: Circle starts the reach animation (local `+0x54F` = 9) at once; about
  1.2-1.5 s later `0x720BE4`, `0x720BE2` = 2 and `0x720B80` rise, and `0x720AA0` / `0x723F95` follow within
  a frame or two, as the prompt is drawn.

### 0x723F95 (u8)

Set to 1 next to `FUN_006A5A10(1)` in `FUN_006B95C0` and cleared at 0x6B9638 on close. Same value as
`0x720AA0` in every dump and poll. Either works; `0x720AA0` is the one the close path clears together with
`0x48BF7E`.

### 0x720BE4 (u8) and 0x720BE2 (u8)

- `0x720BE4`: 1 item screen or take-it prompt, 2 map, 4 pause, 0 when nothing is up (clean, not stale).
- `0x720BE2`: the item overlay's sub-mode. The item screen, map and pause openers zero it (0x6AE750,
  0x6AE848, 0x6AE8A8); it reads 1 in the command submenu (and the REQUEST submenu) and 2 in a take-it
  prompt. It is NOT cleared on close (reads 2 or 1 afterwards), so only use it together with `0x720AA0`.
  `0x720AA0 == 1 && 0x720BE2 == 2` = a take-it prompt.
- Inside a take-it prompt the cursor struct is live (`0x720B80` = 1, `0x723F94` = 1, node `0x720C10`) and
  the D-pad moves the highlight over your four cells exactly as in the item screen (checked: Right went to
  cell 1, Down to cell 3). Circle takes the item, Cross declines. With a free cell the item went into the
  first free cell (cell 0) whatever cell was highlighted.

### 0x48BF7E: co-op caveat (code, not live-tested)

The item screen and map openers write `0x48BF7E = local record +0x03 + 1` (`lbu v0,0x3(s0); addiu
v0,v0,1; sb v0,0x48BF7E` at 0x6AC640-0x6AC668 and 0x6AC698-0x6AC6B8; MemWatch saw s0 = record 0 and v0 = 1).
Pause writes the constant 1 (0x6AE840). So for a local player in slot 1-3 (online co-op) the item screen and
map read 2-4, and every profile check `0x48BF7E equals 1` (lookAt.pauseWhen, the stereo menu scene,
menuPointer.when) would miss them. Offline you are always slot 0, so this could not be seen live.
`0x720AA0` does not have this problem.

### Suggested profile use (not applied)

- `camera.lookAt.pauseWhen: [{ address: 0x720AA0, equals: 1 }]` (u32). The guard list is an AND, so
  replace the `0x48BF7E` entry rather than adding to it: `0x720AA0` covers everything `0x48BF7E` covers
  plus the take-it prompts, for any slot. The same flag fits the stereo `menu` scene.
- If the menu pointer should stay off in take-it prompts (it only steers the highlight there), keep it on the
  menu byte but write it as `{ address: 0x48BF7E, notEquals: 0, width: 1 }` for co-op, or use
  `0x720AA0 == 1` plus `0x720BE2 notEquals 2`.
- File #2 by the same struct layout (inferred, NOT checked): cursor struct `0x7671F0` - 0xE0 = `0x767110`
  for the overlay word, and `0x76A605` for the guard byte (File #1 `0x723F95` - `0x720B80` = File #2
  `0x76A605` - `0x7671F0`, the same offset that maps `0x723F94` to File #2's `0x76A604`).

### Texts that are not windows

- "X Obtained" (green item name + white "Obtained", bottom of the picture, about 2-3 s): play goes on (a
  zombie attacked during it); `0x720AA0`, `0x48BF7E` and `0x48BF9A` all 0. No flag found. For a cue, the
  inventory bytes changing (local `+0xC84..+0xC87`, special `+0xC88`) give the same event.
- Examine text (e.g. "Live concert notice posters. They are all over the place." at the bathroom wall):
  play goes on, Kevin walked during it. `0x48BF9A` (u8) = 1 while it shows (3 rounds of dumps, a poll:
  about 1.9 s), 0 in all 60 other dumps. Writer: the system message routine `FUN_00540140` (0x540204);
  its task (callback 0x6844F0) clears it when the text ends and also ends it when a menu opens. `0x476DC0`
  holds the current message task pointer, `0x476DC4` its priority. Partial: the locked staff-room door's
  text ('"Staff Room" The keyhole looks quite old.') showed with `0x48BF9A` = 0, so door texts use another
  path. Partner speech subtitles do not set it either.
- Partner speech, Kevin's own ad-lib lines ("Mark give me that item.", "Here you are...", "Take this with
  you."): subtitles over live play, no flag.
- File reading: the FILE tab was empty this early in the scenario, so a file text was not seen. It is inside
  the item screen, so `0x720AA0` and `0x48BF7E` are both 1 there.

### How the partner screens happen in Single Play (what worked)

- Asking a partner for an item: item screen (Start), D-pad Right, Down, Right to Mark's panel, Circle on his
  First Aid Spray opens a REQUEST submenu (`0x720BA0` = 1), Circle again. The item screen closes, Kevin says
  "Mark give me that item.", Mark walks over (about 7 s from J's Bar slot 7) and holds the item out (Mark
  `+0x54F` = 21, "Take this with you." / "Here you are...") for about 5.5 s. Nothing is drawn yet. Face him
  and press Circle: the take-it prompt "FIRST AID SPRAY Will you take it?" with Mark's and Kevin's panels
  (`0x720AA0` = 1, `0x48BF7E` = 0). If you do not press, his offer times out.
- At the press the game puts an item trigger (type 0x80) round the partner for the offered record (seen:
  trigger +0x40 = 1, a 63-unit square round Mark); it is not there while he only holds the item out.
  Mark's `+0xC7F` read 88 (his handgun record) before the offer and 1 (the offered record) from the offer
  on; seen in one run only.
- Ad-libs (Square, right-stick flick up "Thanks"-type line, flick down "Good..."-type line) did not make a
  partner offer anything in about 15 s each. Hurt and down, Mark healed Kevin directly (Mark `+0x54F` = 22,
  HP 15 -> 2015): no prompt, no offer.

## B. "Something can be picked up right now"

### There is no persistent game flag

- MemWatch: Kevin idle and facing the J's Bar pistol for 2 s, the pistol trigger's "hit" fields (+0x34
  character pointer, +0x3E mode) were never written; one Circle press wrote +0x34 once (PC 0x5ABFCC, s0 =
  Kevin's record). The item record's own words were not read at all while idle. The search that fires item
  triggers runs only on the Circle press.
- The trigger quads ARE read about 240 times a second while idle (geometry tests at 0x340750 / 0x340E4C),
  presumably the AI partners' own item searches: none of them wrote the trigger's hit fields for Kevin.
- Full-RAM diffs in-reach vs out-of-reach (same spot, facing the item vs. turned away; and near vs far)
  found only the local record `+0x540` and floats next to it, which is a wall probe, below.
- After a press the local record holds the result, stale until the next press: `+0xC78` = 1 (3 when the
  item's check fails), `+0xC79` = 2, `+0xC7A` = the item record index (3 = the pistol, 1 = Mark's First Aid
  Spray, 2 = the knife, 14 = the herb). Do not use these for "in reach".

### Local record +0x540 is a wall probe, not an item flag

`FUN_0062DAE0` casts a ray from the feet + 10 to a point ahead (callers pass the distance: 55, or 75 when
`+0xB9B` == 2, from the action handler `FUN_0061D1F0`; 60 or 96 from others) through the collision grid
(`FUN_005494C0`) and stores hit 1/0 in `+0x540` (written about 60 times a second while idle), the hit point
in `+0x528..+0x530` and the face normal in `+0x534..+0x53C`. It reads 1 facing an empty stretch of the bar
counter (no item, Circle does nothing), facing a door frame or a wall, and 0 at (8950, 12420) facing the
knife, where Circle still picked it up. It agrees with "in reach" only when the item sits on a wall or
counter.

### Read-only replica of the press-time search (validated)

The Circle-press item search is `FUN_00631760(rec, kind 1, mode 0)` (called at 0x61DB94; kind 0 when the
action byte is 26) which calls `FUN_005ABBE0(point, rec, mode, radius)`. Replicated exactly for floor items:

1. Character: position f32 `+0x38` (x), `+0x3C` (y), `+0x40` (z); heading s16 `+0x92` (0x10000 = 360 deg,
   direction (sin h, cos h), 0 faces +Z); reach f32 `+0x1B4` (0.0 for Kevin).
2. Probe point P = (x + sin h * (46 + reach), z + cos h * (46 + reach)), height y. Radius 50.
3. Trigger ("AOT") list: u32 at `0x23BD90` (gp - 0x5EE0, gp = 0x241C70) points at the first node; each
   node's `+0x28` is the next (0 ends). Node: `+0x00..+0x1F` four corners as s32 (x, z) pairs; `+0x20` s32
   y min; `+0x24` s32 y max; `+0x30` u8 type; `+0x31` u8 search mode; `+0x32` u8 priority (lower wins);
   `+0x3C` u8 enabled; `+0x3D` u8 busy; `+0x40` u32 item record index for items.
4. A floor item counts when type == 0x80, mode == 0, enabled != 0, busy == 0, the circle (P, 50) overlaps
   the quad (P inside it, or within 50 of an edge), and the heights overlap: y min <= y <= y max, or
   y < y min <= y + 200.
5. Item: record `0x3967D0 + index * 60`, item id u16 `+0x06` (0 Handgun, 102 Butcher Knife, 300 Green Herb,
   309 First Aid Spray; names in `pcsx2/VR/ItemData.cpp`).

Checked live against the game's own Circle (prompt or not), 25 of 25 agreed: J's Bar pistol on the counter
(y 120) from 16 spots and headings, including 147 deg no / 167 deg yes / 217 deg yes / 238 deg no, and
x 8533 at 209 deg yes / x 8547 at 176 deg no; bathroom Green Herb on the sink (y 137): z 13000 and 12965
yes, 12950 and 12935 no; J's Bar butcher knife (y 117) from both sides, 4 yes and 1 no (probe point 41 units
from the quad: yes; 66 units: no). Also on the dumps: facing the pistol yes, turned 90 deg either way no,
far no.

Other trigger types seen in J's Bar and the bathroom, not validated for a cue: 0x05 mode 0 examine spots
(point P inside the quad, no radius; +0x40 = message number), 0x84 mode 1 doors (+0x40 points at the
door's 0x00/mode 4 node), 0x03 large area events, 0x81 mode 7 (unknown objects), 0x85. A partner offering
is not in the list until the press (see A); for a cue use "a partner record (+0x03 != local slot) with
`+0x54F` == 21 within about 1 m in front".

## Savestates made (File #1)

- Slot 7: J's Bar, right after slot 6 (gun equipped), Kevin at the bar counter facing the pistol
  (8497, 13027, heading 185): Circle gives the pickup prompt; the REQUEST flow above works from here. The
  opening cutscene (zombies break in) starts about 20 s after loading, so keep tests short. The emulator
  also left `SLPM-65428 (32088394).07.p2s.backup`, my first slot 7 (a zombie attacked within 2 s); it can go.
- Slot 8: J's Bar men's bathroom, after the opening cutscene, calm (no HP loss over 15 s; Cindy may wander
  in), Kevin by the door (9451, 12951), not armed. Green Herb on the sink (trigger x 9581-9622, z 13052-13093;
  stand at (9602, 12990) facing 0 for the prompt), a Handgun at (9370, 132, 12727), examine spot at
  (9620, 12874) facing 90. Use it for long prompt, examine and message tests.
- Slot 9: not touched.
- Timings to the opening cutscene (status 0x48BF60 leaves 2): slot 7 about 20 s, slot 6 29 s, slot 4 38 s.

## Notes for test scripts

- 1.5 s after loading slot 4 the local record taken from `0x48BF7D` was Mark's (one of my research
  teleports moved him; offline, nothing saved); after 3 s it was Kevin's. Wait about 3 s after a load.
- The stick dead zone is about 0.5; the game latches the stick mapping across camera cuts, so a walker should
  close the loop on the heading, not trust the camera vectors at 0x306338/0x306344.
- The first Circle right after a FakeController starts is sometimes lost; wait 0.5 s.

## Open questions

- 0x48BF7E = slot + 1 for slots 1-3 is from the code only; check in the first private co-op session.
- No inventory-full pickup, item swap, or partner-asks-you-for-an-item prompt was seen; by the code they
  should open through the same overlay (`0x720AA0`), not verified.
- A file text (FILE tab with a file) was not seen.
- Door texts and "Obtained" have no flag found; `0x48BF9A` covers examine and some scenario texts only.
- Doors and partners as reach-cue targets are described, not validated.
- File #2 addresses are inferred from the struct layout only.
