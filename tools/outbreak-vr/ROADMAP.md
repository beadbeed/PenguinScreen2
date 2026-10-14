# Outbreak VR: the goal and the road to it

Alex's goal, in his words after his second headset session (2026-10-12): "Let's really make this feel like Resident Evil
Outbreak VR." Right now "it feels like a flat screen game with VR thrown on top of it. I want it to feel like an actual VR,
like it was deliberate."

**Scope: File #1 (SLPM-65428) only, until it feels right.** No File #2 parity work, and no Steam Frame standalone work (ARMSX2),
until Alex says File #1 is there. Both stay where they are: File #2 has the shared code and its verified addresses, and ARMSX2
`outbreak-vr-frame` has M0 to M3 (`STEAM-FRAME.md`).

`STATUS.md` has the current build, what to test and every switch. This page is the direction.

## Why it still feels like a flat game in a headset

1. **One picture per frame.** The game renders one view per frame from one camera (we drive it from the head). We show that
   picture on a big curved screen in front of the eyes and make the second eye from depth. Leaning moves the screen, not the
   world, and near things have no real parallax. This is the core of the "flat game" feeling.
2. **Menus pull away.** The game's menus are full-screen 2D overlays, so the build swaps to a flat panel while one is up.
   Outbreak keeps running during menus, so this is our choice and can change.
3. **Things vanish up close.** The game hides models that come too near its camera, which stops a third-person camera clipping
   into characters. In first person it hides the zombie grabbing you. The build also uses that cull to hide the player's own
   body.
4. **Doors pull away.** Opening a door yourself plays the door sequence and drops out of first person. Following a teammate
   through a door does not: you just arrive in the next room. Alex likes the follow case. Cutscenes may still drop out.
5. **Actions are mapped buttons, not physical.**
   - Melee is a button, so a swing has no feel and it is hard to tell when you are attacking.
   - The scrub broom is the wrong size in the hand.
   - Weapons are placeholder shapes, not the game's models.

## The road, in order

### 1. Menus without the pull-away (a game change; Alex approved changing how the game runs)
- **Stage 1:** first person keeps running while a screen is up. The game's 2D menu draws go to a separate layer, shown as a
  panel in front of you (or on a wrist or chest tablet). The menu pointer still works.
  - Done on branch `menupanel` (2026-10-13), behind `[VR] MenuPanel` (off): the item screen, map, take-it prompt and
    pause keep first person; the GS splits each screen's draws off the frame at its first draw and the compositor shows
    them on a panel 1.0 m ahead, where the pointer aims. Checked offline and on the null headset; next, the headset.
    Research: `research/2026-10-13-menu-panel.md`.
- **Stage 2:** a VR-native inventory, our own 3D UI on the body that reads the item records and drives the game's menu logic
  underneath.
- Design research: `research/2026-10-12-menus-in-vr-design.md` (in progress).

### 2. Nothing vanishes up close (a game patch)
- Patch the game's near-camera cull so zombies and partners always draw.
- Hide only the player's own body, directly.
- Check the camera's near plane.
- Done on branch `nearcull` (2026-10-13): the cull never culls in first person; your own draws are skipped. The near
  plane is 1 cm. Checked offline; next, the headset. Research: `research/2026-10-13-near-cull.md`.

### 3. Your own doors feel like following a teammate (a game patch)
- Send the player's own door opening down the follower path, or cut the door sequence to a short fade.
- Keep the room load and every side effect, and keep co-op identical for partners.
- Research: `research/2026-10-12-seamless-doors-research.md` (in progress).

### 4. Physical weapons
- **Models:** the game's own weapon models in the hand and the holster. They are decoded from the disc at VR start (done,
  `GameModels`) and drawn with the mesh renderer (`MeshRaster`) at their real size, placed where the game attaches them
  to the hand (done on branch `handmodels`, `[VR] GameWeaponModels`, checked offline; next, the headset;
  `research/2026-10-14-weapon-models-in-hand.md`).
- **Hand:** a proper hand model (the CC0 Godot XR Tools hand, converted: `HandMesh`; drawn on `handmodels`).
- **Melee:**
  - a physical swing attacks (swing speed and direction trigger the game's attack);
  - a buzz on contact, and a clear cue when an attack is live.
- **Reload:**
  - **Native:** lower the gun, stand still and hold the left trigger. This works today.
  - **Gesture:** a magazine grab at the left hip brought to the gun, which then holds that trigger for you (in progress,
    `ReloadGesture` plus wiring).

### 5. True per-eye rendering: the step that makes it real VR
- **First:** depth-based reprojection. Each display frame, warp the last game frame per eye to the current eye pose using
  its depth buffer. That gives real parallax when you lean and correct stereo per pixel, with no screen.
  - Foundation done on branch `depthproj` (2026-10-13): the shown frame's depth exported beside its colour in metres
    (checked offline against the J's Bar counter), and `[VR] DepthLayer` hands it to SteamVR with the frame as a
    projection layer (`research/2026-10-13-depth-export.md`). Next: lean in the headset with it on and off; then our
    own per-eye warp if SteamVR does not use the depth.
- **Then:** render the game's 3D world twice per frame, once per eye, with the headset's own projection, by replaying the
  game's own draw program (its VU1 display lists) per eye. Then re-render it at 90 Hz from the latest head pose, independent of
  the game's frame rate.
- **Long term:** a native port with its own renderer.
- Research: `research/2026-10-12-true-vr-rendering-research.md` (in progress).

### Alongside: smoothness
- SteamVR stalls (`xrEndFrame` holding a display period) after a dashboard or Steam-button visit cost the game 10 to 20
  fps for 10 to 40 s.
- Fixes:
  - a dedicated XR queue (done, opt-in `DedicatedXrQueue`);
  - pacer-owned copies, so the game thread never waits on SteamVR (in progress, branch `pacer2`, opt-in `PacerCopies`);
  - a steady layer array, so SteamVR stops re-creating shared textures.
- Research: `research/2026-10-12-steamvr-endframe-research.md`.

## Decisions on record (Alex, 2026-10-12)
- **File #1 only** until it is right (see above).
- **Changing the game is OK** where it makes the VR deliberate: patching code, menus and doors. Online rules still hold:
  - private co-op only;
  - never write another player's record;
  - never pause, change speed, or load or save a state online.
- **Reload:** a deliberate reload gesture may drive the reload for you. The native one is the left trigger with the gun
  lowered, so no menu confirm is needed. Nothing else ever confirms a menu entry automatically.
- **Weapon models:** the game's own, extracted from his disc at runtime, kept in memory only, never committed or uploaded.
- **Hands:** a free CC0 hand model is fine.
- **Long guns** sling over the right shoulder, and are drawn from there with the grip. Pistols and melee weapons go at the right
  hip.
- **Full view** (FOV fitted to the headset, edge fill) and **0.5 m depth** are the defaults now.
- **Agents:** Sonnet for most research; Opus only for groundbreaking work such as modifying the game's code.
