# Melee in co-op (File #1): how a hit reaches the other players, and what may run online (2026-10-14)

Branch `meleecoop` (from `melee`). Follows `2026-10-14-melee.md` (the swing, the attack object, the caves). Static work on
the slot-5 RAM dump of File #1 (`ram5_a.bin`, offline: SINGLE.BIN at `0x820000`) with NETAQ.BIN, the online message
overlay, read from the disc and laid over `0x820000`, where its header says it loads (nothing committed); offline live
runs of the `run-meleecoop` build, headset-free, normal speed.

Evidence words: **verified** = read from the code or memory; **checked offline** = seen live in a headset-free run;
**assumed** = reasoned, not measured. Nothing here is checked online: that needs Alex, his brother and the private
Bioserver.

## Short answer

**The sync model: every console runs every player's swing from the state the attacker sends, and the console that runs
a room's enemies (the room's master) decides their HP, deaths and reactions and sends those to the others.** It is
neither attacker-authoritative hit messages nor lockstep from inputs. Confidence: each link below is read from the code
(high); the whole has never been watched online (medium).

1. **The attacker sends state, not the hit (verified).** Entering the swing (stance 2, sub-action 3) changes the action
   bytes, so the character loop (`FUN_006399A0`, `0x639BAC`-`0x639BC4`) sends a type-1 message (`FUN_0064C620`, 0x2C
   bytes): the action bytes `+0x08`-`+0x0B`, the room `+0x1E2`, position, heading, HP, status `+0xBA0`, the equipped item
   `+0xC7C` (and `+0xC7F`, `+0xC80`, `+0xCB0`), aim pitch `+0xBC8`, `+0xCD4`, the lock target `+0xCDC`. The filter
   `FUN_006337C0` drops stance 2's sub-actions 0, 1, 5, 6, 7 and 10 (and a dry shot), not 3. Nothing about the weapon's
   pose, the motion frame or the enemy is sent.
2. **Every console swings every player's copy (verified).** The input stage (stance table `0x6DEC90`) is skipped for a
   remote human in network play (`0x639A1C`-`0x639A38`), but the second stage (table `0x6DECD0`, run at `0x639C2C` for
   every character in the current room) is not: stance 2 is `FUN_00646570`, whose sub-action table `0x6DF100` sends sub 3
   to `FUN_006474A0`. That plays the swing motion from phase 0 and runs its event script (`0x647700` -> `FUN_0055C030` ->
   `FUN_00543410`), whose spawn event (code 2) builds the attack object at that console's copy of the attacker's weapon
   (`FUN_005521B0`). The hit test (`FUN_005A9DB0` over the node list) has no local-player filter. So each console tests
   each player's swing against its own copy of the enemies, with the game's own timing and the canned pose.
