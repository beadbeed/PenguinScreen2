# Your own arms anchored to the headset: `[VR] VrBodySelfStyle` (2026-10-27)

Branch `bodyanchor` (from `nightly` 3df2946). Evidence words as elsewhere: **written**, **built**, **self-tests passed**,
**checked offline** (one headset-free emulator, File #1, scripted PINE head and hands, no OpenXR runtime, offline,
normal speed). Nothing here has been in the headset.

Alex, 2026-10-26, with `VrBody` and `VrBodySelf` on: he "felt like I was in Kevin's chest". `VrBodySelf` drew Kevin's own
arms, posed by IK from the game's own animated shoulders (`research/2026-10-21-vr-body.md`).

## 1. Measured first (checked offline, slot 7, `bodyanchor_test.py`, the GameShoulders behaviour of `nightly`)

The scripted head at 1.1 m, the view recentred on it, six seated hand poses; PINE reads of the game's camera eye
(`0x306338`) and Kevin's bone world matrices (record `0x476DD0`, 36 bones). In the view's level frame, from the eye:

| what | up | forward | right |
|---|---|---|---|
| head bone 21 | +0.2 to +1.8 cm | **+5.1 to +6.4 cm** | -2 cm |
| neck 20 | -5 to -7 | +1.6 to +3.1 | -2 |
| chest 11 | -26 | -7 | 0 |
| left shoulder joint 13 | **-11 to -13** | -7 to -9 | -16 to -18 |
| right shoulder joint 17 | **-13.5 to -15** | **+3.6 to +6.2** | +15 to +16 |

- The eye sits **155.0 cm over the character in every sample, whatever the seated height**: the recentre maps the head to
  `eyeHeight`. So the brief's guess (the eye at the seated height, under a standing Kevin's shoulders) is **refuted** as
  stated; the eye is at a standing Kevin's head.
- But the game's skeleton does not fit a person under that eye: the head bone is 5-6 cm **ahead** of the eye (the eye is
  in the back of Kevin's skull), the shoulder joints are only 11-15 cm under it (a real one is about 20-25 cm under and
  8-10 cm behind), and the gun stance turns the chest so the right shoulder is 4-6 cm **in front of** the eye.
- So the drawn upper arms (shoulder to elbow, a 6 cm sleeve round the bone) reach up to 5-9 cm under the eye and within
  15-17 cm of it; with the right hand raised the sleeve's top is level with the eye (-0.4 cm). The arms come out of a
  body round your neck: "in Kevin's chest" is that, **confirmed** as a mismatch of the skeleton to the eye, not of height.
- The hands themselves were on target (the game-built wrists 0.5 cm off).

## 2. The fix (written, built)

`[VR] VrBodySelfStyle` (ini only), with `VrBody` and `VrBodySelf`:

- **`Forearms` (default).** The arms are anchored to the headset, not to the game's skeleton, and only the game's own
  forearms and hands are drawn.
