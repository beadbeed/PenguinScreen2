# Grabbed, in first person (File #1, 2026-10-21)

Alex, 2026-10-15: "getting grabbed by a zombie and shaking them off could use a little work, because you still go to the
little cut-out screen". Branch `grabdoor`. Labels: **live** = seen offline headset-free in the build (run-grab, no OpenXR
runtime, scripted PINE head and controllers, normal speed, nothing written to the game by the tools); **assumed** = not
checked.

## What the game does while you are grabbed (live)

Slot 5, a zombie grabs Kevin about 5.5 s after the load (stance `+0x08` = 5, action 176, status `+0xBA0` 0):
- **The game's camera does nothing of its own.** With first person handed back (the old behaviour), the eye and target at
  `0x306338` / `0x306344` stayed exactly where first person last wrote them (yaw -71.3 deg, the eye at 155 up) for the
  whole grab: no grab shot, camera mode `0x3AEF75` 0 (one flicker to 3, the usual one). So the "cut-out screen" was a
  frozen copy of the last first-person view on the flat world screen while Kevin moved under it: his position moved by
  up to 38 units from that eye.
- **The grab moves Kevin:** his position `+0x38` wobbles 10-30 units with the bites and his heading turns about 22 deg
  (288.7 to 311.1).
- **The zombie grabs from behind:** the nearest enemy (table `0x3FFCA0`, its f32 position at `+0x38`) sat 32-35 units
  from Kevin at a bearing of 131 deg, Kevin's heading 311: straight behind him.

## What first person does now (`[VR] GrabFirstPerson`, on)

While the profile's `breakout.grabbed` checks hold (grabbed, and still in normal play; never down, dead or turned):
- the guards ignore `lookAt.releaseWhen`, so first person stays armed;
- the camera is written from the head **camera only** (as under a MenuPanel screen): no heading, aim, walk-and-shoot or
  body-follow writes. The eye follows Kevin's position as ever (so it follows the grab's push), the stick still turns the
  view (only the camera's own yaw anchor);
- `LocalRecord` is not handed out, so melee, the reload driver, the pickup cue and the wrist card stand down exactly as they
  did when a grab disarmed; the hurt pulses and the heartbeat (`LocalRecordAny`) and the breakout (shake to break free,
  its buzzes) are unchanged; the sprint latch lets go;
- down, dead or turned: `releaseWhen` still hands the camera back to the flat screen, as before.

Log lines: `grabbed; first person holds, camera only ([VR] GrabFirstPerson)` and `no longer grabbed; first person as usual`.

## Checked (live), slot 5, the scripted right controller shaken from the grab's start

| | GrabFirstPerson off | on | on + GrabFacePull |
|---|---|---|---|
| grab held (shaking) | 3.0 s | 3.0 s | 3.0 s |
| HP lost | 450 | 350 | 400 |
| `DISARMED` lines | 1 (releaseWhen 1) | **0** | **0** |
| eye from Kevin's head while grabbed | up to 38 units (frozen camera) | **at most 12** (10 is the eyeForward) | at most 11 |
| camera yaw while grabbed | -71.3 (frozen) | -71.3 (the head did not turn) | -71.6 to -116.7 (the pull) |
| after the grab | stick walks him (17 units in 1.5 s, the recovery) | the same (17) | 32 |

The breakout is as fast with first person held (the presses are the same pad input). HP lost varies with when the bites
land. Not checked here: `grabtest.py idle-shake` (nothing in this change touches the breakout or its gate).

## `[VR] GrabFacePull` (off)

Eases the view's yaw anchor toward the nearest enemy within `breakout.facePull.within` (120 units) while grabbed: at most
20 deg/s, 1.5 x the error below that, nothing within 3 deg, the comfort vignette fed while it turns (pure self-test:
`CameraDriver::SelfTestGrabDoor`). Toward the zombie, not Kevin's heading: from behind, the heading faces away from it.
Live it turned the view 45 deg in the 3 s grab and the zombie (158 deg behind) was still not in front when the grab
ended. That is a large artificial turn the head did not make, so it stays **off**: a comfort risk for a small gain.
Turn your head (or the stick) to look at it instead.

## Still for the headset

- How the bites look from inside Kevin's head (the zombie stands 32-35 units behind him; the near-cull patch, on while armed, keeps it drawn).
- Whether the grab's push of the eye (10-30 units) is comfortable; if not, a low-pass on the eye while grabbed.
- Whether the pull is wanted at all (try `GrabFacePull = true`).
