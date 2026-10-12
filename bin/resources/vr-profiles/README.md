# VR profiles

One file per game, named by the disc serial (`SLUS-20851.yaml`). A profile
holds the VR tuning for that game: where the virtual screen sits and how much
stereo depth the picture gets. The profiles are licensed separately from the
emulator; see `LICENSE.md` in this folder.

## Changing a profile

Do not edit the files here. They are replaced on every update and are
read-only in a flatpak install.

1. Copy the game's file into your **user profiles folder**. The emulator
   creates that folder on first use and leaves a short note in it, along with
   a ready-to-edit copy of one shipped profile, `SLUS-20851.yaml` (Ace Combat
   5). Because that copy is in your folder, it is the one in effect for that
   game.
2. Edit the copy. A user file overrides the shipped file for the same serial,
   whole file for whole file.
3. A log line containing `user profile overrides shipped` confirms your copy
   is in effect. It appears whenever the profiles are loaded: when the
   emulator starts, and again after you change a profile file.

To go back to the shipped profile, delete your copy. If you delete the seeded
`SLUS-20851.yaml`, the emulator recreates it as a fresh copy of the shipped one,
which gives the same result. While any copy of a game's profile is in your
folder — including the untouched seed — updates to that game's shipped profile
do not reach you until you delete it.

Every profile is checked at launch. An invalid file is reported in a dialog
that names the file and the reason, and that one game falls back; other games
are unaffected. Keys the emulator does not know are ignored, so a newer profile
still loads on an older build.

## File layout

```yaml
SLUS-20851:              # the disc serial; must match the file name
  name: "Ace Combat 5: The Unsung War"   # display name
  crcs: [0x39B574F0]     # optional: only apply to these disc CRCs (hex)
  tier: stereo           # screen | stereo | immersive
  screen: { ... }        # where the virtual screen sits
  presentation: { ... }  # optional: brighter picture in the headset only
  feedback: { ... }      # optional: controller vibration from the game
  hud: { ... }           # optional: a status card on your wrist
  stereo: { ... }        # depth tuning
```

`tier` states what the profile is meant to provide: `screen` is a flat virtual
screen, `stereo` adds per-eye depth, `immersive` adds a head-driven camera. It
is a label: it does not switch anything on or off. A value outside those three
rejects the profile.

## `screen`: the virtual screen

| Key | Meaning |
|---|---|
| `distance` | metres in front of you |
| `height` | metres above the floor |
| `follow` | `head` keeps the screen in front of you as you turn; `world` leaves it fixed in the room |
| `arc` | curve of the screen in degrees; below `5` the screen is flat. Needs a VR runtime with cylinder-layer support, otherwise the screen is shown flat |

A `distance`, `height` or `arc` written here wins over the matching slider in
the settings window for that game. Leave a key out to keep it adjustable from
the settings window.

`menu: { distance, height, drop, tilt }` places the menu tablet: with the Menu
Tablet setting on (off by default), a menu that comes up during first-person
play is shown on a small flat screen `distance` metres ahead of your eyes
(`0.3` to `2.0`, default `0.70`) and `drop` metres below them (`-0.5` to `0.8`,
default `0.30`), `height` metres tall (`0.15` to `1.0`, default `0.36`), tilted
back to face your eyes. `tilt` (degrees, `0` to `89`) fixes the tilt instead.
The tablet is always flat. It is placed in front of you each time it appears.

## `presentation`: brightness in the headset

Some games are too dark to read on a headset's display. `presentation`
brightens the picture only while a VR session is running (the desktop window
mirrors it then); playing without VR never changes. `1.0` is neutral for both
numbers.

| Key | Meaning |
|---|---|
| `gamma` | raises dark and middle tones while black stays black and white stays white. `1.2` is a mild lift; accepted range `0.5` to `2.5` |
| `brightness` | multiplies every colour, which can clip bright areas; prefer `gamma`. Accepted range `0.5` to `2.0` |
| `firstPersonOnly` | `true` (the default) applies the lift only while the head-tracked first-person camera is active, so menus and item screens keep their own levels; `false` applies it all the time |

