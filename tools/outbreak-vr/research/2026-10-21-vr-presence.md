# VR presence: seeing your partner's real VR head and hands in private co-op (2026-10-21)

Branch `vrpresence` (from `nightly`). `[VR] VrPresence`, off by default. Evidence words as elsewhere: **written**,
**reviewed**, **built**, **self-tests passed**, **checked offline loopback** (one headset-free emulator and a Python
echo standing in for the partner). Nothing here is checked online, on a headset or between two machines yet.

## What it does

In private co-op each console sees the other player only as the game's canned character animation: the game sends
position, heading, action and aim (`2026-10-14-melee-coop.md`), never a head or a hand. With `VrPresence` on, two
PenguinScreen2 consoles also send each other their players' VR head and both hands over a small side channel of their
own, and each draws the partner's VR head (a simple shell with a dark visor) and hands (the CC0 hand model, a teal
glove, curled by the grip) beside the partner's game character, moving as the partner really moves. The game's
character stays as it is; the game's own netcode, memory and traffic are not touched.

## The protocol (`pcsx2/VR/VrPresence.*`, `tools/outbreak-vr/presence_echo.py`)

- **UDP, one fixed 156-byte packet, about 30 a second each way** (under 5 KB/s). Little-endian:
  magic `VRP1`, version 1, flags (head / left / right valid), slot, room (u16), grip x2 (u8), key (u32), session (u32),
  sequence (u32), the sender's clock (u32), the sender's character position (3 f32, game units), then head, left hand
  and right hand each as a position and two unit directions (forward, up; 9 f32), trigger x2, and an FNV-1a checksum
  over all of it.
- **Game space, not headset space.** The sender maps its poses with CameraDriver's own mapping (`GameFromVr`: game
  centimetres, Y up), the same mapping hand targeting uses; the receiver maps them into its own headset with its own
  mapping (`VrFromGame`). Orientations travel as two directions mapped through the map's own points, so a mirroring map
  (yaw sign -1) needs no special case. Round trip in the self-test: 0.1 mm, 0.06 deg.
- **Anchored to the partner's character.** The receiver moves the poses by (its copy of the partner's record position
  minus the packet's character position), so the VR head sits on the character the receiver sees even when the game's
  own copy of that character lags or drifts. A part more than 3 m from its own character is dropped.
- **Who and where.** The partner is the record whose identity byte `+0x03` is the packet's slot (the game's own
  local-player test), in use (max HP `+0x546` not zero), with its room (`+0x1E2`) equal to this player's room
  (`0x3065AC`), and the packet's room equal too. A packet carrying this console's own slot is ignored. The profile's
  `presence` block names these (reads only); the records, stride, count and identity byte are `camera.base`'s.
- **Stale and foreign packets.** Only packets from the peer's address are read; then size, magic, version, checksum,
  finite and sane numbers, the key (the hash of `VrPresenceKey`, a word both set alike; empty is fine), and the
  sequence: a later number of the same session (wrap-aware), or a new session only once the old one has been quiet
  1 s (a restarted console). A partner whose newest packet is over 250 ms old is not drawn.
- **No blocking.** The socket is non-blocking on its own thread (10 ms select). The CPU thread (`Poll`, after
  `CameraDriver::Apply`) and the GS thread (the compositor) only swap small structs under a mutex.

## Safety

- Off by default; ini only: `[VR] VrPresence = true`, `VrPresencePeer = host:port`, `VrPresencePort` (this console's
  listening port, 47800), `VrPresenceKey`.
- The peer must be a private IPv4 address: a LAN (10/8, 172.16/12, 192.168/16), Tailscale (100.64/10) or this PC
  (127/8); a name (a Tailscale MagicDNS name) is resolved and its address checked the same way. Anything else, the
  public internet and so obsrv, is refused with a log line and nothing is sent. Packets from any other address are
  dropped.
- Nothing is sent, and the socket is not even open, unless first person is armed (its record resolved and the head
  mapping published this vsync) **and** CameraDriver's network mode is private co-op. The one exception is the offline
  loopback test: `PCSX2_VR_PRESENCE_LOOPBACK=1` in the environment, the game strictly offline and the peer on 127/8.
  With a loopback peer the socket listens on 127.0.0.1 only, so the test needs no firewall rule.
