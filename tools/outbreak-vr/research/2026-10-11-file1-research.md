# Wave 4 research already done (orchestrator, offline PINE reads, 2026-10-11)

## Record identity byte +0x03 (lane coop-safety, item 1): CONFIRMED for both Files
File #1, savestate slot 4 (J's Bar), local slot byte 0x48BF7D = 0. Records at 0x476DD0 + k*0x10E0, u32 at +0x00 (little-endian):
- k0 0x00000101 -> bytes 01 01 00 00 -> +0x03 = 0
- k1 0x01000101 -> +0x03 = 1
- k2 0x02000101 -> +0x03 = 2
- k3 all zero (unused record, max HP 0)

File #2, savestate slot 1 (Training Ground), local slot byte 0x4912BD = 1. Records at 0x47BD30 + k*0x1100:
- k0 0x00000101 -> +0x03 = 0
- k1 0x01000001 -> +0x03 = 1 (== local byte 1)
- k2 0x02000001 -> +0x03 = 2
- k3 all zero

So record[local]+0x03 == local byte and used records have +0x03 == k in both Files: add `idOffset: 0x03` to BOTH profiles
(and `count: 4`). Note an UNUSED record reads +0x03 = 0, so idOffset alone would accept local byte 0 on an empty record 0;
that cannot happen for the local player (its own record is always populated) — fine.

## Focus hold (already shipped in 331dde9)
The hold points at slot 3 only while record 3's max HP is 0 (File #1 0x47A5B6, File #2 0x47F572, u16), else slot 2 when you are
slot 3 and record 2 is empty. Lane coop-safety's item 3 (give the focus back when the condition fails mid-hold) still applies.

## File #1 heartbeat patch (co-op) checked
Fine as shipped; see tools/outbreak-vr/HANDOFF.md "PC reply".

## vrtest.py note
`pitch` must run with --slot 3 (fails from slot 4, a test-save quirk). Do not edit vrtest.py this wave.

## Weapon research (orchestrator, File #1, offline, during the build)
- Equipping Kevin's .45 from the item screen (Menu, D-pad down x2, A, A=EQUIP) sets local +0xC7C = 87. Item record
  0x3967D0 + 87*60: present 1, index 87, id 400, count (+0x24) 7, owner 1.
- Firing (R1 raised, Cross): ammo 7 -> 6 within ~30 ms of the press; sub-action +0x09 goes 1 -> 2 during the shot, then back
  to 1; action +0x54F stays 128 (aiming). The lane's "ammo dropped while raised" shot detection matches.
