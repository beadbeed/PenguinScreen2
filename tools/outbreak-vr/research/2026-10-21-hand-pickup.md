# Real hand pickups: squeeze on an item to take it (File #1, 2026-10-21)

`[VR] HandPickup` (off by default), branch `handpickup` (from `nightly`). File #1 (SLPM-65428) strictly offline. Alex
approved one exception to "never confirm a menu entry automatically" for it (2026-10-21): a deliberate squeeze of the hand
on a highlighted item may confirm the game's "Will you take it?" prompt for that item. Nothing else confirms on its own.

Evidence labels: **written**, **reviewed** (read back against the disassembly), **built** (local, LTO off), **self-tests
passed**, **checked offline** (headset-free, scripted PINE hands, normal speed, no network adapter). Nothing here has been
in a headset.

## What a squeeze does

1. **Which item** (`ItemReach`, every other vsync). Each tracked hand's grip position goes into the game's space
   (`CameraDriver::GameFromVr`); a hand more than `maxReach` (85 cm) from the character's position on the floor plane is
   out of arm's reach and counts for nothing. The rest is measured against every floor-item trigger volume the game's own search looks at
   (the list at `0x23BD90`, type `0x80`, mode 0, enabled, not busy; any item id). The nearest volume within
   `reach.pickup.show` (45 cm) of a hand gets the box (the hand-targeting marker); inside it, or within `near` (6 cm), the
   item is **highlighted**, and that hand ticks once. The right hand wins a tie.
2. **Only where the game would pick that very item.** The search core `FUN_005ABBE0` is replicated at a point
   (`ItemReach::CoreSearchAt`): every enabled, not busy node of search mode 0 or 4 of any type (`0x80` / `0x81` by the 50 cm
   circle against the quad, `0x85` by the character's own position, every other type, such as examine spots, by the point in
   the quad), the height rule (y in `[yMin, yMax]`, or `yMin` up to 200 above), the lowest priority first in the list
   (read from `0x5ABC60`-`0x5ABFBC`). `PickupPoint` picks the search point: the hand's own (or just inside the quad when the
   hand is beside it), else the volume's centre, else halfway to each corner, its height held inside the volume. If another
   trigger would win at every one, the item is not squeezable (logged).
3. **The squeeze** (`HandPickup`, every vsync). The grip must have been open (< 0.35) and then close past 0.75 (release
   first: a grip already closed when the switch, first person or the cue comes on counts only after it opens). Refused with
   a game screen up, the weapon raised (Circle fires), the hand in a body zone (holster, belt, backpack: their squeeze), or
   during a camera cut. Then the host block at `0xFF200` is written (`+4` the game counter `0x48BF93`'s value, `+8` the
   local record, `+0x0C` a tag `0x4850nnnn`, `+0x10` the point; the armed word `'HPK1'` at `+0` last) and Circle is
   pressed once (100 ms) on the VR Gamepad's own pad.
4. **The cave** (`camera.silence`, `whenSetting: HandPickup`, `camera.caves` entry `0xFEC00`-`0xFEC94`, 37 words, all or
   nothing). `FUN_0061DB40` (the Circle pickup handler) calls `FUN_00631760(record, kind, 0)` at `0x61DB94`; the kind is 1
   (the search 46 cm ahead of the character), 0 when the action byte is 26. The site becomes `jal 0xFEC00`. The cave runs the
   stock call (`j 0x631760` with a0, a1, a2 and `$ra` as the game set them) unless: the kind is 1, the block's armed word is
   `'HPK1'`, the block's record is this very record, the record's slot (`+0x03`) is the camera focus u8 `0x3AEF74` (the
   local slot, as caves A-G), and the stamp is at most 18 game frames old (the counter wraps at 60). Then it clears the armed
   word (single use) and tail-jumps to `FUN_005ABBE0(block + 0x10, record, 0)` with `f12` = 50.0, so the search returns to
   `0x61DB9C` as the stock one does. Both ways are tail jumps: neither pc nor `$ra` is ever left inside the cave, so the
   review's drain rule (d2) is met at once. `FUN_00631760` and `FUN_005ABBE0` themselves, their 20 other callers (the AI
   partners, kinds 0/2/3), and the handler's second search (kind 2 at `0x61DBB0`) are untouched.
5. **The host clears the word** on a 300 ms timeout, a game screen coming up first, the hand leaving the item (or moving
   to another), the switch off, first person disarming, the network coming on, and a state load (the loaded state's own
   armed word is cleared on the next poll).
6. **The prompt.** After the search the character does the game's reach (action byte `+0x54F` 9) and the take-it prompt
   rises 0.6-1.4 s later. It counts as this item's prompt only when all of these hold:
   - overlay u32 `0x720AA0` = 1, u8 `0x720BE4` = 1, u8 `0x720BE2` = 2 (the take-it prompt, `2026-10-12-ui-flags-research.md`);
   - the prompt's own item struct at `0x722890` names it: u8 `0x722891` = the item record index, u16 `0x722892` = the
     item id (`+0x08` holds its name, "HANDGUN" / "GREEN HERB"; it reads 0 before the prompt in both savestates);
   - the local record's search result u8 `+0xC7A` = the record index.
   (The research's `+0xC78` = 1 is not true while the prompt is up: it read 0 for the Handgun and the Green Herb, so it is
   not used.) A partner's hand-over shows the offered record, another item shows another record and id: nothing is pressed.