The Headset Brightness setting in the settings window, and the VR brightness
hotkeys, scale `gamma` on top of this, in every scene and for every game.
While the graphics option Shade Boost is on, its own values are used instead
and neither this block nor the setting has any effect.

## `feedback`: controller vibration

`feedback` makes the VR controllers vibrate from what happens in the game. It
only reads the game's memory, so it changes nothing other players could see,
and it does nothing without a running VR session.

| Key | Meaning |
|---|---|
| `padRumble` | `true` sends the game's own controller vibration to the VR controllers: the large motor to both hands, the small one to the right. Only for the controller port of the profile's `Gamepad` control, and only while the Forward Game Rumble setting is on |
| `hurt` | `{ offset, max, width }`: where the character's HP (`offset`) and maximum HP (`max`) are, as offsets into the record `camera.base` finds, each `width` bytes (`1`, `2` or `4`, default `2`). Losing HP pulses both hands, harder and longer the bigger the hit |
| `fire` | `{ address, width, hand }`: a counter the game raises by one per shot, at a fixed `address`. Each rise of 1 to 5 kicks `hand` (`left` or `right`; default the aiming hand of `camera.lookAt`, otherwise right). Bigger jumps are ignored |
| `danger` | `{ below }`: while HP is above zero and below this fraction of maximum HP (default `0.25`), the left hand feels a heartbeat, faster below a tenth. Needs `hurt` |
| `weapon` | `{ equipped, table }`: the byte at `equipped` in the record `camera.base` finds is the number of the equipped item's record (`0`: nothing equipped) in the item table at the fixed address `table`. A record counts only while its first byte is `1`, its third byte is its own number and its owner byte is the local player's slot plus one. Its count dropping while the weapon is raised kicks the aiming hand by the item's kind (a long gun harder and longer, and the left hand too while it holds the fore-end; something to throw or a spray can lightly; a melee weapon not at all), a rise ticks both hands. The first-person right hand holds that kind's shape (a pistol, a long gun, a melee weapon, something to throw or a spray can; the laser sight only on a gun), the wrist card names the weapon from it, and belt pouches name their items from the same table. The record layout defaults to Resident Evil Outbreak's (`stride`, `id`, `count`, `owner`, and `stance`, `raised`, `action`, `reload` for the raised and reload checks, all hex) |

The `hurt`, `fire` and `danger` cues run only while the head-tracked
first-person camera is active, so they stay quiet on title screens, in
cutscenes and with first person off. The Haptic Strength setting scales all
of this vibration, and `0` turns it off.

## `hud`: a status card on your wrist

`hud: { wrist: { ... } }` puts a small card on the back of your left wrist
while the head-tracked first-person camera is active. Turn your wrist toward
your face, as if checking a watch, and it fades in. It shows the character's
condition (FINE, CAUTION below 60% of maximum HP, DANGER below 20%, BLEED
while bleeding), an HP bar, and the virus gauge. With `feedback.weapon` in the
profile and a weapon equipped, a line under the bar shows the weapon's name and
rounds (`45 AUTO 7`; a knife shows its name alone). Like `feedback`, it only
reads the game's memory and does nothing without a running VR session.

| Key | Meaning |
|---|---|
| `hp` | `{ offset, max, width }`: the character's HP and maximum HP, as offsets into the record `camera.base` finds, each `width` bytes (`1`, `2` or `4`, default `2`) |
| `virus` | `{ offset, maxTable, charOffset }`: a 4-byte virus counter at `offset` in the record, shown as a percentage of the character's maximum. The maximum is the 4-byte entry for the character id (the byte at `charOffset` in the record) in a table at the fixed address `maxTable` |
| `bleed` | `{ offset, width }`: the character is bleeding while this value in the record is not zero |

Each part is optional; the card leaves out whatever it can't read sensibly.

## `breakout`: shake to break free

When the game holds your character (a zombie's grab in Resident Evil
Outbreak), shaking either VR controller makes your pad struggle for you: the
left stick waggles left and right, and Circle and Cross are pressed in turn,
with a short buzz on the shaking hand for each press. It works whether or not
the first-person camera is active (a grab hands the camera back to the game),
needs a `Gamepad` control in `controls`, and does nothing without a running VR
session. The presses are your own pad input, so they are allowed in private
co-op, but never on any other online server, never while the item screen, map
or pause is up (`camera.lookAt.pauseWhen`), and never once the grab ends: they
stop on the same input poll.