- Normal play: +0xBA0 = 0x00 for all characters (File #1 slots 3, 4); +0xBA1 0x00/0x20.
- New savestates (File #1): slot 5 = J's Bar, gun equipped, a zombie GRABS Kevin ~4.7 s after loading (stance 5, action 176,
  status +0xBA0 stays 0x00, HP -100 per bite about every 1.2 s) -> grab hand-back and hurt-while-grabbed test.
  slot 6 = J's Bar scenario start (from slot 4), gun equipped, calm.

## Item-screen cursor (File #1, offline, VR test input, slot 6) — for the next wave's point-to-select menus
- 0x720B80 u32 = 1 while the item screen is up; 0x720B84 u32 = pointer to the current cell NODE (nodes 0x98 bytes apart from
  0x720C10): Kevin's cells node 0 0x720C10, 1 0x720CA8, 2 0x720D40, 3 0x720DD8; special/equipped box 0x722928 (node 49);
  partner panel cells seen at 0x721330 (node 12) and 0x721460 (node 14).
- 0x720B8C u16 = cell index within its panel, 0x720B8E/0x720B90 u16 = x/y: cells (498,108) (550,108) (498,160) (550,160),
  52 px pitch; special box (550,216) with index 0. Partner-panel cells reuse the same local x/y, so use the node pointer.
- 0x723F94/95 = 1 while the cursor is in the own item grid (0 in the special box, 0 when closed).
- Opening the item screen always starts at Kevin's cell 0 (node 0x720C10).
- Moves: up/left at cell 0 stop; down from cell 2 (or 3) enters the special box; up from the special box returns to cell 2;
  right from cell 3 jumped to a partner panel's cell 0 (node 12); left from that panel's cell 2 came back to Kevin's cell 0.
- One press moves exactly one cell even at the shortest scripted press (~1 vsync); a held D-pad did not auto-repeat in 1.5 s
  (at an edge, so inconclusive). The PS2 LEFT stick also moves the cursor; the PS2 RIGHT stick (RUp/RDown) does nothing in the
  item screen (so steering the menu with the VR right stick conflicts with nothing).

## R1 lock-on vs point-to-aim (File #1 slot 5, zombie approaching, fake head static, scripted aim)
- Raising with a zombie in the room sets the lock target +0xCDC = 3 immediately.
- Aim yaw 0 / +40 / -40 deg from the anchor: heading +0x92 reads anchor+0 / +40 / -40 exactly (252.9 / 293.0 / 212.9 deg);
  one transient sample 193.7 (game write between our vsync writes, toward the target) then ours again. So the lock-on does NOT
  take over the heading; at most it nudges it for a frame. Stance 3 appears when the zombie hits Kevin (stagger), heading jumps.
- Hit model (does the shot follow +0x92 or the lock target?) still unverified: needs an enemy HP read.

## File #2 (Training Ground slot 1, offline, game camera, VR test input -> PS2 pad)
- Local slot 0x4912BD = 0 this time (Kevin, char 0, HP 2300); an earlier read of 1 was taken before the state had loaded.
- Status +0xBB8 = 0x00 in normal play. Equipped +0xC8C = 0. Inventory +0xC94 = [10, 0, 0, 0]; special +0xC98 = 13.
  Item table 0x397B40 + idx*60: record 10 = present, idx 10, id 210 (handgun), 7 rounds, owner 1; record 13 = id 400 (.45 Auto),
  7 rounds, owner 1. Same record layout as File #1.
- Aim pitch +0xBE0 with the weapon raised: PS2 stick up -> +8192, down -> -8192 (positive = up): pitchSign 1 is right.
- With the weapon raised, the PS2 stick (up, down or right) moved Kevin about 20 units over 1.5 s in every direction: the raise
  step, not walking. File #2 does not natively walk while aiming in this state.

## Command submenu flag (both Files, offline, 2026-10-11 evening)
- The item-screen cursor struct + 0x20 (u8) is 1 while an item's command submenu (EQUIP/RELOAD/USE/...) is open and 0 when
  it is closed (also +0x62). File #1 0x720BA0, File #2 0x767210. Moving inside the submenu does not change the main cursor
  node. Found by full-RAM diffs (3 open/closed rounds) and confirmed on both Files.
- The struct's +0x00 word stays 1 after the item screen closes (stale), so menu logic must also check the menu flag
  (File #1 0x48BF7E, File #2 0x4912BE).
- File #2's equipped box node is 0x768F98 (as inferred: cell 0 node 0x767280 + 49*0x98), index 0, x 550, y 216.

## Breaking free of a grab (File #1 slot 5, offline; timings taken while the PC was hitching, so treat as relative)
| input while grabbed (stance 5) | grabbed for | HP lost |
|---|---|---|
| nothing | 10.2 s | 800 |
| mash Circle (VR A) ~8 Hz | 5.6 s | 500 |
| mash Cross (VR B) ~8 Hz | 5.2 s | 500 |
| mash Square (VR X) | 10.4 s | 850 (no effect) |
| waggle the left stick left/right ~6 Hz | 5.6 s | 500 |
| stick waggle + D-pad + Circle/Cross/Square/Triangle cycling | 3.3 s | 450 |
After the grab: stance 1, action 0/1. So a shake gesture should drive left-stick waggle plus Circle/Cross pulses.
