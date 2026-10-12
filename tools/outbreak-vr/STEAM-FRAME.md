# Outbreak VR standalone on the Steam Frame

Can first-person Outbreak run on the Frame itself, with no PC? Short answer: **probably yes, at lower image quality, and
nothing found blocks it in principle, but none of it exists yet.** This page is the plan. The research behind it (sources,
file-by-file inventory, the full M0 brief) is in `research/2026-10-11-steam-frame-research.md`.

Written by the Windows PC session on 2026-10-12 from read-only research.

**Progress (2026-10-12):** M0 and M1's code are done and CI-green on ARMSX2 branch `outbreak-vr-frame` (`22a1676`):
- the probe and the `ENABLE_OPENXR` option;
- the VR layer (no clang fixes needed) and the M0 hooks;
- the launcher and a smoke job;
- M1's first-person camera, stereo and GS hooks, with a CI hook-parity check.

Nothing has run on a Frame yet. Alex's session (ARMSX2 `frame/README-FRAME.md`) is next.

## How it would work

- **Emulator: ARMSX2** (`beadbeed/ARMSX2`, branch `outbreak-vr`, local copy `D:\dev\ARMSX2`). It is a PCSX2 fork with native
  ARM64 recompilers (EE, IOP, microVU), so it runs natively on the Frame's Snapdragon 8 Gen 3. Running the x86 PenguinScreen2
  through FEX would be far too slow for a JIT emulator.
- **What ARMSX2 already has:** most of PenguinScreen2's stereo renderer (multiview two-layer targets, layer proxies, the
  stereo vertex block) and the seven Vulkan hook sites for OpenXR. They are switched off because `ENABLE_OPENXR` is never
  defined. Its CI builds a Linux arm64 AppImage (ubuntu-24.04-arm, glibc 2.39, the same as the Frame image), and the last
  run, on 4d22c34 (2026-10-08), is green.
- **What it lacks:** the whole headset runtime: session, compositor, input, camera and the game layer. That is
  PenguinScreen2's `pcsx2/VR` (~36k lines), which was written Linux-first: no WinAPI or D3D, and about 95% copies over
  unchanged. Roughly 1,700 hook lines in the GS, VMManager, input and Qt files have to be carried over by hand. A git merge
  won't work, because PenguinScreen2 commit 990b456 stripped comments from 938 files.
- **The platform:**
  - The Frame runs SteamOS (Arch aarch64). Native arm64 Linux apps are a first-class target, loaded with the SteamOS Devkit
    Client.
  - The on-device OpenXR runtime is SteamVR's own linuxarm64 build. `XR_KHR_vulkan_enable2` is reported working on it.
  - The GPU driver is Mesa Turnip 26.3, and ARMSX2 already has driver rules for it.
  - Two community projects already run native OpenXR + Vulkan programs standalone on it: Star Fox Enhanced and dolphinXR.

## The big unknowns

1. **Performance and heat.** All of this has to fit in a roughly 10 W chip: emulation, two-eye GS rendering, frames submitted
   at 72-90 Hz, and SteamVR's compositor. Nobody has published PCSX2 or ARMSX2 running on a Frame.
   - Expect 1x-1.5x internal resolution, clearly softer than the PC's 3x.
   - Expect 72 Hz first.
   - Expect about 1-1.5 h on battery, so plug it in (45 W) for long sessions.
2. **The runtime's real behaviour on the device:**
   - which extensions it offers (the cylinder layer is probably missing, so the screen falls back to a flat quad);
   - whether Turnip has only one graphics queue, so the GS and the runtime share it (the PC build already handles that with
     the bounded queue lock);
   - known SteamVR-for-Linux bugs: #975 (quad layers ignore the array index; PenguinScreen2 already uses one image per
     swapchain), #989 (a second session on the same instance aborts; PenguinScreen2 already tears down the instance with the
     session), and #972 (the FDM injection layer can hang `vkCreateDevice`).
