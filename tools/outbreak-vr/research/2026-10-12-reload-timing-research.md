# File #2 overlay flag, reload, timing and the take-it prompt cursor (2026-10-12)

Offline, headset-free PINE research on both Files (English patch), Kevin, local slot 0. VR test input
(`PCSX2_VR_TEST_INPUT=1`, no OpenXR runtime), network adapter off, normal speed, no code changed, nothing
committed. Tools: `pine.py` (reads, writes, savestates, FakeController), window captures with PrintWindow, keyboard
through SendInput for the flat runs (Select for pause). My own scripts and captures are in the session scratchpad
(`.../scratchpad/rt/`), not in the repo.

## Short answers

| Question | Answer |
|---|---|
| File #2 overlay flag | Verified. u32 `0x767110` = 1 while the item screen (with its command submenu), the map or pause is drawn, 0 otherwise. Same for the guard byte u8 `0x76A605`. 1302 + 142 samples, 0 mismatches between the two. |
| Native reload in File #1 | Yes: hold L1 with the weapon lowered (R1 not held) and the character standing still. One round per 0.33 s while held, stops by itself when full. |
| Native reload in File #2 | Yes, same input: L1 with the weapon lowered reloads the whole magazine at once. The earlier "L1 does not reload" note was most likely taken with the weapon raised. |
| Item-screen RELOAD route | Start, Down, Down, Circle, Down, Circle on the weapon in the special box (EQUIP is entry 0, RELOAD is entry 1). Details and traps below. Works on both Files. |
| Map and take-it delay | The game's own: about 0.64 s on the game flag, the same flat and in VR, with keyboard and with the VR pad. Occasional hitches of 1.1 to 2.1 s happen in both. The item screen opens in 0.09 to 0.12 s. |
| Take-it prompt cursor | The item-screen cursor struct tracks the prompt's highlight exactly (same node, index, x, y per cell). Only the four own cells are selectable, no equipped box, no partner panel. Circle takes (swaps if the cell is occupied), Cross declines. |

## 1. File #2 overlay flag

Savestate slot 1 (Training Ground) and slot 2 (same, .45 equipped), game camera. A thread polled these every
30 ms (about 36 ms real period): u32 `0x767110`, u8 `0x76A605`, menu byte `0x4912BE`, u8 `0x767254` (overlay
kind), u8 `0x767252` (sub-mode), u8 `0x767210` (command submenu flag), u32 `0x7671F0` (cursor struct).

