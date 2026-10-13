# Audit brief: Outbreak VR (PenguinScreen2)

You are auditing a project for Alex: first-person VR for Resident Evil Outbreak File #1 (PS2, SLPM-65428, English patch),
built into PenguinScreen2, a PCSX2 fork with OpenXR. It runs on Windows SteamVR with a Valve Steam Frame, seated. Your job
is a thorough, independent, read-only review: find what is wrong, risky, missing or misdocumented, with evidence. Do not fix
anything unless Alex asks you to.

## What to check out

- Repo: https://github.com/beadbeed/PenguinScreen2. Branch `headset1` is the current work; record the commit you audit
  (`git rev-parse HEAD`).
- In-flight branches that may land soon. Review them only if asked, or as a separate section:
  - `pacer2`: frame pacing, steady layers, opt-in pacer-owned copies;
  - `reloadwire`: the reload gesture driving the native L1 reload;
  - `meshraster`: a textured CPU mesh renderer.
- On hold, out of scope unless asked: File #2 (SLPM-65692) parity, and the Steam Frame standalone port (repo beadbeed/ARMSX2,
  branch `outbreak-vr-frame`).

## Read first, in this order

1. `tools/outbreak-vr/ROADMAP.md`: Alex's goal (deliberate VR, not a flat game in a headset), the decisions he made, and the
   road ahead.
2. `tools/outbreak-vr/STATUS.md`: what the build does, every `[VR]` switch, the test kit, the rules, and what is in progress.
3. `tools/outbreak-vr/README.md` (the tools) and `bin/resources/vr-profiles/README.md` (the profile format).
4. The File #1 profile: `bin/resources/vr-profiles/SLPM-65428.yaml`. Every game address and patch the VR layer uses is here,
   with comments.
5. `tools/outbreak-vr/research/*.md`: the evidence behind the addresses and designs.

## The code

**The VR layer:** `pcsx2/VR/` (about 40k lines). Key files:
- `XRSession`: the OpenXR instance and session;
- `XRCompositor`: frames, layers, the pacer thread, copies;
- `VRVulkanBridge`: the device, the queue lock, the dedicated queue;
- `CameraDriver`: the first-person camera, game patches and guards;
- `VRInput`, plus `pcsx2/Input/VRInputSource.cpp`: controllers and the virtual pad;
- `SpatialControls`: zones;
- `MenuPointer`;
- `BeltPouches`;
- `HudCards`;
- `HandModel`;
- `ZoneMarkers`;
- `ItemReach`: the pickup cue;
- `GameModels`: weapon meshes from the disc;
- `HandMesh`;
- `ReloadGesture`;
- `StereoAuto`;
- `GameFeedback`;
- `VRProfileDB`: the profile parser.

**Hooks in shared PCSX2 code,** under `ENABLE_VR`:
- `pcsx2/GS/Renderers/Vulkan/GSDeviceVK.cpp`;
- `pcsx2/GS/Renderers/HW/GSRendererHW.cpp`, plus the texture cache: stereo;
- `pcsx2/VMManager.cpp`: online locks;
- `pcsx2/Hotkeys.cpp`, `pcsx2/PINE.cpp`, `pcsx2-qt/MainWindow.cpp`;
- `pcsx2/Config.h` and `Pcsx2Config.cpp`: `VROptions`.

**Tests:**
- `tests/vr/`: VR self-tests run in CI;
- `tools/outbreak-vr/*.py`: headset-free scenario tests over PINE;
- `.github/workflows/`: the CI. Windows only; the build is MSVC C++20.

## What to audit (rank every finding: critical / major / minor)

1. **Safety rules** (these are Alex's hard rules; any way to break one is critical):
   - never pause or freeze the emulator while online;
   - never change game speed online;
   - never load or save a state online;
   - never write another player's character;
   - first person arms online only in private co-op, never on obsrv;
   - never confirm a menu entry automatically. The only exception is a deliberate reload gesture, which uses the game's own
     L1 reload;
   - never commit or upload discs, BIOS, saves, dumps or anything extracted from the disc. `GameModels` must stay in memory
     only.

   Trace every path: VMManager, Qt, hotkeys, PINE, input recording, the VR code itself.
2. **Crashes and hangs** in real use:
   - threading between the GS thread, the VR pacer, the EE/CPU thread and the input poll;
   - lock order and the 500 ms abandon path;
   - Vulkan synchronisation: the shared queue, the opt-in dedicated queue and its timeline semaphores, layout transitions,
     swapchain acquire/wait/release;
   - OpenXR session states, loss and recovery;
   - out-of-range memory reads of guest RAM: every address the profile and the code read or write.
3. **Game patches and memory writes:**
   - are the MIPS patches (profile `silence`, `hold`, cave hooks) correct, restored on disarm, and only active when they
     should be?
   - do the guards actually guard?
   - check the evidence in `research/` against the code and the profile.
4. **Input correctness:**
   - phantom presses;
   - stuck buttons after tracking or focus loss;
   - the menu pointer and pouches never confirming by themselves;
   - the reload gesture and holster zones not stealing each other's input;
   - release-first latches.
5. **Comfort and VR quality:** anything that makes it feel like "a flat game with VR thrown on top" that the roadmap misses or
   gets wrong, e.g. latency, judder, scale, stereo, FOV, the menu pull-away, near-cull, doors, hands and weapons, haptics.
   Concrete proposals welcome.
6. **Performance:** the pacer, per-frame CPU work (the HUD and hand rasterisers, GameModels at start, ItemReach every 2
   vsyncs), and the SteamVR `xrEndFrame` stall analysis in `research/2026-10-12-steamvr-endframe-research.md`.
7. **Tests and CI:** what the self-tests actually cover vs what they claim; holes; flaky harness tests (listed in STATUS.md).
8. **Docs vs reality:** does STATUS.md / ROADMAP.md / README.md / the profile README match what the code does? Wrong
   defaults, stale switches, missing settings.
9. **Code health:** dead code, duplicated logic, risky shortcuts, and anything that will make the roadmap's big steps harder:
   - menus as a VR panel;
   - true per-eye rendering by replaying the game's VU1 draw program;
   - depth reprojection.

## Rules for you

- **Read-only.** Don't push to `headset1`, `pc-firstperson`, `coop-firstperson`, `wave4-integration` or any branch. Don't open
  PRs unless Alex asks.
- **Builds:** don't run CI builds unless Alex asks. They are GitHub Actions only, there is no local compiler set up for this,
  and new branches take about an hour.
- **Emulator:** don't run it, go online, log in anywhere or touch SteamVR settings. If you are on Alex's PC and he says you
  may run the emulator: offline only, normal speed, File #1, and only the savestate slots he names.
- **Evidence:** every finding needs `file:line` (or address and value) and a concrete failure scenario. Mark anything you
  could not verify as unverified, and don't pad the report with style nits.

## Deliverable

One report, `tools/outbreak-vr/audits/<YYYY-MM-DD>-audit.md`, in plain, direct English:
- the commit audited;
- a findings table ranked by severity (id, severity, area, `file:line`, what is wrong, failure scenario, suggested fix);
- the top 5 things to do next and why;
- what you could not check.

If you are on Alex's PC, write the file there and don't commit it. If you are elsewhere, give it to Alex, who will bring it
over.