```yaml
breakout:
  grabbed:
    - { offset: 0x08, equals: 5, width: 1 }               # stance: grabbed
    - { offset: 0xBA0, min: 0x00, max: 0x17, width: 1 }   # status: normal play
  shakeSpeed: 1.0
  pressHz: 8
```

| Key | Meaning |
|---|---|
| `grabbed` | checks on the record `camera.base` finds, written like `camera.base`'s `check` list (`offset`, then `equals`, `notEquals`, or `min` and `max`, and `width`). All must hold while your character is held. Required; one entry that does not parse makes the emulator ignore the whole block |
| `shakeSpeed` | a hand counts as shaking while its controller moves faster than this, in metres per second (`0.2` to `5.0`, default `1.0`) |
| `reversals`, `reversalWindowMs`, `reversalSpeed` | or while it reverses direction this many times (`0` to `8`, default `3`; `0` turns this test off) within this many milliseconds (`100` to `1000`, default `300`), moving at least `reversalSpeed` metres per second (`0.05` to `3.0`, default `0.25`). This catches a small, quick shake |
| `holdMs` | a hand keeps counting as shaking this long after its last shake (`0` to `1000`, default `250`), so the turn at the end of each stroke does not stop the presses |
| `stickHz` | full left-right cycles of the left stick per second while shaking (`0` to `15`, default `6`; `0`: no waggle) |
| `pressHz`, `pressMs` | Circle and Cross presses per second, Circle first and then taking turns (`0` to `15`, default `8`; `0`: no presses), each held this many milliseconds (`20` to `500`, default `60`). A press that would last as long as the time between presses is shortened to half of it |
| `buzz`, `buzzMs` | the buzz on the shaking hand for each press: strength `0` to `1` (default `0.35`; `0`: none) and milliseconds (`5` to `100`, default `20`). The Haptic Strength setting scales it |

A button you hold yourself is never let go: while you hold A or the trigger,
no Circle presses are added, and while you hold B, no Cross presses. A
sideways push of your own on the left stick, at least half way, wins over the
waggle. The first grab after the emulator starts shows "Shake to break free!"
in the headset.

## `menuPointer`: point-and-click menus

With the Menu Pointer setting on (the default), a menu opened during
first-person play shows a beam from your controller and a dot where it points
at the menu screen (the world screen, which must be flat, or the menu tablet),
and both hands are drawn pointing. `menuPointer` maps one of the game's menus
so that pointing also moves the game's own cursor: when you point at a cell,
the emulator presses the D-pad on your pad, one press at a time, and reads the
game's cursor after each press until it lands. The trigger stays your confirm.
If you pull it before the cursor has arrived, it is held back until the cursor
gets there, and if you let go first it never reaches the game. Nothing else is
ever pressed for you.

```yaml
menuPointer:
  when: [ ... ]          # guards: all must pass while the mapped menu is up
  liveWhen: [ ... ]      # optional guards: the menu counts as up once any one has passed
  suspendWhen: [ ... ]   # optional guards: steering waits while any one passes
  cursor: { node: 0x720B84, index: 0x720B8C, x: 0x720B8E, y: 0x720B90 }
  cells:
    - { node: 0x720C10, index: 0, x: 498, y: 108, rect: [0.807, 0.184, 0.895, 0.307], right: 1, down: 2 }
```

