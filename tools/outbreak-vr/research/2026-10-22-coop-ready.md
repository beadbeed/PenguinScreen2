# Co-op ready: the online paths, checked offline (File #1, 2026-10-22)

Branch `coopready` (from `nightly` 7b2138a). Goal: Alex and his brother play a whole map in private co-op, both in VR,
and it feels like the offline build. Until now almost everything had only run offline. Nothing here has run between two
machines or online; every check below is offline, headset-free (no OpenXR runtime, scripted PINE controllers), normal
speed, on the `run-coop` dev install (its own PINE slot 28150, savestates copied from Alex's slots 3, 5, 7).

Labels: **verified (code)** read from the disassembly of a NETAQ-resident RAM dump; **verified (offline)** seen running
with the test override; **assumption** otherwise.

## 1. The test override (`VR::NetGate`)

Every VR read of `EmuConfig.DEV9.EthEnable` goes through `VR/NetGate.h` (`IsOnline`, `TestOverrideActive`,
`StrictlyOffline`): CameraDriver's network mode, online-safe and `whenSetting` gates, GunRay, PhysicalMelee, SpatialAudio
and VrBody's locals rule.

`PCSX2_VR_TEST_NET=privatecoop` (read once, a loud warning, and `[TEST OVERRIDE]` on the network-mode line): with the
adapter **off**, the VR layer takes its online code paths. With `[VR] PrivateCoopFirstPerson` the mode is private co-op
(the private login host check taken as passed), without it restricted, as online. It only selects code paths:
- the adapter stays off;
- VR presence keeps to a 127/8 peer under it;
- the emulator's online locks (`VMManager::IsOnlineLocked`) and PINE's refusals still follow the real adapter, so the test
  tools and state loads still work (use `PCSX2_TEST_ONLINE_LOCKS=1` to test the locks);
- with the adapter really on it does nothing at all.

Any other value is ignored and logged. Self-test: "Network gate" (16 checks). Verified (offline): the override and the
switch give "network mode private co-op [TEST OVERRIDE]", the co-op toast and "Gun ray: co-op aim"; no override gives
offline; the override without the switch gives restricted; a bogus value is ignored.

## 2. Safety and oddities

**(a) FadeDim, one dim.** Online, the fade quad skip (`0x1B9DCC`, `whenSetting: FadeDim`) was not co-op live, so the game's
quad drew while the host also dimmed the view: two dims. The skip is draw-only, so it is co-op live now, and the host dims
only while the skip's word (`0x10000029`) is in, wherever it runs. Verified (offline): with the override and a scripted
black fade, before the change the word stayed stock (`0x10600029`) with the host dimming; after it the word is patched
and the host dims once.

**(b) The gun-elevation servo NOP** (profile `0x556968`-`0x5569E8`, `offlineOnly`, which passes in private co-op).
Verified (code):
- `FUN_00556800(record, flag)` is called from `0x55519C`, `0x5555E0`, `0x5555F4` (the stance-2 input handlers) and
  `0x556A84` (`FUN_00556A00`).
- It exits unless the record's `+0x03` equals u8 `0x48BF7D` (`0x556828`).
- Offline the loops `FUN_006383E0` / `FUN_00638850` set `0x48BF7D` to each record in turn, so the servo is shared with the
  AI partners and the NOP freezes their pitch too while you aim. That is the old note's "shared" and it holds only offline.
- Online `0x48BF7D` is the local slot (NETAQ writes it at `0x8223AC`). Remote players never reach the servo
  (`FUN_006399A0` skips their input stage, `0x639A04`-`0x639A3C`).
- A remote player's `+0xBC8` is written only by the type-1 receive (`0x64D410`) and zeroed at a few transitions. Nothing
  eases it.

So in private co-op the NOP holds only your own aim. No change; the profile comment says so. A local-only cave (a `j` at
the servo's entry comparing `+0x03` with `0x3AEF74`) would fix the offline AI-partner freeze. It is not needed online and
was not built.

**(c) Walk-and-shoot** writes the local record's position only. `base` is `camera.base` for the local slot. The game sends
that position in its type-1 messages and the type-2 heartbeat, and receivers write only remote records, so nothing fights
the write. Documented in the code.

**(d) The coopOnly heartbeat** (every 3 frames instead of 30, also with the gun raised). Verified (code):
- `FUN_0064C620` hands messages to NETAQ (`0x820860`), which appends them to a per-slot aggregate (`0x825CC0` via
  `0x820A80`).
- The aggregate is flushed as one packet per slot from `FUN_00822760`, once a frame (assumption on the call rate).
- The game's caps: 172 bytes a channel (`"!!PL DATA full"`) and 512 bytes a tick (`"!!Send data full!!"`).
- 24 bytes every 3 frames is far under them, and there is no per-type throttle in the client.
- The type-2 heartbeat carries heading and position, never pitch. It is what shows your walk-and-shoot moves while the
  gun is up.

Kept at 3 frames. The server side is not in the dumps; the two-player checklist watches for drops.

## 3. Seamless doors in private co-op

The 12 door-shot `jal 0x590B40` sites (in `FUN_00591160` / `FUN_005929E0`, behind "is the local player") are co-op live
with `SeamlessDoors`. The three arrival-shot sites (`FUN_00550220`, kept through disarm, not proven door-only) have their
own name, `whenSetting: SeamlessDoorsArrival`, under the same switch, and that name is never co-op live. Online the arrival
shot still plays, and DoorVeil keeps the load black, then up to 1.5 s more before the game's own picture. The door's
motion, opened bits, network notify (`0x1AF240`), room change and load are untouched.

Verified (offline), override on, slots 3 and 7: the 12 door words NOP'd while armed, the arrival words stock, camera mode
0. **Not reached:** a real door opening under the override. Slot 7's walk ran into the J's Bar cutscene, and slot 3's
script did not open the door. Slot 4, the usual door slot, was not in this install.

## 4. Co-op aim

**What the shot uses (verified (code)).**
- `FUN_0062CF10` reads only `+0xCA8`, `+0x08` and `+0xB98` from the record. The shot's pose is the equipped item's weapon
  object (item record `+0x1C`; matrix `+0x1C0`, position `+0x200`).
- This console's animation poses that object from the record's heading `+0x92`, aim pitch `+0xBC8` and `+0xCD4`.
- For a remote record, `FUN_0062D2C0` calls `FUN_0062CF10` with no ammo check (`0x62D38C`). So a room master computes a
  partner's hits from the partner's pose as received.

**What the partner packet carries (verified (code)).** Type 1, `0x2C` bytes, received by `FUN_0064D140` at `0x64D230`:

| Payload | Field |
|---|---|
| `+4`..`+7` | the action bytes `+0x08`..`+0x0B` |
| `+8`..`+0x10` | position |
| `+0x14` | heading |
| `+0x16` | HP |
| `+0x18` | `+0xBA0` |
| `+0x1C`..`+0x1F` | the equip bytes |
| `+0x20` | aim pitch `+0xBC8` |
| `+0x22` | `+0xCD4` |
| `+0x24` | room |
| `+0x25` | lock |

A type-1 message is sent on an action change that `FUN_006337C0` does not filter, and firing (stance 2, sub-action 1 to 2,
with ammo) is not filtered. Heading and pitch are read at the call (`0x64C790`, `0x64C7C8`).

**So:** steer the LOCAL record's heading and aim pitch so the game's own barrel points where the laser does, and the game's
shot here and on the master agree with the laser.

**Built** (`GunRay`, `[VR] GunRayAim` in private co-op, never online-safe, with the gun raised; CameraDriver's aim writes
the command instead of the controller's own while one is fresh, within 100 ms):
- **The aim point.** Where the VR ray passes nearest an enemy hurt sphere it comes within r + 24 cm of (the pistol's
  capsule), else 6 m along it (`AimPoint`).
- **The heading.** Solved so the barrel's line passes through the aim point (`PoseFrom`, `SolveHeading`). The line keeps a
  lateral offset from the character's centre (about -13 cm) and a yaw offset from the heading, and both are learnt only
  while heading and barrel hold still (0.6 deg a poll), because in a turn the pose lags the heading. A first try that only
  iterated on the barrel's error diverged with a zombie within a metre: the gun swings sideways as the character turns.
- **The elevation.** Measured from the shoulder pivot, 55 cm behind the weapon object along the barrel (offline, 34 deg of
  pitch moved the object 32 cm). From there the muzzle's own rise and fall drops out. A trim is learnt from the measured
  barrel on steady, consistent reads (the matrix row 3 equal to `+0x200`, within 12 deg, the gun up for 0.3 s; gain 0.12,
  clamped to 15 deg).
- **Logs.** `PCSX2_VR_GUNRAY_LOG=1` logs `Gun ray co-op:` lines: how far the game barrel's line passes from the aim point.

**Verified (offline), slot 5**, the scripted right hand replayed from an offline GunRayAim run (`gun_test.py`: `head` the
laser on the zombie's head, `up` the laser well above it, `offset` the hand 0.7 m to the side pointing in):

| Run | head | up | offset |
|---|---|---|---|
| Offline, GunRayAim (cave I), reference | hit 600 -> 150 | miss | hit |
| Override, co-op aim (twice) | hit | miss | hit |
| Override, GunRayAim off (the old online aim: heading and pitch from the controller's direction) | hit | miss | **miss** |

With the aim steady, the game barrel's line passed 1.5-6 cm from the aim point. While the hand or the zombie moves it
settles over about 0.3-0.5 s (25-60 cm off meanwhile).

Caveat: the replay depends on the case order (the camera's yaw anchor carries over between state loads). Run in another
order, the replayed laser pointed elsewhere and co-op aim followed it (correctly) to a miss.

**Not checked:** a moving target, long guns, the shotgun (a wider shape), the grenade launcher, the master's copy (needs
two machines).

## 5. Spatial audio in private co-op

The cave rewrites only the projected x and w that `FUN_001B7B50`'s pan reads for a sound this console plays, and the host
block is its own free RAM. It is co-op live now. Verified (offline), override on, slot 5: "SpatialAudio: on" and 507
positioned updates in 5 s, the same pans as offline.

## 6. Hand pickup online (left)

Not done. Cave `0xFEC00` moves the game's Circle search point (`0x61DB94`) to the hand, and a held squeeze confirms the
take-it prompt. Whether the take then goes through the game's own request path online (a request to the server or master
that decides it), rather than a local grant, was not traced: the pickup's network call after `FUN_0061DB40` was not
followed. Until that is shown, HandPickup and hand targeting stay strictly offline.

## Self-tests

65 suites, all PASS: new "Network gate" and "Co-op switches" suites, the "Seamless doors" split, and 14 co-op aim checks in
"Gun ray".

## Files

`pcsx2/VR/NetGate.{h,cpp}`, `CameraDriver.cpp`, `GunRay.{h,cpp}`, `HudCards.cpp`, `VRProfileDB.cpp`, `SpatialAudio.cpp`,
`PhysicalMelee.cpp`, `VrBody.cpp`, `VrPresence.cpp`, `bin/resources/vr-profiles/SLPM-65428.yaml`,
`tests/vr/VRSelfTests.cpp`. The test scripts (`step1.py`, `fade.py`, `door.py`, `gun_test.py` with `GT_NET` /
`GT_GUNRAY`) are in the session scratchpad, not committed.