3. **Development hazards on the device:**
   - three or more failed SteamVR runs in one boot trigger `steam-health-check`, which wipes `~/.local/share/Steam`, so never
     script SteamVR restart loops;
   - the headset must be worn (or standby defeated) for a session to reach FOCUSED.

## Milestones

| | What | Engineer | Alex |
|---|---|---|---|
| **M0** | CI build with an OpenXR session; Outbreak on a fixed flat screen, Frame controllers as a pad; plus `frame-xrprobe`, a small program that answers every runtime question in one headset session | 4-6 days (mostly CI) | ~30 min setup + one ~45 min session |
| **M1** | First-person camera, per-eye stereo, render-pose sync | 4-6 days | 1 session |
| **M2** | Full VR controls and haptics (gesture zones, smooth turn, recenter, grab breakout) | 2-3 days | 1 session |
| **M3** | Hands, HUD, wrist card, menu pointer, belt pouches, brightness | 3-4 days | 1 session |
| **M4** | Performance and thermal tuning (refresh rate, resolution sweep, thread pinning, Valve's perf CSV) | 3-7 days, open-ended | 1-2 sessions |
| **M5** | Packaging, keeping the two copies of `pcsx2/VR` in step, optional co-op with Adrian | 3-5 days | 1 session |

Rough total: 3-5 weeks at CI pace, plus 5-6 short headset sessions. Recommendation: **do it, probe first.** One session with
`frame-xrprobe` answers the runtime questions before most of the porting is done. Keep the Windows build streamed through
SteamVR as the high-fidelity way to play until M4 shows standalone is good enough.

**M0 in one paragraph** (full brief in the research file, section 4): copy `3rdparty/openxr`, `pcsx2/VR`, `VRInputSource` and
the vr-profiles from PenguinScreen2 into ARMSX2, behind a new `ENABLE_OPENXR` option that is off by default and turned on only
by `outbreak-vr.yml`. Add the shims `VMManager::IsOnlineLocked`, `VROptions`, `EmuFolders::VRProfiles` and
`ShadeBoost(GSVector4)`, and port the M0 hooks (the queue lock, frame generation forced off in VR, `EndOfFrame` /
`EnsureFrameSubmitted` / `PresentationLift`, the VMManager settings hooks, the InputManager VR source and the `--vr` /
`-vr-info` flags). Ship an AppDir tarball with a launcher script that logs everything to `~/armsx2-vr/logs` and writes a
first-run ini (Vulkan, 1x, MTVU, VR on, head camera and stereo off). Add a `frame-smoke` CI job that runs `-vr-info` and the
probe with no runtime, and checks glibc symbol versions. Expect clang-17 fixes, because waves 4-7 have only been compiled with
MSVC; port them back to PenguinScreen2.

## What Alex would need to do (nobody else can)

- Turn on Developer Mode on the Frame (Settings > System) and set the developer password himself.
- Choose how builds get there: the SteamOS Devkit Client on the PC (paired from the Frame's Settings > Developer), or SSH with
  a key. With a key, a session could deploy builds by itself; whether to allow that is his call.
- Say yes to pushing to `beadbeed/ARMSX2` (it starts CI) and to downloading its artifacts.
- Copy the BIOS and the two discs onto the Frame himself, over the LAN, USB-C networking or microSD, into
  `~/.config/ARMSX2/bios` and a folder under `~`. Never through GitHub or a cloud service, and never into the devkit title
  folder (it gets deleted).
- Accept any SteamOS, Devkit or SteamVR prompts himself.
- Wear the headset for the probe and the M0 checklist (on a 45 W charger), and copy back the logs: `~/frame-probe/*.log`,
  `~/armsx2-vr/logs`, and `xrclient_*`, `vrcompositor` and `vrserver` from `~/.local/share/Steam/logs`.
- Decide whether co-op with Adrian on the Frame (through the Mac's Bioserver over the LAN) is in scope.

His PC graphics settings stay untouched throughout.