| Address | Width | Values | Meaning |
|---|---|---|---|
| `0x767110` | u32 (u8 would do) | 1 / 0 | An overlay is up: item screen (including the command submenu and an R1 press in it), map, pause. This is the inferred address from the File #1 layout, and it is right. |
| `0x76A605` | u8 | 1 / 0 | Guard byte, always equal to `0x767110` (0 mismatches in 1302 samples). |
| `0x767254` | u8 | 0 none, 1 item screen, 2 map, 4 pause | Which overlay (File #1 `0x720BE4`). |
| `0x767252` | u8 | 1 in the command submenu, else 0 | Sub-mode (File #1 `0x720BE2`); the "2 = take-it prompt" value was not tried on File #2. |
| `0x4912BE` | u8 | 1 in item screen, map and pause | The existing menu byte; rises first. |

Samples (main run, 1302 in total: slot 1 879, slot 2 423):

- `0x767110` = 0 in: idle (54 + 14 + 21 + 56 + 22), walking (58 + 42), running (54), weapon raised (74), a shot
  (40), left stick while raised (41), weapon lowered (42), and after every screen closed. None was 1.
- `0x767110` = 1 in every sample of: item screen (81 + 45 + 58), item screen moving the cursor (23), command submenu
  (69), submenu backed out to the item screen (28), item screen with an R1 press (41), map (82 of 83; the first sample
  after the press was still 0).
- Pause: the first run's Select (keyboard) never reached the window (focus). Second run: 142 samples, all
  `0x767110 = 0x76A605 = 0x4912BE = 1`, `0x767254 = 4`; closed by pressing Select again (Cross did not close the pause
  on File #2 in two tries, unlike the map and item screen).
- Rise order on File #2: the menu byte rises 62 to 109 ms before `0x767110` (item screen 77, 63, 62 ms; map 109 ms;
  slot 2 item screen 63 ms). On File #2 the map did NOT show the long stall File #1 has (one map opening only).
- Both clear within one sample of the close.

So the profile's `pauseWhen` can use `{ address: 0x767110, equals: 1 }` (u32) on File #2, as proposed for File #1.
Not tested on File #2: a take-it prompt (no floor item known in Training Ground), file reading.

## 2. Reload

### 2a. Native reload: L1 with the weapon lowered

Setup: File #1 savestate slot 9 (new, see the end): Kevin's .45 (item record 87, 7 rounds) equipped, spare "45 Auto
Rounds" in cell 2 (record 8). Fired to 3 rounds first. Watched every 30 ms: local action byte (`+0x54F` File #1,
`+0x54B` File #2), sub-action `+0x09`, stance `+0x08`, the weapon record's `+0x24` and the ammo record's `+0x24`.
VR L1 is `LeftTriggerPress` (left hand trigger 1.0).

File #1:

| Input | Result |
|---|---|
| L1 tap (0.1 to 0.15 s), weapon lowered, standing | Action byte 0 -> 0x1C (28) within 50 ms, sub-action 56. ONE round added 0.31 to 0.36 s after the press (3 -> 4, ammo item 14 -> 13). Action byte back to 0 at 1.31 s after the press. |
| L1 held, weapon lowered | Same start, then one more round every 0.33 s: 3 -> 7 at 0.34, 0.67, 0.98, 1.34 s. Stops at full by itself even with L1 still held (a 3 s hold on a 6/7 magazine added one round and nothing more). The action byte stays 28 until 0.97 to 1.0 s after the last round. From empty, L1 held 5 s: rounds at 0.31, 0.65, 1.00, 1.33, 1.65, 2.00, 2.34 s, done at 3.35 s. |
| Handgun (record 86, 15 rounds, 9mm Rounds item 6) | Same: 12 -> 15 in 1.0 s, 30 -> 27 spare, stops at 15. The magazine size is the weapon's (7 for the .45, 15 for the Handgun); no field for it was found in the item record, so detect "done" by the action byte leaving 28 or the count stopping. |
| L1 while R1 is held (weapon raised), tap and 1.5 s hold | Nothing (action stays 128). |
| L1 while walking (left stick pushed) | Nothing. It starts only from standing. |
| L1 0.06 s after releasing R1 (the lowering takes about 0.3 s) | Works: the lowering is cut and the reload starts at 0.08 s. |
| No spare ammo (ammo item count 0), or magazine already full | Nothing at all, no animation. |
| R1 pressed 0.5 s into a reload | The reload goes on; the weapon is raised after it ends. Cross pressed during a reload is ignored. |
| Other inputs for a reload | R1 + Circle on an empty magazine: nothing (6 shots 3 -> 0, then 3 more pulls and a 2.5 s hold: no click byte, no reload). Square with and without R1: nothing. Triangle with R1: nothing. Cross with R1: a 2 s move (action 65), not a reload. Keyboard L2, R2, L3, R3 with the weapon lowered: nothing. |

File #2 (slot 2, .45 record 13, ".45 Auto Magazine" record 10 in cell 0, fired to 3 of 7):

- L1 with the weapon lowered, tap or 3 s hold: action byte (`+0x54B`) 28 at 0.02 to 0.06 s, the whole magazine at
  once: 3 -> 7 at 0.08 to 0.13 s, the magazine item 7 -> 3. Back to 0 about 2.05 s after the press. A hold does not repeat.
- L1 with the weapon raised, 2 s hold: nothing.

For the automatic gesture: lower the weapon (release R1, which can be done 0.05 s before), then hold L1; on File #1 hold
until the count stops rising and the action byte leaves 28 (7 rounds take 3.4 s from empty); on File #2 a tap is enough
(2 s). Do not start while walking. The profile's `feedback.weapon.reload: 0x1C` is right: 0x1C is the action byte value
during any reload on both Files (File #1 `+0x54F`; File #2 `+0x54B`).

### 2b. Item-screen RELOAD route

Measured with VR test input from savestate slot 9 (File #1) and slot 2 (File #2), polling every 30 ms. "t" is the
time after the press.

File #1 (weapon in the special box; the cursor struct is at `0x720B80`):

| Step | Press | What memory shows | t |
|---|---|---|---|
| 1 | Menu (Start) | `0x48BF7E` 0 -> 1 after 30 to 60 ms; `0x720AA0` 0 -> 1 after 90 to 120 ms (`0x720B84` reads 0 for a sample or two, then node `0x720C10`, idx 0, x,y 498,108 = cell 0). The item screen always opens on cell 0. Picture after 0.18 to 0.35 s. | 0.1 s |
| 2 | D-pad Down | node `0x720D40`, idx 2, 498,160 | 0.06 s |
| 3 | D-pad Down | node `0x722928`, idx 0, 550,216 = the equipped/special box. idx 0 is also cell 0's index, so check the node or x,y, not idx. | 0.06 s |
| 4 | Circle (A) | `0x720BA0` = 1, `0x720BE2` = 1, highlight byte `0x720B82` = 0 (EQUIP) | 0.02 to 0.07 s |
| 5 | D-pad Down | `0x720B82` = 1 (RELOAD); `0x723C2C` follows one frame later | 0.07 to 0.11 s |
| 6 | Circle (A) | action `+0x54F` = 0x1C within 30 ms; first round at 0.62 to 0.68 s, then one per 0.33 s (3 -> 7: 0.68, 1.01, 1.32, 1.67 s); action back to 0 and `0x720BA0` -> 0 at 2.40 s (0.73 s after the last round). The item screen stays open (`0x720AA0` stays 1), cursor still on the box. | 2.4 s for 4 rounds; about 3.4 s from empty |

Done = the weapon count at `+0x24` of its record has stopped rising and `0x720BA0` is 0 again (or the action byte left
0x1C). The sequence ran correctly with 0.6, 0.15 and 0.08 s gaps between 0.1 s presses (three runs, no lost press);
this PC's flakiness is still a reason to check memory between steps (below).

File #2 (cursor struct `0x7671F0`; box node `0x768F98`), same buttons: Menu -> node `0x767280` idx 0 (498,108); Down ->
`0x7673B0` idx 2; Down -> `0x768F98` idx 0 (550,216) (the inferred box node is correct); Circle -> `0x767210` = 1,
`0x767252` = 1, highlight `0x7671F2` = 0; Down -> `0x7671F2` = 1 after 0.06 to 0.1 s (`0x76A29C` leads it by a frame);
Circle -> action `+0x54B` = 28 after 0.05 s, the WHOLE magazine at 0.11 s (4 -> 7, magazine item 7 -> 4); `0x767210`
falls to 0 and the action byte to 0 at 2.05 s. The item screen stays open. A partial spare (4 left, magazine empty)
loaded the 4 and the item stays in its cell with count 0.

What shows the entry: the highlight index is the u8 at cursor struct + 2 (u16 at +2): File #1 `0x720B82`, File #2
`0x7671F2`. It resets to 0 when the submenu opens. Down on the last entry does not wrap (it stays).

Things that could make it pick the wrong entry:

1. Entry index 1 is RELOAD only on a weapon. Command submenus seen on File #1: weapon in the special box: EQUIP, RELOAD.
   A weapon in a grid cell (the Handgun): EQUIP, RELOAD, COMBINE, PRESENT. An ammo item: COMBINE, PRESENT (so Down +
   Circle on an ammo cell hands the item to a partner, PRESENT). File #2: box EQUIP, RELOAD; the magazine cell
   COMBINE, PRESENT. Check the cursor node before step 4 (`0x722928` / File #2 `0x768F98` for the box, or the cell
   node of the equipped record).
2. If the equipped weapon is in a grid cell (File #1: item-record index at `+0xC7C` equals one of the bytes at
   `+0xC84..+0xC87`) the route is different: the cursor starts on cell 0, move to that cell, Circle, Down, Circle.
   After equipping from a cell the submenu stays open (`0x720BA0` stays 1); from the box it closed.
3. EQUIP is a toggle: Circle on the highlighted EQUIP of the equipped weapon unequips it (`+0xC7C` -> 0). Only press
   Circle in step 6 when `0x720B82` reads 1.
4. Full magazine or no spare ammo: Circle on RELOAD just closes the submenu after 10 to 60 ms and nothing else
   happens (both Files; on File #2 an empty spare item stays at count 0). Not harmful; read the count to see it did nothing.
5. Down, Down reaches the box only from cell 0 (0 -> 2 -> box). From cell 1 it is 1 -> 3 -> box, from cell 2 or 3 one
   Down. The item screen opens on cell 0, but check the node after each press instead of counting presses.
6. The item screen only opens when standing still (profile `stopFirst`); the screen is real time, the game goes on
   behind it.
7. A command submenu left open by a lost press: 0x720BA0 = 1 at step 1 means do not press Circle until it is closed (Cross).

## 3. Timing: map and take-it prompt

File #1, savestate slot 7 (Kevin at the counter facing the pistol), headset-free, normal speed, 5 valid trials per
cell (each trial loads the slot, waits 3.6 s, presses once). Press time = when the press was handed to the emulator.
Polled every 5 ms (PINE), window captured about 60 times a second.

- "Flag": `0x720AA0` rising (`0x720BE4` rises about 30 ms earlier; the menu byte `0x48BF7E` at 15 to 60 ms for the map).
- "Picture": first captured frame that differs from the pre-press frame (whole frame, mean gray difference above 3,
  for the pickup the right-hand panel region, identical in these runs). Captures lag the screen by a frame or two.

Seconds after the press (median, [min - max]):

| Case | Input | Flag | Picture |
|---|---|---|---|
| Map (Triangle), flat | keyboard | 0.652 [0.637 - 0.698] | 0.729 [0.724 - 0.780] |
| Map, VR mode, fake head, first person armed | VR pad Y | 0.638 [0.638 - 0.670] | 0.730 [0.716 - 0.772] |
| Map, VR mode | keyboard | 0.637 [0.636 - 0.641] | 0.715 [0.703 - 0.739] |
| Take-it prompt (Circle on the pistol), flat | keyboard | 0.654 [0.652 - 0.670] | 0.762 [0.753 - 0.794] |
| Take-it prompt, VR mode | VR pad A | 0.655 [0.640 - 1.104] | 0.814 [0.776 - 1.433] |
| Take-it prompt, VR mode | keyboard | 0.668 [0.653 - 0.670] | 0.759 [0.741 - 0.759] |
| Item screen (Start), flat | keyboard | 0.093 [0.092 - 0.107] (n 4) | 0.185 [0.180 - 0.206] |
| Item screen, VR mode | VR pad Menu | 0.108 [0.093 - 0.123] | 0.295 [0.234 - 0.348] |
| Item screen, VR mode | keyboard | 0.093 [0.092 - 0.094] (n 4) | 0.189 [0.182 - 0.191] |

Other readings:

- Take-it prompt: the reach animation (action byte `+0x54F` = 9) starts 0.015 to 0.06 s after the press and the prompt comes
  at the end of it, 0.62 s later. The game frame counter `0x48BF93` ticks 19 to 20 times in that time: the game is
  running normally and the prompt follows the reach animation. (The earlier note "about 1.2 to 1.5 s" was not
  reproduced; it probably included a hitch.)
- Map: the game frame counter ticks only 2 times (3 once) between the press and the map flag, so the game loop
  freezes for about 0.6 s while the map is set up. Flag then picture about 0.09 s later.
- Taking the item: Circle on the prompt changed the inventory byte and closed the prompt within 0.015 to 0.046 s (5 trials,
  VR pad); the "X Obtained" text strip at the bottom starts to appear 0.23 to 0.30 s after that press.
- Hitches: three in about 40 trials, in both modes: a flat pickup that took 2.133 s (frame counter gap 1.47 s, not in the
  table), a VR-pad pickup that took 1.104 s, and a flat map with a 0.64 s extra gap in the frame counter (flag at 0.698 s).
  This PC is flaky in this test set-up; hitches like these are part of the "up to a second or two" Alex felt.
- Lost presses: the keyboard runs lost between a quarter and a half of the presses (window focus), and the emulator ended
  up paused three times (twice during keyboard runs, once right after a flat launch), probably the Alt tap in
  `drive.focus`. The VR-pad runs lost none.

Conclusion: the delay is the game's own. The medians differ by at most 0.015 s between flat and VR, and between the
keyboard and the VR pad; our VR layer adds nothing measurable. The map takes about 0.64 s because the game loop freezes
for that time (probably a synchronous disc read at emulated drive speed: the emulator's `fastCDVD = false`; NOT tested, it
needs a settings change), the take-it prompt waits for the reach animation (0.62 s), and the item screen is fast. On a real
headset the VR frame pacing would add to these; it cannot be measured here.

## 4. Take-it prompt cursor (File #1)

Floor pickups in savestate slot 9 (the bathroom herb trigger node at `0x3B7A9C`, whose item index at `+0x40` I set to
the record I wanted: Green Herb 14), and slot 7 (the pistol, Mark's panel on screen). Read after each press.

| Cell | Node (+4) | idx (u16 +0xC) | x,y (+0xE, +0x10) | Same as the item screen |
|---|---|---|---|---|
| 0 | `0x720C10` | 0 | 498,108 | yes |
| 1 | `0x720CA8` | 1 | 550,108 | yes |
| 2 | `0x720D40` | 2 | 498,160 | yes |
| 3 | `0x720DD8` | 3 | 550,160 | yes |
| equipped box | not reachable | | | `0x722928`, 0, 550,216 in the item screen |

- At the prompt: `0x720AA0` 1, `0x720BE4` 1, `0x720BE2` 2, struct +0 (`0x720B80`) 1, `0x723F94` 1, `0x723F95` 1, command
  submenu `0x720BA0` 0, highlight byte `0x720B82` 0, menu byte `0x48BF7E` 0. None of them changed while moving.
- The cursor starts on the first free cell (slot 7 empty inventory: cell 0; slot 9 with only cell 3 free: cell 3; all four
  full: cell 0). So the node is the highlight, from the first sample.
- D-pad: one cell per press, nothing at an edge. Seen: 0 -> right 1; 1 -> down 3; 3 -> left 2; 2 -> up 0; right from 1 or 3,
  left from 0 or 2, down from 2 or 3, up from 0: no move. Mark's panel is drawn in slot 7 but the cursor does not
  go there; neither does it reach the equipped box. The left stick was only tried at the corner of cell 3, where nothing was to move (no
  change); the earlier research found it moves the item-screen cursor.
- Circle on a FREE highlighted cell: the item goes into that cell (herb into cell 3: inventory 86, 6, 8, 14).
- Circle on an OCCUPIED highlighted cell: a swap, even when a free cell exists: the new item takes the cell (herb into
  cell 0: 14, 6, 8, 0) and the old item (the Handgun) falls to the floor beside Kevin as a floor item (owner 0, at
  9648, 53, 12948; the trigger list then shows an enabled pickup trigger for it: node `0x3B7A9C`, `+0x40` = 86). This corrects the old note "goes to the first free cell
  whatever cell is highlighted" (that was an empty inventory). With a full inventory the same.
- Cross: declines. The prompt closed within one sample, nothing changed.
- Nothing else is selectable: R1, L1, Triangle, Square, Start and the stick had no effect (state unchanged, prompt stays).
  No command submenu opens (`0x720BA0` stays 0).
- For the menu pointer: the cell rects of the item screen apply unchanged (same x,y), only the four own cells count, and
  the guard is `0x720AA0 = 1` with `0x720BE2 = 2`. Pulling the trigger (Circle) on an occupied cell throws that item on
  the floor, so a pointer should confirm on a cell only when the player deliberately points at it (or only on free cells).
- File #2: not tested (no floor item found in Training Ground, and its trigger list address is not known). By layout the struct is
  `0x7671F0`, cells `0x767280 + 0x98 * k`, but whether the box is unreachable there is unchecked.

## Savestates and set-up

- File #1 slot 9 was overwritten (allowed): the men's bathroom, Kevin at (9602, 13000), calm (no zombies in 15 s,
  Cindy may walk in), Kevin's .45 (record 87) equipped with 7 rounds, cells: Handgun 15 (record 86), 9mm Rounds 30 (record 6),
  45 Auto Rounds 14 (record 8), cell 3 free. Made from slot 8 by picking up the real Handgun, then pointing the herb trigger at
  records 6 and 8 and picking them up through the game's own prompt, then EQUIP on the .45. The previous slot 9 is in the
  scratchpad (`rt/states_backup/`) and as `...09.p2s.backup` next to it (an older backup of slot 9 was replaced by the emulator).
  The herb node `0x3B7A9C` in slot 9 points at record 8 (already owned); set `+0x40` to 14 before using it for a prompt.
- File #2 slot 9 not touched; slots 1 and 2 only loaded.
- Pickup trigger positions in slot 9 and 8: the floor items of the whole scenario sit in the item table, but only the
  current room's triggers are in the list (`*(u32*)0x23BD90`): the bathroom has just the Handgun (record 86) and the herb (14).
  No ammo can be picked up in J's Bar or the bathroom, hence the retargeted trigger.

## Notes for test scripts

- A captured frame takes 16 ms (PrintWindow into a numpy array); the emulator window updates at 30 fps.
- `drive.focus()` taps Alt when the emulator is not the foreground window; that and key presses went to a Qt menu bar a few times and
  paused the emulator (`status` "paused", no "Resuming" in the log). Check `p.status()` between trials.
- `read_block` needs an 8-aligned address and size; the item table is 60 bytes per record, read it in one block of 7808 bytes.
- FakeController input does nothing without `--vr`; flat runs need the keyboard (or a pad).

## Open questions

- File #2 take-it prompt and the take-it cursor there.
- Whether `fastCDVD` shortens the 0.62 s map stall (settings change, not tried).
- Real-headset latency on top of the game's own 0.64 s.
- Magazine size per weapon (7 for the .45, 15 for the Handgun) is not in the item record; the gesture should watch the
  count stop, not compare with a number.