- No game memory is written: `Poll` reads the local slot, the room and the records' positions, rooms and max HP.
- The channel is not the game's connection (DEV9 is not involved; the game's packets are unchanged).

## Drawing (`ZoneMarkers::kPartner`, `XRCompositor` BuildZoneLayers)

- One impostor image (256 x 256, the zone markers' size) holds the head and both hands, on one quad facing the eye from
  their middle, just big enough for them (0.1 to 2.4 m). It is one more zone-marker slot: placed after the holster and
  before the belt bag, the rings and the hand-targeting box when layers are short (`ZoneMarkers::Plan`).
- **The 16-swapchain limit.** The partner's swapchain is made only once a partner has actually shown up (never with the
  setting off, never before anyone is seen). If it cannot be made (SteamVR's limit) the partner is skipped for the
  session with one log line, `(VR) Presence: no swapchain for the partner's head and hands ...`; the markers and
  everything else keep theirs (unlike the other zone slots, whose failure turns the markers off).
- It is drawn again at most at the packets' rate (33 ms), or when what it shows changes; between, the last image rides
  with the partner's head. Cost: about the hands' (two hand meshes and two small ellipsoids, 2 x 2 samples).
- Not drawn while a game screen is up (as the zone markers).

## Logs

- `(VR) Presence: on, peer ..., listening port ...` / `... refused ...` / `... is not host:port ...`.
- `(VR) Presence: live` / `waiting: not in private co-op (nothing is sent)` / `waiting: first person is not armed`.
- `(VR) Presence: channel open: listening on ..., sending to ...` and `channel closed (...); N sent, N received,
  dropped N foreign, N bad, N wrong key, N stale`.
- `(VR) Presence: a partner is drawn (packet slot S room R, this console slot L room R)` or why not (no fresh packet,
  own slot, another room, record not used ...), on each change.
- Every 2 s in the loopback test or with `PCSX2_VR_PRESENCE_LOG=1`: the local head and the partner's head, facing and
  hands in the headset's space, its record and the packet's character in the game's.
- `PCSX2_VR_PRESENCE_DUMP=<folder>`: one PNG of the partner's image as seen from the local head, 3 s after one is first
  drawn (for inspection on the PC only; never committed).

## Self-tests (`VR presence`, in `tests/vr/VRSelfTests.cpp`)

Encode and decode (every field back exactly; a short buffer, a flipped bit, another magic, a slot past 3, a non-unit
direction, a NaN, an unknown flag refused), the key hash, dotted quads, the private filter and each range's edges, the
peer parser, the stale and reorder filter (first, later, repeat, older, another session while live, a restart after
1 s, wrap), the pose round trip through a normal and a mirroring map, a point 1 m ahead at 100 units and eye height,
a partner seen through another console's mapping and moved (back to the shifted game point, facing the same way), the
placement (size, facing the eye, the eye inside: none) and the raster (head and hands drawn, the visor dark from the
front and hidden from behind, teal gloves). Also `Zone markers`: the partner's place in the layer plan.

## Offline loopback check (`tools/outbreak-vr/presence_test.py`)

One headset-free emulator (no OpenXR runtime, PINE scripted head and hands, File #1 slot 6, offline, normal speed)
with `VrPresencePeer = 127.0.0.1:47801`, and `presence_echo.Echo` on 127.0.0.1:47801 sending each packet back as slot
1 (an AI partner). **Checked offline loopback, 2026-10-21** (run-presence, local build of 0fe3ada):

- `(VR) Presence: live (offline loopback test)`, `channel open: listening on 127.0.0.1:47800, sending to
  127.0.0.1:47801 ..., about 30 packets a second of 156 bytes`, then `a partner is drawn (packet slot 1 room 1, this
  console slot 0 room 1)` 52 ms after the channel opened.
- **Placement** (scripted head at the origin, hands at (-0.20, -0.35, -0.35) and (0.22, -0.30, -0.40)): the partner's
  VR head at (3.941, 0.200, 3.297) m with AI partner 1's record at game (8392.0, 20.0, 12642.2) and Kevin at (8396.0,
  0.0, 13156.0): horizontal distance 5.1383 m against the records' 5.1382 m (0.03 mm), height 0.200 m against 0.20,
  each hand moved exactly as the head (under a micrometre), facing (0, 0, -1) as our own head. As partner 1 walked
  about J's Bar the drawn head followed its record (three positions logged over 4 s).
- **Turn and move:** our head turned 40 deg left and the right hand 20 cm out: the partner's head faces (-0.643, 0,
  -0.766) (0.0002 off), the distances 4.0516 m against 4.0517 m, the hands again exact.