- **`Arms`.** The same anchoring; the upper arms are drawn too.
- **`GameShoulders`.** The first version, unchanged (cave U's locals offline, from the animated shoulders).

The anchoring (`VR::VrBody::AnchorShoulders`, pure, VR space):
- **Neck model**: the neck's pivot 7.5 cm under and 8 cm behind the eye in the head's frame (it stays put as the head
  nods and turns about it); the shoulders' midpoint 14 cm under the pivot and 1 cm behind it, upright; the model's own
  half width each side (Kevin 18.5 cm). So the shoulders sit 21.5 cm under and 9 cm behind the eye (Kevin's own model has
  19 / 10 between its eyes and its shoulders).
- **Body yaw**: the head's, 70% of the way toward the tracked hands' midpoint (seen from the pivot, when that is 15 cm out
  and not behind), at most 60 deg off the head, eased at 8 per second.
- **Seated calibration** (`CalibrateScale`): at VR start and on every recentre (the camera mapping's reference changing)
  the eye's height over the floor (the runtime's STAGE space, located against the VIEW space each input poll; with
  scripted input the scripted head's own y) scales the neck model by (eye - 0.45 m seat) / 0.75, clamped 0.85-1.15; an
  unknown floor or an eye outside 0.9-1.5 m gives 1. Logs `(VR) Body: self calibrated at the recentre: ...`.
- **As the eye sees it**: every VR point goes through `GameFromVr` plus `eye_forward` (10 cm) along the view's yaw, the
  same shift the game's own eye has, so the drawn hand lands on the controller the eye sees, not 10 cm short.
- **IK** (`SolveAnchored`): the existing two-bone IK and hand solve (now `SolveArmChain`, shared with the partner's) from
  the anchored shoulder to the controller's wrist point; a target out of reach first slides the shoulder up to 8 cm toward
  it (a real shoulder reaches forward too), then the arm stops short and straight. The bones keep their lengths: the arms
  never stretch.
- **Hide plan** (`SelfHidePlan`, by bone, from the game's own skeleton): the drawn bones first (per arm the forearm, the
  wrist and the hand's four; with `Arms` the upper arm too), then bones given zero axes at one point so their vertices
  vanish there: with `Forearms` each upper arm at its elbow; the clavicles at the chest's joint (`Arms`: at their
  shoulder); the chest and the neck's subtree (head, face) at the chest's joint. Kevin: 24 entries, the table's limit; the
  jaw (27) falls on its own joint, every bone round it on one point, so no triangle opens. The waist, hips and legs keep
  cave V's own collapse (each on its joint, as before). An untracked hand hides its whole arm. Nothing else changes:
  the near-cull and every other character draw as before (zombies close to you too).
- **Draw-only**: the anchored slot is cave V's world entries (flags self + collapse, never flag 8): the game's own arms,
  its held items and its hurt spheres stay its animation's, and the hit tests are untouched. The reference bone is the
  root (bone 0: the character's place and heading, none of the chest's breathing and stance sway): the host converts
  everything into the root's frame, the cave multiplies by the root's world of the frame it draws. No new cave words, no
  new free RAM: the same caves, block (0xFF300) and tables (0xFD400-0xFDFFF). Offline only, as `VrBodySelf` was.

## 3. Checked offline (2026-10-27, run-body2, local non-LTO build, PINE slot 28160)

`bodyanchor_test.py <install> 7 --style Forearms` (each height from a fresh load of slot 7: its own cut takes first
person away about 20 s in), the eye at 1.1, 1.2 and 1.3 m, six seated poses each (aim forward, hands low near the lap,
reaching far forward, right raised and left low, out to the sides, aim forward looking down 50 deg). From the emulog's
`(VR) Body: self ...` lines and PINE:

- `(VR) CameraDriver: cave 0xFD000-0xFD374 in (221 words, read back)`, `(VR) Body: live`, then per load `(VR) Body: self
  calibrated at the recentre: eye 1.20 m over the floor, body scale 1.00` (1.10 m gave 0.87, 1.29 m 1.13) and `self posed
  (record 00476DD0)`, 24 entries.
- **The forearms meet the controllers**: in all 36 arm samples (18 poses) the wrist bone **in memory** (the world matrix cave
  V wrote at the draw) was 0.0 cm from the controller's wrist point as the eye sees it, except reaching far forward at
  1.3 m (out of reach after the 8 cm shoulder slide): 0.3 cm short. The elbows in memory 0.0 cm from the solve.
- **No stretch**: the drawn forearm 25.15 and the upper arm 26.09 model units in every sample (Kevin's own 25.14 / 26.08).
- **The eye**: 155.0 cm over the character in every sample; the anchored shoulders 18.6 / 21.5 / 24.3 cm under it and
  7.8 / 9.0 / 10.2 cm behind it at 1.1 / 1.2 / 1.3 m; the nearest drawn bone (a forearm) 32.7 cm from the eye at the
  closest (hands far forward; the sleeve's surface some 27 cm). With `Forearms` no shoulder or upper arm is drawn at all:
  PINE reads Kevin's head 21, neck 20 and chest 11 all at the chest's joint (26 cm under the eye) and each upper arm's
  joint on its elbow, i.e. their vertices vanish there.
- **`Arms`** (slot 7, 1.2 m): the wrists 0.0 cm, the drawn shoulders 21.3-21.5 cm under the eye (12.7 cm with the scripted
  nod, which turns about the eye: section 5), the nearest drawn bone 25-30 cm from the eye.
- **Slot 5** (the zombie that grabs at 5-7 s; `Forearms`, 1.2 m): the wrists 0.0 cm in both samples. Other characters are
  untouched by construction (cave V's SELF only draws the slot naming the focus record; the near-cull entries are as
  before), but **not checked on screen**: the emulator window captured black in VR mode without a runtime, so no screen
  measurement was possible.
- Normal speed throughout (the OSD read 100%); one emulator, its own PINE slot, killed by its own handle.

## 4. Self-tests (`VR body`, 59 checks; 65 of 65 suites)

The calibration (1.1 / 1.2 / 1.3 m give 0.87 / 1 / 1.13; no floor, a head-level origin or a standing eye give 1); the
neck model (shoulders 21.5 cm under and 9 cm behind, the half width each side; a nod of 50 down or 30 up and a turn of 35
about the pivot keep it and the shoulders); the body's yaw (70% toward hands ahead and to the left; the head 90 left with
the hands ahead stays between, within 60 deg; the easing; a NaN refused); the hide plan on Kevin (Forearms and Arms, 24
entries each, exactly which bone goes where) and the woman's 43 (the table full, only the head's last bones left out);
the end-to-end solve from the headset on Kevin's bind pose at 1.1-1.3 m, six poses, both styles: every wrist on its
controller within 0.5 cm, the forearm and upper arm their lengths to 1e-6, the drawn shoulders' sleeve over 5 cm under
the eye and every drawn bone's sleeve over 8 cm from it; out of reach (the shoulder slides exactly the shortfall, then
its 8 and stops short, the forearm its length); an untracked hand's arm hidden at the chest; the upper arm's vertices at
the elbow and the clavicle's at the chest; the anchored slot's flags (self and collapse, never locals). The refactor of
the arm solve into `SolveArmChain` keeps every earlier check.

## 5. Not checked, and risks

- **The headset.** How it looks and feels: whether 21.5 / 9 cm and the 70% body turn suit Alex, the hand's angle on the
  controller, the elbow's direction. The tunables are `VR::VrBody::AnchorTuning`.
- **The calibration on SteamVR**: STAGE is located each input poll; if SteamVR's STAGE is not the floor in his seated
  setup, the scale stays near 1 (the log line says what it saw).
- **Lag**: the host writes at the vsync from the last tick's root; while the character turns (first person turns its
  heading with the head) the drawn arms swing with the turn for one tick (about 3 cm at 100 deg/s at arm's length);
  standing still they are exact (0.0 cm in memory, above). GameShoulders has the same coupling.
- **The scripted nod** turns the head about the eye, not the neck, so the "looking down" samples move the pivot (the
  shoulders 11 cm under the eye there); a real nod turns about the neck (the self-test covers that case).
- `Arms`: where the upper arm meets the hidden chest, triangles weighted to both close onto the shoulder joint and the
  chest's; a thin flap may show. That is why `Forearms` is the default.