| Key | Meaning |
|---|---|
| `when` | a list of guards, written like `camera.guards`, that must all pass while the menu is up. Required |
| `liveWhen` | guards for when `when` can also pass on another screen (a cursor struct that keeps its values after the menu closes, under a menu flag the map and pause share). The menu counts as up only once any one of them has passed since the `when` guards last started passing, and stops counting the moment `when` fails. Use a value that is true where the menu opens, even if it is not true everywhere in it (Outbreak's own-grid byte reads 0 in the equipped box). If none of them ever passes, point-to-select stays off and the log says so as the menu closes. Optional; every entry must be a valid guard, or the whole block is ignored |
| `suspendWhen` | guards for a submenu or any state where the D-pad does something else. While any one of them passes nothing is pressed (a press under way is let go at once) and the trigger is yours: a fresh pull goes straight to the game, and one held back before the guard passed stays back until you let go, so it can never confirm a submenu entry on its own. After the guard stops passing steering waits 6 vsyncs more, then until you point at a different cell, so closing a submenu never moves the cursor to wherever you left the dot. After every confirm that reaches the game steering also waits 0.4 s, so the guard of a submenu that confirm opens has time to pass. Left empty, steering instead waits after every confirm that reaches the game, until Cross, the cursor moving to another cell, or the menu closing |
| `cursor` | the addresses of the game's cursor: a 4-byte `node` pointer to the cell it is on, and 2-byte `index`, `x` and `y` |
| `cells` | 1 to 16 cells, each with `node`, `index`, `x`, `y` (the cursor's values on that cell; all four must match), `rect` (where the cell shows: left, top, right and bottom as fractions `0` to `1` of the game picture) and `up`, `down`, `left`, `right` (the number, counting from 0, of the cell one D-pad press in that direction leads to; leave out a direction that leads nowhere, or out of the cells) |
| `hand` | `right` or `either` (the default): the right hand points, and the left hand's aim stands in only while the right controller is not tracked, so the left controller (the Steam Frame's D-pad hand) never takes the pointer while the right one just points off the screen. `left`: the left hand points |
| `coop` | `true` lets it steer online too, which happens only in private co-op. Default `false` |
| `pressVsyncs`, `gapVsyncs` | each D-pad press is held this many vsyncs, then released at least this many (default `2` and `2`) |
| `settleVsyncs` | how long to wait for the cursor to move after a press before giving up (default `10`). The second press in one opening of the menu that the cursor does not answer turns point-to-select off until the menu opens again |
| `maxPresses` | the most presses for one move (default `6`) |
| `dwellMs` | how long you must point at a cell before the cursor goes there (default `60`; the Outbreak profiles use `150`, since a real hand drifts across a cell's edge) |
| `armDelayMs` | nothing moves for this long after the menu opens (default `300`) |
| `enterInset` | a cell counts only inside its `rect` shrunk by this fraction of its size on each side (default `0.12`); you leave it only outside its full `rect` |

Watch the number formats. `node` and the `cursor` addresses are hex, like every
address in a profile. A cell's `x`, `y`, `index` and neighbour numbers are
**decimal** unless written with `0x`: `x: 498` means 498, not `0x498`.

Steering stops if the cursor stops answering, goes outside the mapped cells,
or needs more than `maxPresses` presses. It also stops the moment you use the
D-pad, a stick or a grip (except a grip squeezed in a belt pouch, below), press
B or Menu, or the cursor moves without the pointer (the left stick, the game
itself); your move stands until you point at a different cell than the one the
dot was on, and the dot wobbling while your thumb works a stick does not count.
A pull goes to the cell the dot was on as the trigger began to move, so the dip
a pull gives the controller never sends the confirm to another cell. A trigger
or A still held as the controls come back from a gap (the SteamVR dashboard)
does nothing, in a menu or out of one, until you let it go once. If the
cursor's node matches a cell but its index, `x` or `y` do not, the layout is
not the one mapped, and point-to-select stays off until the menu opens again. Any invalid
part of the block makes the emulator ignore the whole block, with a log line
saying why.

## `pouches`: belt pouches on your body

In first-person play, `pouches` puts small pouches on your body, each tied to
one of the game's item cells, like the belt in Dungeons of Eternity. Reach a
hand into a pouch and the controller ticks once, lightly, if that cell is
empty, or twice and firmer if it holds an item. With `feedback.weapon` in the
profile, a short message in front of you also names the item, with its count
where it has one ("Kevin's 45 Auto 7", "45 Auto Magazine x7", "Green Herb"),
once each time you reach in and never while a menu is up. Squeeze the grip
there and the game's item screen opens with its cursor on that pouch's cell;
if the screen is already open, the cursor just moves there. You still pick the
item yourself with the trigger. Nothing is ever confirmed for you, and the item
bytes are only read. The pouches work only while first person is running.

```yaml
pouches:
  anchor: seat           # seat (default) or head
  open: belt             # the id of the Zone control that opens the item screen
  suspendWhen: [ ... ]   # optional guards: a squeeze is refused while any one passes
  list:
    - { cell: 0, item: 0xC84, side: -0.10, height: -0.20, forward: 0.14 }
```

| Key | Meaning |
|---|---|
| `list` | 1 to 8 pouches. Each has `cell` (the number, counting from 0, of the `menuPointer` cell its squeeze steers the cursor to), `side`, `height` and `forward` (metres from your eyes, placed the way a Zone control's `placement` is, measured sitting upright and looking ahead), and optionally `item` (a hex offset into the record `camera.base` finds: the byte there is `0` while the cell is empty, and otherwise the number of the item's record in `feedback.weapon`'s item table, which names it) and `radius` |
| `anchor` | `seat` (the default) hangs the pouches from your neck and turns them to the forward you last recentered to, so looking down at a pouch or glancing at it does not move it. `head` places them the way the Zone controls are placed, turning with your head |
| `open` | the `id` of one of the profile's Zone controls. A squeeze with the item screen closed presses that zone as a squeeze inside it would, including its `stopFirst`. Without it, a pouch only moves the cursor of an item screen that is already open |
| `hand` | `either` (the default), `left` or `right` |
| `radius` | how far from its point a pouch reaches, in metres (`0.02` to `0.20`, default `0.065`) |
| `suspendWhen` | guards, written like `camera.guards`, for a submenu of the item screen. While the item screen is up and any one of them passes, a squeeze is refused, and the menu pointer's own steering waits too |

Steering the cursor uses `menuPointer`, so it follows the Menu Pointer setting
and the same rules: it waits for `armDelayMs` after the screen opens, never
runs while steering is suspended (a `menuPointer.suspendWhen` submenu and the
6 vsyncs after it, the 0.4 s wait after a confirm, the latch without
`suspendWhen`, or a cursor that stopped answering presses: a squeeze then is
refused with a dull buzz), and stops if you take over with the D-pad or
a stick. The Zone controls come first: a hand within a zone's reach (its
`radius` plus 3 cm) is that zone's, never a pouch's, when the zone takes that
hand, so a pouch overlapping a hip zone never changes what a squeeze at the hip
does. Elsewhere a hand inside a pouch belongs to that pouch only, and no zone
sees it there. Keep each pouch's point well clear of the zones that take the
same hand (the zones turn with your head, seat-anchored pouches do not). Any
invalid part of the block
makes the emulator ignore the whole block, with a log line saying why.

## `stereo`: depth

| Key | Meaning |
|---|---|
| `separation` | how far apart the two eyes' images are pushed. Larger is deeper. Start small: in the shipped profiles the values directly under `stereo:` (outside `scenes`) run from `0.007` to `0.02` |
| `convergence` | the depth that sits exactly at the screen surface. Content nearer than this stays at the screen; content beyond it goes into the screen. The useful range depends entirely on the game: in the shipped profiles the values directly under `stereo:` run from `0.005` to `6.0`, so adjust from the shipped value and do not copy one game's number to another |

Depth goes *into* the screen, never out toward you, as long as `separation`
is positive. The emulator does not check its sign, and a negative value turns
the depth inside out. A negative `hudCollimate` `disparity` (below) likewise
places aiming marks in front of the screen; it is accepted with a warning. Keep
both positive.

If far scenery looks doubled or is uncomfortable to look at, lower
`separation`. If the whole picture looks flat, raise it, or lower
`convergence`. This applies to the plain pair; when a depth map is in use,
adjust the map's own values instead (see below).

### Different depth for different parts of a game

One `separation` / `convergence` pair rarely suits both gameplay and menus, or
both a cockpit and the world outside it. Two tools cover that.

**`scenes`** switches the pair when the game changes state. The shipped
profiles use this for menus and alternate camera views. Each rule has a `when`
block that identifies the game state; leave those as shipped. The values you
can adjust are each rule's own `separation` and `convergence`. A rule that
leaves one out inherits the base value. The first matching rule wins. A rule
with `separation: 0` shows that state flat, on the screen's own plane: the
Outbreak profiles do this while a menu is up, so the menu pointer's dot sits on
the menu instead of in front of or behind it.

**`map: bands`** gives each depth range its own setting, so near objects and
the distant world can both have comfortable depth at once:

```yaml
stereo:
  map: bands
  splits: [...]          # where one band ends and the next begins, near to far
  bands:                 # exactly one more entry than splits; first is nearest
    - { conv: 2.0, sep: 0.008 }
    - { conv: 2.0, sep: 0.006 }
    - { conv: 8.0, sep: 0.009 }
```

At most 3 splits and 4 bands. `sep` must be greater than 0 and `conv` must not
be negative. The joins between bands are worked out for you so depth stays
continuous. An invalid band set is reported in the log and ignored: in the
block directly under `stereo:` the game falls back to the plain `separation` /
`convergence` pair, and in a scene it falls back to whatever the block under
`stereo:` uses. A band set that exceeds the
comfort budget is loaded with a warning.

`map: log` is a smooth alternative with no band edges:

```yaml
stereo:
  map: log
  log: { w0: 2000, w1: 22000, dfar: 0.02 }   # near anchor, far anchor, depth reached at the far anchor
```

`w1` must be greater than `w0`, `w0` greater than 0, and `dfar` must not be
negative.

While `map` is `bands` or `log`, the plain `separation` / `convergence` pair is
not used, in the base block or in a scene: adjust the band or `log` values
instead. The plain pair takes over only if the map is invalid.

### First person: `firstPerson`

Used instead of the values above while the profile's first-person camera
(`camera.lookAt`) drives the game camera, with the picture on the head-locked
first-person screen. It wins over `scenes`.

```yaml
stereo:
  firstPerson: { separation: 0.0132, convergence: 150, nearest: 1.0, auto: true }
```

| Key | Meaning |
|---|---|
| `separation` | the first-person separation. With `auto`, the fallback |
| `convergence` | the first-person convergence, in game units. With `auto`, the fallback |
| `nearest` | metres, `0.45` to `1.5`: how near true depth reaches in front of the screen plane before it flattens. Measured as the distance you see, so it holds at any strength. Only perspective-textured draws get it, so the game's 2D text stays on the screen plane and geometry nearer than that plane shows in front of text drawn over it; the Outbreak profiles use `1.0` (not `0.75`) until the near field has been judged in the headset. VR `NearDepthMetres` overrides it (below 0 turns it off: `[VR] NearDepthMetres = -1` if the game's own text looks doubled) |
| `auto` | `true` works the pair out from the headset: separation = IPD / the first-person screen's width, convergence = its distance x `camera.lookAt.unitsPerMeter`. Far content then lands exactly one IPD apart and the screen plane sits at the screen's distance. Needs both `separation` and `convergence`, which are used until the headset reports its eyes, when the IPD is outside 45-80 mm, when the screen is under 1 m or over 12 m wide or nearer than 0.5 m or farther than 5 m, or with VR `FirstPersonAutoStereo = false` (ini only). VR `FirstPersonStereoStrength` (0.5-1.5) scales either |
| `ndcWidthRatio` | optional, `0.5` to `2.0`, default `1.0`: how much wider the first-person draws' NDC is than the displayed picture. The `auto` separation is divided by it. The log's 'First-person multiview draw' line reports the measured ratio; leave it at 1.0 unless that line says otherwise |

Each first-person arm (and each later change, such as a new strength) eases
the depth in over half a second. The log's 'First-person stereo' line shows the
values and where they came from.

### HUD aiming marks: `hudCollimate`

Flat HUD elements stay at the screen surface, which is right for text. Aiming
marks such as a target bracket are easier to use when they sit at the depth of
the thing they surround. `hudCollimate` moves matched HUD draws to a fixed
depth.

`disparity` is that depth. Values above `0.0135` load with a comfort warning,
and values above `0.01815` make the emulator ignore the `hudCollimate` block;
the rest of the profile still loads. The `rules` list selects which HUD
draws are affected and is specific to each game; leave it as shipped. This
feature works on the Vulkan renderer only.

## Everything else

Shipped profiles also contain `camera`, `writes`, `guards`, `base` and similar
blocks. These tie the profile to one specific game build and are not
documented here. Changing them can crash the game or corrupt a session. Leave
them exactly as shipped.