3. **Enemies belong to the room's master (verified).** `FUN_0060EF30(enemy)` is true offline, or online when
   `0x1AF440(enemy +0x1E2)` is the local slot; online `0x1AF440` is NETAQ's `0x821590`: byte 0 of the 4-byte row for that
   id in NETAQ's table `0x825810` (rows 0-127, `0xFF` empty; filled by `0x8210B0` from a list of 5-byte entries, an id and
   four slots). An enemy's `+0x1E2` is its room: in eight File #1 dumps every enemy, every player record and the u16
   current room `0x3065AC` read the same value (1 in J's Bar; a partner in another room read 4). That the first slot is
   the console that entered first is **assumed** (HANDOFF's "whoever enters first runs the zombies").
4. **Only the master kills, and only the master sends enemy state (verified).** A hit staged on any console is
   subtracted there (`FUN_0060F0C0`), but a killing hit on a console that is not the master leaves HP at 1
   (`0x60F138`-`0x60F15C`). Enemy messages go on channel 8 only from the master (`FUN_006094A0`, `0x609520`-`0x609528`:
   the master of the enemy's room, or an entry of the grant table `0x70F9F8` for enemies with `+0xB9A` set). Eighteen of
   `FUN_00606500`'s message codes carry the enemy's HP `+0x544` with the attacker's kind and index (`+0x14A`, `+0x14B`),
   the damage kind `+0x148` and the hit zone `+0x13E`; the receiver `FUN_00609590` (reached from NETAQ's dispatcher at
   `0x820520`) writes them into its copy. Every other console's HP and reaction are its own prediction until then.
5. **A hit report that nobody seems to send (verified receiver, sender not found).** `FUN_00609590` has handlers for
   codes 82 (a hit: attacker `kind<<12 | index`, attack id, flags, damage capped at HP - 1, then the damage state) and 83
   (a kill: damage = HP, the dead flag), but no code that builds 82 or 83 was found in the main ELF, GAME.BIN or
   NETAQ.BIN as resident in the dump (the 0x71A000 senders, `FUN_00606500`'s list and every channel-8 send site).
   **Assumed:** unused in File #1, or built by code that is not in RAM in these dumps.

**So a melee hit "reaches" the partners as the master's enemy state.** If Alex's console is the room's master, the hit
his console computes is the one that counts and partners get it as the game's normal enemy messages. If a partner's
console is the master, the hit that counts is the one that console computes from Alex's character as it sees it: the
game's own swing, timing and canned pose.

## The messages (verified, `FUN_0064C620` and NETAQ's dispatcher `FUN_00820080`)

Channels (NETAQ jump table `0x824720`): 1-4 a player's own (slot + 1), into `FUN_0064D140` unless the sender is this
console; 5 statistics (`FUN_0064C130`); 6 system; 7 lobby-style sub-messages (0 to `FUN_0064E590`, 3 to `FUN_0064C130`);
**8 enemies** (`FUN_00609590`, or `FUN_0064FCB0` for the grant table when `msg[2] & 0x80`); 9 other objects
(`FUN_006531C0`). A channel-8 message is code, size, sender slot, the enemy's row in `0x71A030` (0x60 a row), payload.

Player message types (`FUN_0064C620`, jump table `0x70B9E0`; payload sizes in bytes):

| Type | Size | What | Sent |
|---|---|---|---|
| 1 | 0x2C | the action bytes, position, heading, HP, status, the equipped item, aim pitch, lock target | on an action change not filtered by `FUN_006337C0` |
| 2 | 0x18 | position, status, heading (the heartbeat) | `FUN_00633530` (every 30 frames; the co-op patch makes it 3) |
| 3 | 0x20 | 7 words from `+0xAB8` | |
| 4 | 0x08 | `+0xC78`-`+0xC7B` (the inventory) | |
| 5 | 0x34 | **the sender's own hurt**: action bytes, position, HP, the hit record `+0x13E`-`+0x14C`, the knock vector `+0x12C`, the attacker `+0x11C` | while stance 3, 4 or 5 (`0x639B98`) |
| 7 | 0x0C | item events `+0xD92` (bit `0x200`/`0x400`: a throw, which `FUN_005C4FB0`/`5C5560` replay with its own attack object) | |
| 8 | 0x0C | `+0xC64`-`+0xC6A`, written into another character (an interaction) | |

HANDOFF's "type 5: damage and knock-down" is the hurt player's own record, not a hit on an enemy; no player type
carries a hit on an enemy.

## Every patch local-only (changed in this branch)

Before, the seven spawn-frame immediates in `FUN_005661C0` were set to 2 for everyone: AI partners' swings went live
early offline, and online the partners' copies on Alex's console would have too, which while Alex's console is the
master would have moved the hit window that counts for his brother's swings.

Now the seven `jal FUN_00565C80` words that follow those immediates call **cave C** (`0xFE100`, 22 words) instead. With
`a1` the swinging record it sets the frame (`a0`) to 2 only when the block holds the magic, names `a1` as the local
record and was stamped within 6 frames, exactly the test caves A and B make, then jumps on to `FUN_00565C80`. The frame
words stay stock, and every other record gets the game's own frame. Caves A and B were already per-record: their
fallback is the game's copy (the weapon object `t0` is the pointer the original reads its matrix through:
`0x552D40` against `0x552DAC`-`0x552DC4`). The host now also zeroes the whole block when it stops, so the free RAM reads
as it was once the caves are out.

**Checked offline** (slot 5, the poked pipe as in the melee note, Circle pressed by the script with R1 held; the script
writes the block itself; `local_only.py` in the session scratchpad):

| Run | Words | Attack object | Zombie |
|---|---|---|---|
| switch off | sites and frames stock | spawned at motion frame 18 (the read lags the spawn at 16), at the weapon object (7983, 130, 13625) | 600 -> 530 |
| switch on, block names Kevin | 7 sites `jal 0xFE100`, frames stock, caves A B C in | at frame 4, 63 ms after the swing began, on the block's pose | 600 -> 530 |
| switch on, block names Mark's record | the same | at frame 18, at the weapon object (7983, 130, 13625): the game's own code | 600 -> 530 |

The third row is the case of any character that is not the local one: with the caves in and armed, Kevin's swing ran
exactly as with the switch off. The build's own path (`e2e.py`: a scripted right hand, PhysicalMelee's detector and
block): `swing at 3.9 m/s; the game's swing started 33 ms later`, `contact, enemy 49 HP 600 -> 540`, then 530, the
zombie staggering. No AI partner swung a melee weapon in 85 s of slot 5 (Mark shoots; the bare-handed partner never
attacked), so an AI partner's own swing was not watched; the self-test runs the caves' words on an interpreter for the
partners' records (and catches a changed word: checked).

## What may run online (the verdict)

- **The press (safe).** Circle on Alex's own pad is what a player does. Partners see Alex's character make the game's own
  swing (from the type-1 message, about 33 ms after his arm plus the network's delay), and every console computes the
  same hit from it as when he presses A.
- **The hit on the VR weapon, while Alex's console is the room's master (allowed, behind its own setting).** Only his
  console's attack object moves; its hits are the authoritative ones and leave as the game's normal enemy messages, so
  HP, stagger and deaths agree everywhere. What his brother will see: Alex's character swinging the game's canned swing
  while enemies react where the VR weapon actually hit, possibly before the canned swing gets there (the early window)
  or off to its side. His own console also tests Alex's canned swing against its copies, so it may show a flinch the
  master did not make; Alex's next enemy message overwrites the HP (assumed self-correcting: that is how the game already
  handles a lagging copy). Never a kill that the master did not make.
- **The hit on the VR weapon while a partner's console is the master: not done.** It would be a hit only Alex's console
  computes; the master never sees it, the enemy would flinch and lose HP on his screen only, until overwritten. Online the
  swing is then the game's own, exactly.
- **The free RAM.** The disc headers place the overlays at `0x340000` (SUBMAIN), `0x540000` (GAME, NETWK), `0x6C0000`
  (NETDNAS, NETHTTP) and `0x820000` (NETAQ, SINGLE), none in `0x84000`-`0x100000`; online use there is not known, so
  before the caves go in online the host reads `0xFE000`-`0xFF090` and requires zero (else it logs the word and stays on
  the game's own swing).
- Alex's rules hold: nothing writes another character's record (the caves touch only the attack object whose owner is
  the local record; the block is free RAM), and nothing pauses, changes speed or touches states.

## The online mode (implemented)

`[VR] PhysicalMeleeOnline` (off by default, ini only), honoured only with `PhysicalMelee` on, CameraDriver's network mode
private co-op (where first person already arms) and Online-Safe off. `VR::PhysicalMelee::Decide`: `Offline` (strictly
offline: as before), `CoopPress` (presses and cues; the game's own swing and hit), `CoopFull` (presses, the block and
the caves). `CoopFull` needs the profile's `melee.online` (File #1:
`{ room: 0x3065AC, masters: 0x825810, overlay: 0x820020, overlayTag: 0x6174656E, free: 0xFE000 }`), NETAQ.BIN's name
word in place, `masters[room * 4]` equal to the local slot (`RoomMasterSlot`), and the free RAM read zero once.
CameraDriver's `whenSetting` gate lets the PhysicalMelee entries in during private co-op only while the last input poll
decided `CoopFull` (`ProfileDB::SilenceSettingCoopLive`); `SeamlessDoors` stays strictly offline.

Self-tests (all pure; 32 of 32 suites): `Physical melee online` (the mode table, the room master read on a scripted
memory: first slot, empty row, wrong overlay, room past the table, unreadable row; the free-RAM check), `Physical melee
caves` (caves A, B and C run from the profile's words on a small EE interpreter: local and fresh, a partner's record, no
magic, a stale stamp, a wrapped counter, the 6-frame edge), the File #1 entry check (12 sites, 84 cave words, no other
word) and the parser's `online` cases.

## The private co-op test (Alex and his brother; needs Alex's go)

Setup:
- Both on a build with this branch, private co-op as it already works (`netmode.py private`, `PrivateCoopFirstPerson =
  true`), File #1, plain ISO, 100% speed, nothing paused, no states.
- Alex only: `[VR] PhysicalMelee = true` and `PhysicalMeleeOnline = true` in his ini. His brother changes nothing (the
  caves exist only on Alex's console; his game is stock).
- Alex picks up and equips a melee weapon himself (J's Bar has an iron pipe, a butcher knife, a scrub brush, a wooden
  pole and a spear).
- Alex's emulog, at the start: `(VR) CameraDriver: network mode private co-op...` and
  `(VR) Physical melee: on online, private co-op (swing to attack with Circle; ...)`. (The caves going in and out log
  only in dev builds; the `(VR) Melee: online, ...` lines below say which way it went, and the caves follow on the next
  frame.)

Steps:
1. **Brother is master.** The brother enters the room first. Alex's log: `(VR) Melee: online, room N: the console of
   slot B runs its enemies (this is slot A), so the hit is the game's own swing (press only).` Alex swings at a zombie
   in front of him. Both: Alex's character makes the normal swing each time; the zombie is hit exactly as if Alex had
   pressed A; same HP and reaction on both screens. Alex's log: `swing at ... the game's swing started ... later` and
   `contact, enemy n HP a -> b` when it lands.
2. **Alex is master.** Both go to another room, Alex first. Alex's log: `(VR) Melee: online, room M: this console (slot
   A) runs its enemies, so the hit follows the VR weapon.` Alex hits a zombie with the VR weapon somewhere the canned swing would not reach (to the side,
   or low). The brother: does the zombie flinch and lose HP on his screen too? After enough hits, does it die on both?
3. **A miss as master.** Alex swings the VR weapon well away from a zombie that the canned swing would hit. Alex's
   screen: no hit. The brother: note whether the zombie flinches for a moment and then carries on (his console's
   prediction, overwritten by Alex's).
4. **The brother attacks** (gun or melee) while Alex's caves are in: his hits look and count as usual on both screens.
5. **Room change back** to a room the brother entered first: Alex's log switches back to `... (press only).` (the caves
   come out).

Abort at once if a zombie stays alive with no damage on one screen while dead on the other, characters warp or the
game reports a communication error, or Alex's log shows `the caves' free RAM is in use`: Alex ticks Settings -> VR ->
Online-Safe First Person (applies at once: PhysicalMelee turns off and the caves come out on the next frame), or both
leave the game normally. Never pause or load a state.

Afterwards: Alex's emulog lines above, and what the brother saw in steps 2 and 3, decide whether `CoopFull` stays.

## Not known

- Who sends codes 82 and 83, and when; the grant table `0x70F9F8` (which enemies a non-master may run).
- That byte 0 of a room's row is always the console that entered first (assumed), and how the row changes when that
  player leaves.
- That the master's copy of Alex's character finds its weapon object and hits as on Alex's own console (the code path
  is the same; not watched).
- How large the prediction mismatch of step 3 looks with real lag.
- File #2 (not traced).