- **Own slot:** the echo claiming slot 0: `the peer's packets carry this console's own slot`, nothing drawn.
- **Wrong key:** `no fresh packet from the peer`, the counts `50 wrong key` two seconds later; back to the right key,
  drawn again. **Echo stopped:** `no fresh packet from the peer` within 250 ms.
- 0 foreign, 0 bad, 0 stale over about 600 packets each way; the echo's own decoder (Python, written from the format
  above, not from the C++) read every packet.
- The dump (`PCSX2_VR_PRESENCE_DUMP`, kept on the PC): from 5 m, a 0.82 m quad, 5,216 pixels drawn: the grey shell with
  its dark visor toward us, the right hand curled (grip 0.8), the left nearly open (0.2), both teal.
- Not exercised: the compositor's layer (no OpenXR runtime; the null headset was not used: other agents' sessions and
  SteamVR stay untouched), so the swapchain, the 16-limit fallback, the layer plan and the kept image are built and
  reviewed only.

## The two-machine test (Alex and his brother; needs Alex's go)

Setup (both):
- Both run a build of this branch, private co-op exactly as it already works (`netmode.py private`,
  `PrivateCoopFirstPerson = true`), File #1, plain ISO, 100% speed, nothing paused, no states.
- In `inis\PenguinScreen2.ini`, `[VR]`:
  - Alex: `VrPresence = true`, `VrPresencePeer = <brother's Tailscale address>:47800` (`tailscale ip -4` on his PC,
    `100.x.y.z`; or his LAN address), `VrPresencePort = 47800`, `VrPresenceKey = <a word you both pick>`.
  - Brother: the same with Alex's address.
- The first time, Windows may ask whether `pcsx2-qt.exe` may use the network: allow it on **private** networks only
  (Tailscale counts as private). Nothing else changes; no router or firewall rule.
- Each emulog at the start: `(VR) Presence: on, peer 100.x.y.z:47800 ...`; once in the scenario with first person
  armed: `(VR) Presence: live.` and `channel open: listening on 0.0.0.0:47800, sending to 100.x.y.z:47800`.

Steps:
1. **Same room.** Both stand in J's Bar. Each log: `(VR) Presence: a partner is drawn (packet slot B ...)`. In the
   headset, a grey head with a dark visor sits on the partner's character's head and two teal hands near its hands.
   Wave, nod, look around, point: the other sees it as it happens (well under a quarter of a second).
2. **Distance and turning.** Walk apart, turn away and back: the head and hands stay on the character as it moves;
   your own turning does not move them in the world.
3. **Rooms.** One walks into another room: the other's log says `the partner is in another room` and nothing is
   drawn; back in the same room, drawn again.
4. **One without it.** One sets `VrPresence = false` and restarts: the other's log says `no fresh packet from the peer`
   and draws nothing; the game itself plays exactly as before on both.
5. **Menus.** Open the item screen: your partner's head and hands are hidden while it is up, back after.
6. After the session, each runs `PCSX2_VR_PRESENCE_LOG=1` next time if the placement looks off, and sends the
   `(VR) Presence:` lines.

What to look for: the head sits on the right character and at the right height; the hands at the character's sides,
not inside walls; nothing stutters in either game; and any `dropped ... stale` or `wrong key` counts in the
`channel closed` line (a mismatched `VrPresenceKey` shows as all wrong key).

## Not done / risks

- **Never between two machines.** Tailscale's latency and loss, the Windows firewall prompt and a real second
  player are untested.
- **Never drawn in a headset or on the null headset.** The compositor path is built and mirrors the zone markers; its
  layer, swapchain, placement and kept-image handling have not run. The offline check exercises everything up to
  the poses the compositor reads, and the raster itself (the dump).
- **Calibration.** The VR head is placed by the partner's own recentre: if the partner recentres badly (head far from
  the character's eyes), the head floats off the character by the same amount (capped at 3 m).
- **Image resolution.** 256 x 256 over up to 2.4 m: a partner with arms spread close up is soft.
- **The partner's own body.** Kept as it is: the game's character animation still plays under the VR head and hands.
- **Rooms.** The room check reads the game's u16 room; across a load the partner disappears until both are in.