7. **The confirm** (the approved exception). With the grip held all the way from the squeeze (never below 0.35 since; let
   go during the reach and the prompt is yours, even if you squeeze again), after the prompt has been up 300 ms, with no
   button of your own pressed (A, B, X, Y, the trigger or the D-pad: you are answering it yourself), and only with a free
   cell of your own (`+0xC84`..`+0xC87`, 0 = empty) **under the prompt's cursor** (`0x723F94` = 1 and the cursor's node
   `0x720B84` / index `0x720B8C` one of your four cells): one Circle. A cursor on a full cell is moved toward the nearest
   free cell with one D-pad press at a time (never right of the right column, the partner's panel, nor down from the
   bottom row, the equipped box), each checked to have moved within 0.5 s, at most 3. A full cell under the cursor is
   never confirmed (that would swap the item out, the old one dropped).
8. **A full inventory** is the game's swap flow: nothing is pressed, the prompt is the player's (logged).
9. **Afterwards** the item is looked for in your cells (logged "taken ... cell k"). The grip must open before the next squeeze.

Why "held through" and not "squeeze again": the squeeze that starts the take is one deliberate action that lasts until the
item is yours, like closing your hand on it; holding is what a hand does with something it has taken, and letting go is
the natural way to change your mind. A second squeeze would need the player to notice the prompt first (it shows on the
flat screen without `MenuPanel`), and a re-squeeze test could be met by a fidgeting hand. A timer never confirms anything.

## Offline checks (2026-10-21, `tools/outbreak-vr/pickuptest.py`, run-pickup, local LTO-off build)

| Run | What | Result |
|---|---|---|
| slot 7, Handgun (id 0) on the J's Bar counter, Kevin facing it | squeeze, hold | cave took the word 19-35 ms after the press; prompt (record 3, item 0) 0.64 s later; confirmed 0.31 s after; Handgun in cell 0 |
| slot 7, head (and with it Kevin) turned 90 deg left, hand on the Handgun | squeeze, hold | the character's own Circle would not take it ("no" in the log); same result: Handgun in cell 0 |
| slot 8, Green Herb on the bathroom sink, Kevin at the door 1.5 m away facing +x | squeeze, hold | own Circle would not take it; the cave took the word 34 ms after the press, the herb's prompt (record 14, item 300) 1.34 s later (the reach walk), confirmed 0.31 s after; Green Herb in cell 0 |
| slot 7, squeeze then let go after 0.3 s | squeeze, release | the prompt came (record 3, item 0) and stayed up; logged "the grip was let go before the prompt: the prompt is the player's"; nothing pressed, nothing taken |
| slot 9, the .45 rounds (id 262) on the sink, one free cell | squeeze, hold, twice | the cave took the word and the character reached both times, but the game opened no prompt (the rounds' record 8 is the one already in cell 2); "no take-it prompt came": nothing pressed, cells unchanged |
| slot 7, `HandPickup = false`, head turned 90 deg (heading 33732 -> 50116, 90.0 deg), the same squeeze at the Handgun | squeeze, hold | no `Hand pickup` lines, the cave never went in, overlay stayed 0, cells unchanged |

After the reach limit (f802fcf, 2026-10-22): slot 7, the hand 4.5 cm in front of the Handgun's volume (about 77 cm from
Kevin): highlighted, prompt, confirmed, Handgun in cell 0. Slot 8, the hand on the Green Herb 1.9 m from Kevin: "no floor
item near a hand within arm's reach", no highlight, nothing pressed, overlay 0, cells unchanged.

Not checked live: a full inventory (no savestate has four full cells and a floor item with a prompt; the refusal is
self-tested), the cursor resting on a full cell (every prompt seen opened on cell 0 and it was free), a partner's
hand-over, the left hand, the box in a headset. One of the slot 8 runs did not arm first person at all after the load
(a startup flake of the headset-free setup; the retry ran clean).

Self-tests: 44 of 44 (the 42 before, plus "Hand pickup" with 48 checks and "Hand pickup cave" interpreted; the pickup
cue's suite grew from 120 to 141 checks with the selection).

## Risks and what is not known

- **The box** draws with `HandPickup` on too (9b1c2a9: the compositor's two `HandTargeting` gates, the target slot's
  swapchain and its draw, also take `HandPickup`; nothing else in that file changed).
- **The replica is conservative, not exact.** Types `0x01`, `0x02` and `0x81` have extra checks in the core that can only
  refuse; `0x85` uses the character's position. A node the replica names may lose to nothing, so the worst case is an item
  that is not squeezable, never a different item taken (and the prompt check catches a different item anyway).
- **Arm's reach** (f802fcf, `reach.pickup.maxReach`, 85 cm): a hand farther than that from the character's position on
  the floor plane highlights nothing, so its squeeze arms nothing. Before it, the game took the slot 8 herb from 1.9 m (the
  character walked over in the reach animation). The J's Bar Handgun's volume centre is 104 cm from Kevin's spot at the
  counter, its near edge 78 cm: at 85 cm the counter's front half of the volume is in reach. Tune in the headset.
- **Partner hand-overs and the inventory-full swap with a real prompt** are guarded by the prompt struct and the cells;
  the swap is checked offline below, a hand-over is not.
- **Co-op:** `offlineOnly`, and `SilenceSettingCoopLive` says no for `HandPickup`, so the site and cave never go in with
  the adapter on. Not claimed for co-op until a private session checks it (the cave already compares the record with the
  camera focus, as A-G do).
- **Kind 0** (the action byte 26) is not redirected: a squeeze then times out (logged) and nothing happens.
- **Cave RAM:** `0xFEC00`-`0xFEC94` only (another agent's caves use `0xFE900`-`0xFEBFF`; PhysicalMelee's fake object starts
  at `0xFEE80` and is left clear). Every File #1 dump in the scratchpad (18) reads zero over `0xFEC00`-`0xFF000` and
  `0xFF200`-`0xFF300`. The host block is checked (zero, or carrying our tag) before the first arm of each enable.
