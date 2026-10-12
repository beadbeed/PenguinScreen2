# Steam Frame standalone: research notes (2026-10-11 night)

Raw findings behind `tools/outbreak-vr/STEAM-FRAME.md`, from four read-only research passes by the Windows PC session:
an inventory of PenguinScreen2's VR layer, a comparison with ARMSX2's `outbreak-vr` branch, a survey of the Steam Frame as a
platform (web sources, listed inline), and the milestone plan. Nothing was built or run for this; the device facts come
from Valve's documentation and community reports, not from Alex's headset.

## 1. PenguinScreen2's VR layer: what has to move

PenguinScreen2 VR layer inventory for a Steam Frame (Linux aarch64) port. Read-only analysis of D:\dev\PenguinScreen2 (branch pc-firstperson, HEAD 97bd99c), compared against D:\dev\ARMSX2 (branch outbreak-vr).

## Headline

The VR layer was written on Linux first. Upstream PenguinScreen2 shipped a Linux x86_64 flatpak for WiVRn/Monado. The Windows port was one small commit, 7679470 "Build on Windows x64 for SteamVR":
- it changed 14 lines in pcsx2/VR/SeatSession.cpp, 4 lines in XRSession.cpp and 7 lines in pcsx2/CMakeLists.txt;
- everything else in that commit was Windows CI.

There is no WinAPI, D3D or registry code in pcsx2/VR. The real porting work is:
1. Re-applying the GS, VMManager and Input hooks onto ARMSX2's slightly different tree.
2. Checking what the Frame's own OpenXR runtime supports.

## 1. pcsx2/VR: every file (bytes, lines, role)

**Session, compositor and Vulkan glue (OpenXR plus GS Vulkan internals)**
- XRSession.cpp/.h (20,662 / 1,997 B; 659 / 65 lines)
  - Creates the XrInstance, system and session through XR_KHR_vulkan_enable2. The runtime itself creates the GS VkInstance and VkDevice (xrCreateVulkanInstanceKHR / xrCreateVulkanDeviceKHR).
  - Optional extensions: XR_KHR_composition_layer_cylinder and XR_VALVE_frame_controller_interaction.
  - Also handles event pumping, loss handling and ForgetWithoutDestroying.
  - Seat binding swaps XDG_RUNTIME_DIR; this is the only `#ifndef _WIN32` code in the file.
- XRCompositor.cpp/.h (243,973 / 3,164 B; 5,930 / 80 lines). The largest file.
  - Runs the VR pacer thread (xrWaitFrame/Begin/End at headset rate) and manages swapchains.
  - Copies the GS frame into swapchains with vkCmdCopyImage from GSTextureVK on the shared GS queue, using its own command pool and fences.
  - Places the world, first-person and menu screens (cylinder or quad layers) and the opaque background.
  - Owns the lever, hand, HUD, laser, vignette and menu-pointer quad layers (staging uploads), head and eye location, reprojection to the render pose, frame stats and fault injection.
  - Swapchain format: R8G8B8A8_SRGB, falling back to UNORM. At most 16 layers.
- VRVulkanBridge.cpp/.h (6,890 / 2,383 B): the bootstrap hooks GSDeviceVK calls, plus ScopedXrQueueLock, a bounded 500 ms timed_mutex around the VkQueue that the session and the GS share.
- VRManager.cpp/.h (38,752 / 1,598 B)
  - Arming: --vr, seats, the stereo gate, settings application and the -vr-info runtime report.
  - ApplySceneStereo, which publishes StereoState to the GS thread through MTGS::RunOnGSThread.
  - The PresentationLift brightness and EndOfFrame / EnsureFrameSubmitted, called from GSRenderer.
- VRInternal.h (641 B): shared Vulkan handles.
- StereoDump.cpp/.h (12,854 / 835 B): headset-free stereo PNG capture. It uses GSDeviceVK's VMA allocator and command buffer.
- SeatSession.cpp/.h (27,932 / 222 B)
  - Linux only: extra headsets over WiVRn per-seat sockets, using dlopen of the runtime and libvulkan, with its own XR and Vulkan instances.
  - On _WIN32 the whole file is a stub.
  - Built with -fexceptions.
- SeatCast.cpp/.h (1,317 / 553 B): frame handoff to a second seat.

**Input and controls (OpenXR actions; otherwise pure)**
- VRInput.cpp/.h (58,510 / 6,651 B)
  - Action set, interaction-profile bindings (oculus/touch, khr/simple, valve/frame_controller), aim, grip and view spaces, haptics.
  - Watchdogs for focus and tracking.
- VRInputState.h (3,576 B): the controller snapshot struct.
- SpatialControls.cpp/.h (73,210 / 21,498 B): gesture zones and the VR Gamepad mapping (also levers, throttles, wheel and shifter). Pure maths.
- GrabBreakout.cpp/.h (9,104 / 4,449 B): shake-to-break-free detector. Pure.
- MenuPointer.cpp/.h (87,777 / 14,419 B): point-to-select menus. Reads the game's cursor struct and steers it with D-pad presses.
- BeltPouches.cpp/.h (47,710 / 11,891 B): body pouches mapped to item cells.
- PadLook.cpp/.h (10,051 / 564 B): measurement probe and recenter chord (hooked from PadDualshock2).
- ControlQuads.cpp/.h (11,954 / 1,421 B): lever card state.

**Game logic (guest memory plus profile; no XR or GS types)**
- CameraDriver.cpp/.h (126,233 / 5,883 B)
  - The first-person camera.lookAt: writes the game camera and aim pitch, snap and smooth turn, prediction, disarm guards, net-mode resolution.
  - "Silence" patches and MIPS code-cave hooks are written into EE code with memWrite32 every vsync.
- GameFeedback.cpp/.h (51,836 / 4,452 B): haptics from game memory (shots, hits, heartbeat), rumble forwarding, the weapon reader.
- HudCards.cpp/.h (70,899 / 16,336 B): CPU-rasterised cards (toast, wrist card, blink, laser, vignette, pointer) with a built-in bitmap font.
- HandModel.cpp/.h (84,245 / 12,095 B): CPU-rendered low-poly hand and weapon impostors, 256x256.
- ItemData.cpp/.h (12,747 / 1,969 B): Outbreak item table (MIT data).
- HeadPose.cpp/.h (904 / 1,244 B): pose publish.
- SplitState.cpp/.h (4,340 / 647 B): split-screen state.
- VRProfileDB.cpp/.h (173,007 / 26,412 B)
  - YAML profile parser and validator (ryml through common/YAML.h).
  - Profiles come from EmuFolders::Resources/vr-profiles and EmuFolders::VRProfiles.

**Stereo maths and measurement**
- StereoState.cpp/.h (2,181 / 2,840 B): the GS-thread stereo params. ARMSX2 has a 71-line subset of this file.
- StereoAuto.cpp/.h (11,503 / 4,670 B): IPD-derived first-person separation and convergence. Pure.
- DepthHistogram.cpp/.h (16,765 / 2,800 B): depth census (PINE/qhist).
- MemWatch.cpp/.h (3,547 / 2,857 B): EE memory watch over PINE. It relies on the x86 recompiler's memcheck hook.

**Other**
- LICENSE.note (1,653 B).

**Totals:** 35,624 lines, about 1.37 MB. Outside pcsx2/VR there are also pcsx2/Input/VRInputSource.cpp/.h (1,185 lines) and pcsx2-qt/Settings/VRSettingsWidget.cpp/.h/.ui (870 lines).

## 2. Files outside pcsx2/VR that the VR layer touches

ENABLE_VR-guarded lines per file are in brackets.

**GS (the tightest coupling)**
- pcsx2/GS/Renderers/Vulkan/GSDeviceVK.cpp [68 guarded; about 700 unguarded multiview lines]
  - Bootstrap hooks: VR::BeginVulkanBootstrap in Create (~2974), CreateVulkanInstanceThroughXR in CreateVulkanInstance (~136), the physical-device override with GetXrVulkanPhysicalDevice (~3035), CreateVulkanDeviceThroughXR in CreateDevice (~666), OnVulkanDeviceCreated (~3085) and OnGSDeviceDestroyed in Destroy (~2614).
  - ScopedXrQueueLock is taken in WaitForGPUIdle (vkDeviceWaitIdle), SubmitCommandBuffer (submit through present, ~1325) and SubmitSpinCommand (~2021).
  - Unguarded stereo engine: the vk_khr_multiview feature, GetTFXMultiviewRenderPass, the VS_MULTIVIEW and PS_TEX/RT/DEPTH_IN_ARRAY shader variants, layer-aware copies, DrawMultiStretchRects and RunMultiviewSelfTest.
- pcsx2/GS/Renderers/Vulkan/GSTextureVK.cpp/.h: 2D_ARRAY textures, per-layer views, layer proxy textures, layout tracking through proxies.
- pcsx2/GS/Renderers/Common/GSDevice.h/.cpp and GSTexture.h
  - A `layers` parameter on CreateSurface, FetchSurface, CreateRenderTarget, CreateDepthStencil and ResizeRenderTarget.
  - GetArrayLayers and GetLayerProxyTexture, SupportsStereoTargets, a ShadeBoost(const GSVector4&) overload.
  - cb_vs vr_stereo, vr_map_mode, vr_band_count, vr_splits and vr_band[4], plus the multiview selector bit.
- pcsx2/GS/Renderers/HW/GSRendererHW.cpp [583 guarded]
  - Per-draw stereo constants from StereoState (~5877–6187): uniform-Q pinning, mono centre, band and log maps, collimation, first-person near floor, roll parallax through HeadRoll, the depth-histogram census.
  - A Z/Q depth fit at VSync, stereo promotion at the display-chain feed edge (~8911), and snapshot layer selection.
- pcsx2/GS/Renderers/HW/GSTextureCache.cpp/.h [21 guarded, plus PromoteToStereo at ~7301 and the m_vr_display_bps set]: 2-layer display-chain targets.
- pcsx2/GS/Renderers/Common/GSRenderer.cpp [35]
  - The VR::PresentationLift → ShadeBoost(lift) call.
  - VR::EndOfFrame after the present and in the skip path (EnsureFrameSubmitted).
  - StereoDump::MaybeCapture and the snapshot layer.
- bin/resources/shaders/vulkan/tfx.glsl: GL_EXT_multiview, the per-eye vertex offset (vr_stereo_disp, collimation, roll) and sampler2DArray reads. dx11/tfx.fx, opengl/tfx_vgs.glsl and Metal/GSMTLSharedHeader.h only mirror the constant-buffer layout.

**Core**
- pcsx2/VMManager.cpp [50]
  - VR::UpdateSettings in CPUThreadInitialize, UpdateDiscDetails and CheckForConfigChanges.
  - ClearSpatialControls in ClearDiscDetails; CameraDriver and GameFeedback OnStateLoaded in DoLoadState.
  - In VSyncOnCPUThread: CameraDriver::Apply, GameFeedback::Poll, HudCards::Poll, ApplySceneStereo and SplitState::Apply.
  - VMManager::IsOnlineLocked (co-op online locks) is **not** ENABLE_VR-guarded and does not exist in ARMSX2. VR code calls it from MenuPointer, VRManager and XRCompositor.
- pcsx2/Config.h and pcsx2/Pcsx2Config.cpp: Pcsx2Config::VROptions (about 35 fields, the [VR] ini section), its LoadSave, and EmuFolders::VRProfiles ("vrprofiles").
- pcsx2/x86/ix86-32/iR5900.cpp [7]: VR::MemWatchOnHit inside the x86 EE recompiler's memcheck handler. This is the only x86-only hook.
- pcsx2/PINE.cpp [449]: MsgVRQhistFlush, MemWatch, MemWatchPoll, FakeInput, Telemetry and MenuPointer (0xE0–0xE5), and PINE online refusals.
- pcsx2/Hotkeys.cpp [42]: VRRecenterHead and VR brightness. pcsx2/SIO/Pad/PadDualshock2.cpp [43]: PadLook probes, recenter chord, Rx handling.

**Input**
- pcsx2/Input/InputManager.cpp/.h [105]: InputSourceType::VR, the source registration, a VR binding overlay merged into binding parsing, and rumble hooks (SetPadVibrationIntensity → GameFeedback::OnPadRumble, PauseVibration → PauseRumble).
- pcsx2/Input/VRInputSource.cpp/.h: an InputSource subclass with ParseKeyString, ConvertKeyToString/Icon, motors and the guest-reader guards.

**Frontends**
- pcsx2-qt/QtHost.cpp [87]
  - The --vr, --vr-seat, --vr-cast, -vr-info and qhist arguments. No environment variable arms VR; only --vr does.
  - A VR profile validation dialog at launch, and no pause on a VR controller disconnect while OnlineSafe is on.
- pcsx2-qt/Settings/SettingsWindow.cpp [10] adds the VR page; VRSettingsWidget.* implements it.
- pcsx2-gsrunner/Main.cpp [138]: stereo replay harness, -vr and -qhist.

**Upstream APIs relied on** (all present in ARMSX2, which merged upstream through 2026-09-25)
- memRead8/16/32 and memWrite8/16/32, eeMem->Main
- VMManager::GetDiscCRC/GetDiscSerial/HasValidVM/GetState/ReloadInputBindings
- MTGS::RunOnGSThread/IsOpen
- Host::AddKeyedOSDMessage/GetStringSettingValue
- InputManager::OnInputDeviceConnected/Disconnected/InvokeEvents/HasHook
- Pad::GetPad/GetControllerInfo, USB::GetConfigSection/SubKey
- CBreakPoints memchecks
- GSDeviceVK::GetInstance()->ExecuteCommandBuffer and GetAllocator, GSTextureVK image and layout API, the VKLoader function pointers

ARMSX2 differences found:
- No IsOnlineLocked.
- Its ShadeBoost has a different signature (adds an output_size parameter).

## 3. Windows-only and platform-specific pieces

- **WinAPI and D3D in the VR layer:** none. The only `#ifdef _WIN32` sites are the SeatSession whole-file stub and the XRSession setenv guards. Some user-facing strings mention SteamVR ("Recentered (SteamVR)", "check SteamVR"); that is cosmetic only.
- **OpenXR loader packaging:** none. 3rdparty/openxr is the vendored Khronos OpenXR SDK 1.1.61 (5.7 MB, pre-generated dispatch tables, src/loader, src/external jsoncpp).
  - cmake/SearchForStuff.cmake builds it as a static loader (DYNAMIC_LOADER OFF) with -fexceptions -frtti on the openxr_loader target. No openxr_loader.dll is shipped.
  - The same setup builds on Linux aarch64.
- **steamxr JSON handling:** none in C++. The runtime is found by standard loader discovery: XR_RUNTIME_JSON, or on Linux $XDG_CONFIG_HOME and /etc/xdg/openxr/1/active_runtime(.<arch>).json.
  - steamxr_win64.json appears only in D:\dev\PenguinScreen2\tools\outbreak-vr\launch_vr.ps1, which sets a per-process XR_RUNTIME_JSON.
  - VRManager::ResolveSeatRuntimeJson reads XR_RUNTIME_JSON only for seats.
- **Windows-only build:** .github/workflows/build-windows.yml (it triggers only on windows-steamvr and coop-firstperson, not pc-firstperson), windows_build_qt.yml, scripts/windows/*.bat and the ffmpeg patches, and the MSVC /EHsc branch in pcsx2/CMakeLists.txt.
- **Windows-only tools:** tools/outbreak-vr/*.ps1 (launch_vr, launch_fakevr, install_build, mute).
  - pine.py assumes TCP 127.0.0.1:28011, but PINE on Linux uses a Unix socket.
  - The Python harnesses are otherwise portable.
- **Linux x86_64-only, from the PC/Deck WiVRn era (not needed on the Frame):** launch-vr-session.sh (hard-coded x86_64 NVIDIA ICD path), enable-vr-discovery.sh, the flatpak/ manifest, and SeatSession's WiVRn multi-seat support.
- **x86-only:** the iR5900.cpp memcheck → MemWatch hook. ARMSX2's ARM64 EE recompiler (pcsx2/arm64/iR5900-arm64.cpp) has no memcheck support, so MemWatch would compile but stay inert. No SSE intrinsics in the VR code; GSVector4/4i usage is minor and ARMSX2 has NEON GSVector.

## 4. What the VR layer needs from the build

- ENABLE_VR in PCSX2_DEFS (cmake/BuildParameters.cmake: an option, on by default, off on Apple).
- USE_VULKAN/ENABLE_VULKAN: VR is Vulkan-only and XR_USE_GRAPHICS_API_VULKAN is behind ENABLE_VULKAN.
- 3rdparty/openxr plus the SearchForStuff block, and `target_link_libraries(PCSX2_FLAGS INTERFACE openxr_loader)`.
- An exception island: VR/XRSession.cpp and VR/SeatSession.cpp built with -fexceptions and SKIP_PRECOMPILE_HEADERS.
- ryml through common/YAML.h. ARMSX2 vendors rapidyaml in-tree.
- fmt, libpng (common/Image, for StereoDump), VMA (from the GS), dl and pthread.
- C++20: span, bit, std::bit_cast, regex, timed_mutex, thread_local.
- Qt6 Widgets plus the .ui for VRSettingsWidget (pcsx2-qt/CMakeLists.txt adds it only under ENABLE_VR).
- Resources: bin/resources/vr-profiles (5 YAML profiles plus README and LICENSE).
- ARMSX2 has no 3rdparty/openxr. Its flag is ENABLE_VR_STEREO, and its [VR] ini keys (StereoEnable, Separation, Convergence, SBS/CrossEye, PinUniformQ, ...) differ from PenguinScreen2's VROptions keys.

## 5. What ARMSX2 already has

ARMSX2's commits ec8b251803, e9ca441d9c and 4f4bb5be20 (about 2,300 added lines) already port most of the GS stereo engine:
- the multiview render passes, 2-layer targets and layer proxies
- PromoteToStereo and IsDisplayChainBP
- tex_in_array and rt_in_array
- the tfx stereo bands, collimation and pin_uniform_q

Still missing from ARMSX2:
- the first-person StereoState fields (first_person, near_floor, fp_*)
- RollParallax/HeadRoll and the DepthHistogram / Z-Q fit
- ScopedXrQueueLock and all the XR bootstrap hooks
- every other file in pcsx2/VR

## 6. Portable fraction (estimate)

Of the 35,624 lines in pcsx2/VR:
- **About 68% (24,139 lines)** is pure game, profile, controls and rasteriser logic that uses only core APIs ARMSX2 shares. It should compile unchanged.
- **About 28% (9,935 lines)** is the OpenXR session, input, compositor and bridge. It is portable C++ from Linux-first code and also compiles unchanged against ARMSX2's GSTextureVK, but it needs on-device validation against the Frame's runtime.
- **About 1% (425 lines)** is StereoDump/VRInternal, which ties into GS internals but is otherwise portable.
- **About 2.4% (843 lines)** is SeatSession: compiles on Linux, but should be stubbed (single headset).
- **About 0.8% (282 lines)** is MemWatch: compiles, but is inert without an ARM64 recompiler memcheck.

Adding the hooks:
- about 1,700 guarded lines across 15 files, to be re-applied by hand to ARMSX2's GSDeviceVK, GSRendererHW, GSTextureCache, GSRenderer, VMManager, InputManager, PINE, Pad, Hotkeys and QtHost;
- about 2,500 unguarded stereo-engine lines, of which roughly 85–90% are already in ARMSX2;
- about 200 config lines plus IsOnlineLocked;
- 870 lines of Qt.

**Overall:** about 95% of VR-layer code moves over as-is, about 4% is mechanical hook re-application against the same upstream APIs, and about 1% is stub-or-drop (SeatSession, the x86 MemWatch hook, Windows CI and the PowerShell tools). Effectively 0% is Windows-specific. Feasibility depends on the Frame's OpenXR runtime and on Adreno and JIT performance, not on code portability.

Key paths:
- D:\dev\PenguinScreen2\pcsx2\VR\XRSession.cpp, XRCompositor.cpp, VRVulkanBridge.cpp, VRManager.cpp, CameraDriver.cpp
- D:\dev\PenguinScreen2\pcsx2\GS\Renderers\Vulkan\GSDeviceVK.cpp, pcsx2\GS\Renderers\HW\GSRendererHW.cpp, pcsx2\GS\Renderers\HW\GSTextureCache.cpp, pcsx2\GS\Renderers\Common\GSRenderer.cpp
- D:\dev\PenguinScreen2\pcsx2\VMManager.cpp, pcsx2\Input\InputManager.cpp, pcsx2\Input\VRInputSource.cpp
- D:\dev\PenguinScreen2\pcsx2\CMakeLists.txt (lines 1063–1203 and 1539–1555), cmake\SearchForStuff.cmake (lines 113–128), cmake\BuildParameters.cmake (lines 26–28 and 242)
- D:\dev\PenguinScreen2\3rdparty\openxr
- D:\dev\ARMSX2\pcsx2\VR\StereoState.h, D:\dev\ARMSX2\.github\workflows\outbreak-vr.yml

### Risks

- The session hard-requires XR_KHR_vulkan_enable2: XRSession has the runtime create the GS VkInstance and VkDevice, with no vulkan_enable v1 fallback. The Frame's native runtime must expose it to aarch64 apps.
- Swapchain negotiation accepts only R8G8B8A8 SRGB or UNORM, and the frame is copied with vkCmdCopyImage. A runtime offering only BGRA swapchains makes VR fail until a blit/swizzle path is added.
- The composition layer budget is capped at 16 (static_assert, sized for SteamVR). The Frame runtime's layer limit and its cylinder-layer support are unverified.
- The OpenXR session shares the GS graphics queue (queue index 0) under a 500 ms bounded lock. Adreno/Turnip usually has a single queue, so runtime stalls in xrEndFrame block the GS up to the bound, and then VR is abandoned.
- CameraDriver patches EE code every vsync with memWrite32 (silence patches and MIPS code-cave hooks). ARMSX2's ARM64 EE recompiler must invalidate stale blocks for these writes (manual_page/recClear exist there, but this is unverified for this path).
- Performance: 2-layer multiview roughly doubles GS raster cost, and the EE/VU JIT runs on a Snapdragon 8 Gen 3. Holding 60 fps game speed with headset-rate submission has only been measured on a PC.
- Camera sync assumptions (2-frame render sync, pose-lag tuning) were tuned on an x86 PC with SteamVR and may need retuning for on-device latency and EE timing.
- The VR pacer thread sets no priority or affinity, so on big.LITTLE it may land on an efficiency core and miss xrWaitFrame deadlines.
- VMManager::IsOnlineLocked (co-op online locks) is not ENABLE_VR-guarded and does not exist in ARMSX2. VR files call it, so it must be ported alongside the VR layer.
- The two repos use conflicting ini schemas and build flags: ARMSX2 has ENABLE_VR_STEREO and StereoSettings [VR] keys, PenguinScreen2 has ENABLE_VR and VROptions [VR] keys. They have to be reconciled when merging.
- ARMSX2's GS has diverged: Adreno-specific changes, a different ShadeBoost signature and a different SubmitCommandBuffer layout. The about 1,700 guarded hook lines must be re-applied by hand, not cherry-picked.
- MemWatch and its PINE MsgVRMemWatch depend on the x86 recompiler's memcheck hook. ARMSX2's ARM64 recompiler has no memcheck support, so those research tools will be inert.
- GSDeviceVK::Create acquires a window before the VR bootstrap. A standalone headset launch needs a gamescope/Wayland window or verified surfaceless operation.
- Tooling: pine.py assumes TCP port 28011 (Windows PINE), while Linux PINE uses a Unix socket. The PowerShell launchers are Windows-only.

### Open questions

- Which OpenXR runtime does SteamOS on the Steam Frame expose to native aarch64 apps (SteamVR for linux-arm64, Monado, or something else), and where is its active_runtime json?
- Does that runtime support XR_KHR_vulkan_enable2, XR_KHR_composition_layer_cylinder, XR_VALVE_frame_controller_interaction and R8G8B8A8_SRGB swapchains?
- Can a native app open its own Vulkan device on the Adreno 750 (Mesa Turnip) and share its queue with the runtime through vulkan_enable2 while the system compositor is running?
- Frontend choice: ship the pcsx2-qt AppImage under gamescope in the headset, or use ARMSX2's pcsx2-sdl (FullscreenUI)? The latter would need the --vr argument parsing and VR settings moved out of QtHost and VRSettingsWidget.
- Which base should the Frame build use: PenguinScreen2's ENABLE_VR plus VROptions (the full feature set), or ARMSX2's ENABLE_VR_STEREO plus StereoSettings (with the missing pieces added)?
- Does ARMSX2's ARM64 EE recompiler pick up memWrite32 code patches (CameraDriver silence and code-cave hooks) immediately, the same way the x86 recompiler does?
- What internal resolution and multiview cost can the Adreno 750 sustain at the Frame's display rate while the JIT holds full game speed?
- Is online co-op (DEV9 networking plus IsOnlineLocked) in scope for the standalone build, or should first person stay offline-only on the Frame?

## 2. ARMSX2 `outbreak-vr`: what is already there

VERDICT: ARMSX2's outbreak-vr branch is the right target, and most of the GPU-side port is already done. The branch already has nearly all of PenguinScreen2's stereo renderer: multiview two-layer targets, layer proxies, display-chain promotion, the stereo vertex block, and the Vulkan hook sites for OpenXR. Those Vulkan hook sites are switched off because ENABLE_OPENXR is never defined anywhere. What is missing is the whole VR runtime: XRSession, XRCompositor, VRManager, VRInput, CameraDriver and the other game-layer files, about 36.8k lines in PenguinScreen2/pcsx2/VR plus Input/VRInputSource. That code is portable C++ that was written Linux-first. A git merge will not work, though. PenguinScreen2 commit 990b456 ("PR#7 derivation pipeline normalization") stripped comments across 938 files (+13,000 / -44,956 lines; in GSRendererHW, 1453 of 1655 removed lines were comments). The hooks therefore have to be carried over by hand at matching anchors, not 3-way merged. Porting the other way (putting ARMSX2's arm64 JIT into PenguinScreen2) would be much harder: pcsx2/arm64 alone is 52 files and ~44k lines, plus 568 changed core files.

All git commands I ran were read-only (log, diff, grep, show). I did not edit, build or run anything.

1. WHAT THE BRANCH CONTAINS (D:\dev\ARMSX2, outbreak-vr = 5fc05a01a9, 7 commits on top of origin/master 3cf66e03aa)
- 39 files, +2355/-158 lines.
- D:\dev\ARMSX2\OUTBREAK-VR.md states: per-eye stereo done, headset session not done.
- pcsx2/VR has only StereoState.{h,cpp} and StereoSettings.{h,cpp}.
  - StereoSettings.h calls itself a "slim stand-in for PenguinScreen2's VRManager + VRProfileDB".
  - Its [VR] keys are StereoEnable, Separation, Convergence, ZDrivenDepth, PinUniformQ, CollimateDisparity, InterleaveEyes and DebugView (SBS/CrossEye/Left/Right).
  - PenguinScreen2 uses different key names (VROptions in Config.h, e.g. Enable, StereoSeparation, StereoConvergence, HeadsetRateSubmit).
- ARMSX2's StereoState.h is a strict subset of PenguinScreen2's. It lacks first_person, near_floor, the fp_auto fields, PublishHeadRoll/HeadRoll, RollParallax and DumpAllowed. The branch is an older snapshot of the same code.
- Commit 4f4bb5be20 (M2, both eyes per frame into per-eye targets) was checked only on Vulkan/MoltenVK (macOS) with the J's Bar dump. It has never run on Turnip or Adreno.
- PINE gained MsgPadSet 0x30.
- cmake/BuildParameters.cmake: option ENABLE_VR_STEREO (default ON) adds the define ENABLE_VR.
- Dormant hooks in D:\dev\ARMSX2\pcsx2\GS\Renderers\Vulkan\GSDeviceVK.cpp, all under `#ifdef ENABLE_OPENXR`:
  - line 43: include of VR/VRVulkanBridge.h (the file does not exist in ARMSX2)
  - line 239: CreateVulkanInstanceThroughXR
  - line 1011: CreateVulkanDeviceThroughXR
  - line 3555: OnGSDeviceDestroyed
  - line 4128: BeginVulkanBootstrap
  - line 4204: GetXrVulkanPhysicalDevice
  - line 4261: OnVulkanDeviceCreated

2. CI AND FRONTEND
- D:\dev\ARMSX2\.github\workflows\outbreak-vr.yml
  - Runs on pushes to outbreak-vr and calls linux_build_qt.yml.
  - Runner ubuntu-24.04-arm, clang-17, flags -DHOST_PAGE_SIZE=4096 -DHOST_CACHE_LINE_SIZE=64 -DENABLE_VR_STEREO=ON, buildAppImage true.
- The AppImage is the Qt frontend: appimage-qt.sh packages build/bin/armsx2-qt.
  - Built with linuxdeploy-aarch64 and the Qt plugin.
  - Qt 6.11.2 built from source (build-dependencies-qt.sh), including qtwayland, plus KDDockWidgets, SDL3 and shaderc.
  - libvulkan is not bundled, so it uses the system ICD.
- So yes, ARMSX2's Qt UI runs on Linux arm64. It is a mainline target: build-all.yml builds "PC · Linux arm64 (Qt)" in both 4K-page and 16K-page variants.
- Other frontends exist but outbreak-vr does not build them:
  - armsx2-sdl (D:\dev\ARMSX2\pcsx2-sdl\Main.cpp): SDL3 handheld frontend using VK_KHR_display or a Wayland surface, with the ImGui FullscreenUI.
  - The libretro core.
  - pcsx2-gsrunner: the headless dump replayer, which has the -vr-stereo / -vr-hud / -vr-view flags used for the stereo tests.
- Surfaceless windows are supported in both repos' GSDeviceVK. A VR-only mode with no window is therefore possible with a small frontend change.

3. VULKAN DRIVER PATH AND SHARING THE DEVICE WITH OPENXR
- The custom Adreno driver loader is Android-only.
  - VKLoader.cpp uses adrenotools (Android linker namespaces, /system/lib64/libvulkan.so).
  - ARMSX2_USE_ADRENOTOOLS is set only for Android arm64-v8a: D:\dev\ARMSX2\pcsx2\CMakeLists.txt:892-894 and platforms/android/app/src/main/cpp/CMakeLists.txt:36.
- On Linux, LoadVulkanLibrary opens the system libvulkan.so.1. On the Frame that means whatever ICD SteamOS ships, presumably Mesa Turnip for the Adreno 750. To force a different Mesa build you would use the VK_DRIVER_FILES environment variable.
- ARMSX2's driver-rule database is keyed on the driver, not the OS. GSDeviceVK.cpp:1279-1280 says Turnip on an ARM Linux handheld is the same driver as Turnip on a phone. So its Turnip workarounds apply on the Frame, for example the rule that Turnip before Mesa 26.2 hangs on EARLY_Z_LATE_Z with D32S8.
- Can an OpenXR runtime use the same VkInstance/VkDevice? Yes, the same way PenguinScreen2 already does it:
  - XRSession.cpp:360/392 passes the dynamically loaded global vkGetInstanceProcAddr to xrCreateVulkanInstanceKHR and xrCreateVulkanDeviceKHR.
  - ARMSX2 exposes the same global (VKEntryPoints.inl:13).
  - So the runtime creates the very instance and device the GS uses, through the same loader and the same Turnip ICD.
  - ARMSX2 requests Vulkan 1.1 and enables multiview whenever the device has it.
  - The Vulkan backend has no geometry shaders, so multiviewGeometryShader is not needed.
- The VR layer needs only:
  - XR_KHR_vulkan_enable2 (required)
  - XR_KHR_composition_layer_cylinder (optional)
  - XR_VALVE_frame_controller_interaction, plus the valve/frame_controller interaction profile with oculus/touch and khr/simple fallbacks.
  - PenguinScreen2 vendors the Khronos OpenXR SDK (3rdparty/openxr, 5.7 MB) and builds a static loader (cmake/SearchForStuff.cmake). ARMSX2 does not have it.

4. HOW FAR THE PCSX2 BASES HAVE DRIFTED
- PenguinScreen2 was imported from upstream PCSX2 92914cee4 (2026-07-12; commit 34b74d8).
- That commit is an ancestor of ARMSX2 master. ARMSX2 has synced upstream up to about 2026-09-26 ("Merge upstream PCSX2 (2026-08-30 .. 2026-09-25)"), which is 278 upstream commits later, plus ARMSX2's own large changes.
- Lines changed from 92914cee4 to ARMSX2 master, total vs the upstream-only part:

| File | Total | Upstream only |
|---|---|---|
| GSRenderer.cpp | +707/-119 | +42/-14 |
| GSRendererHW.cpp | +2133/-271 | +168/-108 |
| GSTextureCache.cpp | +956/-168 | — |
| GSDeviceVK.cpp | +3790/-285 (now 10,746 lines vs 7,025 in PenguinScreen2) | +162/-117 |
| GSDevice.cpp/.h | +738 / +605 | — |
| tfx.glsl | +200/-74 | — |
| VMManager.cpp | +1434/-82 | +55/-6 |
| InputManager.cpp | +120/-35 | +51/-35 |
| PINE.cpp | +524 | — |
| QtHost.cpp | +47/-70 | — |
| MainWindow.cpp | +216/-170 | — |
| Qt overall (excl. translations) | 104 files, +2235/-1189 | — |
| pcsx2-gsrunner/Main.cpp | +2488 | — |
| Hotkeys.cpp | unchanged | unchanged |

5. MERGE / PORT DIFFICULTY PER HOOK POINT
(a) Build glue: easy.
- Vendor 3rdparty/openxr and copy the static-loader block from SearchForStuff.cmake.
- Add the ~30 VR sources and VRInputSource to pcsx2/CMakeLists.txt, and VRSettingsWidget to pcsx2-qt/CMakeLists.txt.
- Define ENABLE_VR and ENABLE_OPENXR together, or rename the existing ENABLE_OPENXR guards.

(b) The VR layer itself (pcsx2/VR/*): easy to moderate (fixes for API drift).
- Copy it whole and replace ARMSX2's StereoState with PenguinScreen2's superset. Retire StereoSettings, or keep DebugView/InterleaveEyes as debug knobs.
- It is already Linux-first: SeatSession.cpp is Linux-only (WiVRn seats) with a Windows stub.
- No x86 intrinsics and no MSVC-only calls (scanned).
- Every core header it needs exists in ARMSX2: common/YAML.h (rapidyaml vendored), USB/USB.h, DebugTools/Breakpoints.h, SIO/Pad/PadBase.h, MTGS.h, Counters.h.
- The GSDeviceVK/GSTextureVK APIs the compositor calls all exist: GetAllocator, DeferBufferDestruction, the fence counters, ExecuteCommandBuffer, InRenderPass/EndRenderPass, GetCurrentCommandBuffer, TransitionToLayout, GetImage.

(c) GSDeviceVK: low to moderate.
- Instance, device, physical-device and lifecycle hooks are already in place.
- Missing: VR::ScopedXrQueueLock at three places:
  - WaitForGPUIdle (ARMSX2 line 1750)
  - SubmitCommandBuffer, from the submit through the present (lines 2118 and 2158)
  - the spin submit (line 2925)
- ARMSX2-only users of the graphics queue also need the lock, or frame generation must be forced off in VR: GSLsfg.cpp lines 871 (vkDeviceWaitIdle), 1348 (vkQueueSubmit), 1368 and 1378 (vkQueuePresentKHR), plus FrameGen.
- Also missing: the DATE-stencil-on-stereo warning.
- Cosmetic: the XR instance path returns before s_enabled_instance_extensions is filled, so the driver report would show nothing.

(d) GSRendererHW: moderate.
- Already present: the stereo block (ARMSX2 around line 7301: bands, log map, collimation, the Z-to-Q fit) and the promotion / feed-edge block (around line 11450).
- Missing compared with PenguinScreen2 (its block starts around line 5877):
  - the first-person linear map with near_floor
  - roll parallax (HeadRoll)
  - DepthHistogram draw classes
  - the PCSX2_VR_SNAPSHOT_LAYER dump hooks
  - the newer diagnostics
- This means re-syncing roughly 200-300 lines by hand into ARMSX2's heavily changed draw function.

(e) GSTextureCache: easy.
- Same two hook sites (ARMSX2 lines 4264 and 7810). Only the condition differs: StereoSettings::PerEyeTargets() vs StereoState::Get().enabled.

(f) tfx.glsl and the constant buffer: easy.
- The vertex constant-buffer layout is identical in GSDevice.h (vr_stereo, vr_map_mode, vr_band_count, vr_splits, vr_band[4]).
- Only the near floor (vr_splits.x) and the roll terms (vr_splits.y/z) need porting.

(g) GSRenderer.cpp: low to moderate. Needed:
- an overload GSDevice::ShadeBoost(const GSVector4&) for PresentationLift
- VR::EnsureFrameSubmitted in the frame-skip path (ARMSX2 line 1158, which has an extra skip_blank case)
- VR::EndOfFrame and StereoDump::MaybeCapture after the present (around line 1341)
- EndOfFrame in PresentCurrentFrame (line 1569)
- the snapshot-layer hook

(h) VMManager: easy.
- The same anchor functions exist: LoadSettings, ApplySettings, ClearDiscDetails, DoLoadState, VSyncOnCPUThread.
- Replace StereoSettings::Apply with VR::UpdateSettings.
- Check the VR pacer against upstream 1810140d1d ("Split frame limiter in to two wait points"), which ARMSX2 has.

(i) InputManager / VRInputSource: easy.
- InputSource's virtual interface is byte-identical in both repos, and the InputSourceType enum is identical apart from the VR entry.
- Upstream only added InputLayout::Generic.

(j) PadDualshock2, Hotkeys, Pcsx2Config/Config.h (VROptions): easy.
- ARMSX2's PadDualshock2 changes are in Set(), not the read path PenguinScreen2 hooks.

(k) PINE: easy. The command IDs do not collide: PenguinScreen2 uses 0xE0-0xE5, ARMSX2 uses 0x10-0x15 and 0x30.

(l) EE JIT memcheck hook (PenguinScreen2 pcsx2/x86/ix86-32/iR5900.cpp:1480, VR::MemWatchOnHit): no arm64 equivalent.
- ARMSX2's arm64 EE JIT has no CBreakPoints/memcheck support, so MemWatch (PINE 0xE1/0xE2, a research tool) will not work on the Frame unless the interpreter is used.
- Self-modifying code is still protected: the arm64 JIT uses the same mmap_MarkCountedRamPage scheme (iR5900-arm64.cpp:3128). CameraDriver's code-cave hooks should therefore invalidate correctly. The Outbreak profiles (SLPM-65428, SLPM-65692) use only look-at memory writes, no code hooks.

(m) Qt: easy.
- QtHost: launch args and profile validation at launch.
- SettingsWindow: adding the widget (ARMSX2 changed this file by only +8/-7).
- VRSettingsWidget: 338 lines plus its .ui file.

(n) gsrunner null-HMD and depth-histogram hooks: moderate but optional.

6. SUGGESTED ORDER
1. Vendor OpenXR and copy pcsx2/VR plus VRInputSource into outbreak-vr.
2. Turn on ENABLE_OPENXR together with ENABLE_VR.
3. Carry over hooks (b) through (m).
4. Get the build green on ubuntu-24.04-arm in CI.
5. Smoke-test without a headset on arm64 Linux using Monado's simulated or qwerty HMD with lavapipe or Turnip. This could even run as a CI job.
6. Only then try the Frame.

Key paths:
- D:\dev\ARMSX2\OUTBREAK-VR.md
- D:\dev\ARMSX2\.github\workflows\outbreak-vr.yml, linux_build_qt.yml, build-all.yml
- D:\dev\ARMSX2\.github\workflows\scripts\linux\appimage-qt.sh, build-dependencies-qt.sh
- D:\dev\ARMSX2\pcsx2\VR\StereoState.h, StereoSettings.h
- D:\dev\ARMSX2\pcsx2\GS\Renderers\Vulkan\GSDeviceVK.cpp, VKLoader.cpp, GSLsfg.cpp
- D:\dev\ARMSX2\pcsx2\GS\Renderers\HW\GSRendererHW.cpp, GSTextureCache.cpp
- D:\dev\ARMSX2\pcsx2\GS\Renderers\Common\GSRenderer.cpp
- D:\dev\ARMSX2\pcsx2\VMManager.cpp, PINE.cpp
- D:\dev\ARMSX2\pcsx2-sdl\Main.cpp
- D:\dev\PenguinScreen2\pcsx2\VR\VRVulkanBridge.h, XRSession.cpp, XRCompositor.cpp, SeatSession.cpp
- D:\dev\PenguinScreen2\cmake\SearchForStuff.cmake
- D:\dev\PenguinScreen2\pcsx2\x86\ix86-32\iR5900.cpp

### Risks

- The Frame's on-device OpenXR runtime is unverified for native arm64 Linux apps: whether XR_KHR_vulkan_enable2 is available, where active_runtime.json is, and whether an app launched outside Steam may start an OpenXR session.
- The page size is fixed at compile time. outbreak-vr builds only -DHOST_PAGE_SIZE=4096; if the Frame's kernel uses 16K pages the binary will not run. build-all.yml already has a 16K variant.
- The stereo path has only been checked on MoltenVK (macOS). Turnip with multiview, together with ARMSX2's Turnip-specific feedback-loop and framebuffer-fetch code paths, has never been run.
- The OpenXR session shares the GS graphics queue, and Turnip may expose only one queue. ARMSX2's frame generation (GSLsfg/FrameGen) submits and presents on that queue and would need the lock or be forced off in VR.
- A git merge is not feasible: PenguinScreen2 commit 990b456 stripped comments from 938 files (-44,956 lines), so every hook in GSRendererHW, GSDeviceVK, VMManager and the other files must be carried over by hand and can drift.
- PenguinScreen2 pc-firstperson now only builds in Windows CI. Waves 4-7 have never been compiled with GCC/Clang on Linux, although no MSVC-only calls were found in pcsx2/VR.
- Performance and heat: VR doubles the fill rate on the Snapdragon 8 Gen 3, the VR pacer adds compositor copies, and the SteamVR compositor shares the same GPU. Outbreak's speed under ARMSX2 on Linux arm64 is unmeasured.
- MemWatch (PINE 0xE1/0xE2) depends on x86-JIT memchecks; ARMSX2's arm64 EE JIT has no breakpoint or memcheck support.
- The AppImage needs FUSE2 on SteamOS-ARM (fallback: --appimage-extract-and-run). Qt over Wayland under the Frame's compositor may need a VR-only mode with no window (Surfaceless).
- outbreak-vr's StereoSettings [VR] keys (StereoEnable/Separation/Convergence) clash in naming with PenguinScreen2's VROptions keys (Enable/StereoSeparation/StereoConvergence) and must be reconciled.

### Open questions

- Does the Frame's SteamOS expose an OpenXR runtime (SteamVR/vrclient arm64 or Monado) to native arm64 Linux apps launched outside Steam, with XR_KHR_vulkan_enable2 and the valve/frame_controller profile?
- What is the Frame's kernel page size (getconf PAGESIZE) and its Mesa/Turnip version? ARMSX2's driver rules change behaviour at Mesa 26.2.
- How are non-Steam Linux apps launched on the Frame: is there a desktop mode, can they be added as a non-Steam game, and is FUSE2 available for AppImages?
- Did the outbreak-vr GitHub Actions AppImage build succeed? Answered afterwards with `gh run list`: yes, run 37707272283 on 4d22c34 (2026-10-08) is green; the later 5fc05a0 only adds a .md file, which the workflow ignores.
- Should the port go into ARMSX2 outbreak-vr as recommended, with PenguinScreen2 staying the Windows/SteamVR build, and how will the two copies of pcsx2/VR be kept in sync afterwards?
- Does the frontend on the Frame stay Qt (armsx2-qt AppImage), or should a lean VR-only launcher (Surfaceless GS, based on armsx2-sdl or Qt -nogui) be added?
- Does Turnip on the Adreno 750 expose more than one graphics queue? That decides whether the planned dedicated XR VkQueue can replace the shared-queue lock.

## 3. The Steam Frame as a platform

STEAM FRAME AS A NATIVE PLATFORM FOR OUTBREAK VR: RESEARCH FINDINGS (2026-10-11)

== 0. Bottom line ==
Running ARMSX2 natively on the Frame with its own OpenXR session is a supported path, and other projects have already done it. Valve documents Linux ARM64 as a first-class standalone target, deployed with the SteamOS Devkit Client. The on-device OpenXR runtime is SteamVR's own linuxarm64 build. The GPU driver is Mesa Turnip, not Qualcomm's. Two community projects already run native arm64 OpenXR+Vulkan programs standalone on the Frame:
- Star Fox Enhanced: a devkit title that reaches FOCUSED with two eye swapchains.
- dolphinXR: a Dolphin OpenXR fork that "runs in VR mode" from a distrobox.
The missing piece is still what OUTBREAK-VR.md says: ARMSX2 has no XrInstance/XrSession yet. PenguinScreen2's pcsx2/VR session code is a good fit to port:
- It uses XR_KHR_vulkan_enable2 and OpenXR API 1.0.
- It enables the cylinder layer and the Frame controller profile only when the runtime offers them.
- Every swapchain has arraySize=1.
- It destroys the instance together with the session.
All of these avoid known Frame runtime bugs (section 9).

== 1. Platform facts ==
Hardware:
- Snapdragon 8 Gen 3 (SM8650) with Adreno 750. 16 GB LPDDR5X. 256 GB or 1 TB UFS, plus microSD.
- Two 2160x2160 LCD panels at 72/80/90/120 Hz, with 144 Hz marked experimental.
- 21.6 Wh battery with 45 W USB-C charging. Eye tracking. Active fan, confirmed by the iFixit teardown.
- Released 2026-09-18. Sources: https://en.wikipedia.org/wiki/Steam_Frame , https://www.uploadvr.com/valve-steam-frame-official-announcement-features-details/ , https://hothardware.com/news/steam-frame-teardown-modular-vr-designa-breeze-until-hit-fan
OS:
- SteamOS on Collabora's pure aarch64 Arch port ("Holo Core"). VARIANT_ID=vr, kernel 6.18 per community field notes.
- Root filesystem is read-only btrfs, about 10 GB. /home is about 929 GB on the 1 TB model.
- glibc 2.39 and libfuse.so.2 are on the image. Present tools: python3, rsync, flatpak, podman, ffmpeg, git, konsole, dolphin (the file manager).
- The SSH user is `steamos` (uid 1000, in wheel).
- Sources: https://www.gamingonlinux.com/2026/07/collabora-announce-a-preview-of-holo-core-an-aarch64-port-of-arch-linux-for-steam-frame/ ; community notes in https://github.com/saphid/frame-control (docs/how-the-frame-works.md).
Execution models Valve lists: native Linux ARM64, Android APKs through Lepton (a Waydroid fork; Android 11 / API 30, arm64 only), and x86 through FEX with Proton. Source: https://partner.steamgames.com/doc/steamhardware/steamframe/compatibility
FEX is a bad fit for an x86-JIT emulator, as expected; the native ARMSX2 recompilers are the right path. The local repo D:\dev\ARMSX2\pcsx2\arm64\ has iR5900-arm64, iR3000A-arm64 and microVU-arm64. Unity's Frame page notes that GPU buffer access under FEX is uncached and slow.

== 2. How a native arm64 app is launched in standalone mode and becomes the active VR app ==
Official route: SteamOS Devkit Client. Source: https://partner.steamgames.com/doc/steamhardware/steamframe/loadgames
- On the headset: Settings > System > Enable Developer Mode, then set a user password under Developer. This enables SSH, ADB and RDP.
- Pair the PC from Settings > Developer. In the Devkit Client (a Steam tool on the PC), connect to `frame`.
- Title Upload fields: Name; Local Folder (uploaded recursively); Start Command (path relative to the folder); Runtime. For Linux ARM64 binaries, Valve's page names "Steam Linux Runtime 3.0 ARM64 (Sniper)".
- The title appears under Library > Non-Steam > "Devkit Game: <name>". Launch options and env vars go in Properties as `VAR=1 %command%`.
What actually happens on the device (community, Frame Control docs/sideloading.md, verified 2026-09-26):
- Valve's scripts land in ~/devkit-utils. Files go to ~/devkit-game/<id>.
- `steam-client-create-shortcut` registers the title over ~/.steam/steam.pipe. `steam-devkit-rpc run-game gameid=<id>` launches it.
- The Frame's runtime alias is `SteamLinuxRuntime_4-arm64`. Steam records that mapping, but an aarch64 devkit title was launched natively, without the runtime's entry-point prefix.
- So an AppImage runs on the host (glibc 2.39 and FUSE2 are present). Uploading the extracted AppDir avoids the FUSE dependency entirely, which also covers a future change that puts titles inside the container.
- Star Fox Enhanced builds against the Sniper ARM64 sysroot, runs "natively with Steam Play turned off" under SteamLinuxRuntime_4-arm64, and uses a launch script as the Start Command. Source: https://github.com/kandowontu2/starfox-enhanced/blob/main/docs/STEAM-FRAME.md
- Title ids must match ^[A-Za-z_][A-Za-z0-9_.]+$.
Alternative routes:
- Desktop mode: a nested KDE Plasma session shown as a 1280x800 panel from the SteamVR dashboard, with Konsole.
- SteamVR's "Launch a program" list (Frametop installs entries there): https://github.com/DeeJanuz/frametop
- dolphinXR ran from an Arch Linux ARM distrobox. It needed `/opt` mounted, because SteamVR lives in /opt/steamvr, and `pid=host` to talk to the host's SteamVR processes. Source: https://github.com/iChris4/dolphinXR/issues/7
- Over SSH: set XDG_RUNTIME_DIR=/run/user/1000 (as the SteamVR-for-Linux #975 and #989 repros do).
Becoming the active VR app:
- The loader finds the runtime through ~/.config/openxr/1/active_runtime.json, which points at /opt/steamvr/bin/linuxarm64/vrclient.so.
- Once the app's XrSession is running, SteamVR treats it as the scene app.
- A Steam-launched app is shown in Theater (a flat panel) until then (setting /settings/dashboard/autoShowGameTheater). While no instance exists, for example during a ~2.1 s xrCreateInstance on the Frame, the app falls back to Theater. Source: https://github.com/ValveSoftware/SteamVR-for-Linux/issues/989
- A Steam-launched X11/Wayland window becomes a gamescope panel (gamescope --backend openvr, one overlay per app id).
- stdout isn't kept on the Frame, so log to a file. The headset in standby hides OpenXR sessions (one frame, then stop). For headset-free tests the community uses `vrcmd --set-settings-bool power.pauseCompositorOnStandby 0` plus `vrcmd --handlewakeup`.

== 3. OpenXR runtime and extensions on the device ==
Runtime:
- SteamVR's native aarch64 build (package deckard-steamvr) in /opt/steamvr with linuxarm64 binaries.
- xrGetInstanceProperties reports "SteamVR/OpenXR 2.18.1/2.18.2". 2.17.10 was the stable channel on SteamOS 0.3.0.
- Logs are in /home/steamos/.local/share/Steam/logs (shell alias `cdl`). The client log is ~/.steam/steam/logs/xrclient_<app>.txt.
- Sources: SteamVR-for-Linux #975 and #989; https://partner.steamgames.com/doc/steamhardware/steamframe/debugging
Graphics binding: XR_KHR_vulkan_enable and XR_KHR_vulkan_enable2 both work on the device (#989 tested both; #975 used enable2 with Vulkan 1.2 and R8G8B8A8_SRGB).
API version: SteamVR's Android runtime is OpenXR 1.0 only (https://github.com/saphid/frame-control/pull/20). Requesting XR_API_VERSION_1_0, as PenguinScreen2 does, is safe.
Controller profile:
- /interaction_profiles/valve/frame_controller_valve, which requires XR_VALVE_frame_controller_interaction.
- Left hand: dpad_up/down/left/right, view, system, bumper, squeeze (click/value), trigger (click/value), thumbstick, grip/pose, aim/pose, output/haptic.
- Right hand: a, b, x, y, menu, plus the same bumper, squeeze, trigger, thumbstick, poses and haptic.
- Fallback order: Frame profile, then XR_KHR_generic_controller, then Oculus Touch.
- Source: https://partner.steamgames.com/doc/steamhardware/steamframe/input
Eye gaze: XR_EXT_eye_gaze_interaction (Valve docs). In OpenVR it is an "eyetracking" action.
Foveation extensions Valve recommends: XR_FB_foveation, XR_FB_foveation_configuration, XR_FB_foveation_vulkan, XR_FB_swapchain_update_state, XR_META_foveation_eye_tracked, XR_META_vulkan_swapchain_create_info. Source: https://partner.steamgames.com/doc/steamhardware/steamframe/engines/custom
Reprojection inputs Valve names: XR_KHR_composition_layer_depth, XR_EXT_frame_synthesis, XR_FB_space_warp. Source: https://partner.steamgames.com/doc/steamhardware/steamframe/compat/perf_criteria
Refresh rate: requested with XR_FB_display_refresh_rate. Star Fox logged the runtime offering 72-144 and noted that the system 120 Hz setting doesn't apply to the game by itself. Unity's Frame page says SteamVR exposes only the current rate. These two reports conflict, so test on the device.
Extension names found in SteamVR's own vrclient binary (PC build of 2026-10-10, https://github.com/SteamTracking/GameTracking-SteamVR bin/vrclient_strings.txt). This list was extracted from strings; it is not a confirmed device enumeration:
- KHR: vulkan_enable, vulkan_enable2, opengl_enable, opengl_es_enable, composition_layer_depth, visibility_mask, generic_controller, locate_spaces, binding_modification, convert_timespec_time.
- EXT: eye_gaze_interaction, hand_tracking, hand_interaction, palm_pose, dpad_binding, local_floor, user_presence, frame_synthesis, render_model, interaction_render_model, view_configuration_views_change.
- FB: display_refresh_rate, foveation*, space_warp. META: foveation_eye_tracked, performance_metrics, recommended_layer_resolution, vulkan_swapchain_create_info.
- VALVE: frame_controller_interaction, analog_threshold, app_space_delta_pose, timing_utils. Also MNDX_egl_enable.
- XR_KHR_composition_layer_cylinder does NOT appear as an extension name, only as a type string. Expect PenguinScreen2's flat-screen layer to fall back to quads (D:\dev\PenguinScreen2\pcsx2\VR\XRSession.cpp checks for it before enabling).
Implicit API layer: /usr/share/openxr/1/api_layers/implicit.d/XrApiLayer_VALVE_fdm_injection.json is active for every OpenXR app unless DISABLE_VULKAN_FDM_INJECTION_LAYER is set (#972).
Valve's bar for standalone VR: at least 72 fps at 1728x1728 per eye for Verified; below 1440x1440 is Unsupported. vrpreferences.json at the package root (keys preferResolution, preferMinRefreshRate, preferHalfFramerate, preferMotionSmoothingMode) sets the defaults: 1728 and 72 Hz. Sources: https://partner.steamgames.com/doc/steamhardware/steamframe/compat and /vrpreferences

== 4. GPU driver; can apps bring their own? ==
The driver is Mesa Turnip.
- Igalia confirmed it is used instead of Qualcomm's driver (https://www.phoronix.com/news/Steam-Frame-Turnip-Vulkan ; https://www.igalia.com/chats/steamy).
- On the device, vulkaninfo shows "Turnip Adreno (TM) 750", DRIVER_ID_MESA_TURNIP, Mesa 26.3.0-devel (git-abc426bb62), device apiVersion 1.4.362, conformance 1.4.0.0. The package is deckard-mesa-linux-aarch64 26.3.0_devel+gitabc426bb-1.
- OpenGL goes through zink. mGBA's AppImage on the Frame couldn't create a GL context (https://github.com/mgba-emu/mgba/issues/3877).
- Instance layers include VK_LAYER_FROG_gamescope_wsi_aarch64, VK_LAYER_VALVE_fdm_injection, VK_LAYER_VALVE_rpo (Renderpass Optimizer), fossilize, MangoHud and RenderDoc.
- Lepton also uses Valve's Mesa: Valve's debug package is deckard-mesa-android-aarch64-debug (https://partner.steamgames.com/doc/steamhardware/steamframe/debugging).
Bringing your own driver:
- Valve doesn't document it. On Linux the Vulkan loader would accept a bundled Mesa ICD through VK_DRIVER_FILES, but nobody has reported doing it on the Frame. SteamVR's compositor interop and Valve's Frame-specific Mesa work (Valve says the Alyx port needed driver work) make it risky, and it isn't needed.
- Qualcomm's proprietary blob would need the KGSL kernel interface; whether the Frame kernel exposes it is unconfirmed.
- ARMSX2's custom-driver feature is Android-only: D:\dev\ARMSX2\pcsx2\GS\Renderers\Vulkan\VKLoader.cpp loads through adrenotools only under ARMSX2_USE_ADRENOTOOLS, set in pcsx2/CMakeLists.txt only for ANDROID arm64. On Linux it loads the system libvulkan, which is correct.
- ARMSX2 2.6.9 already limits its Adreno stencil-kill workaround to Turnip older than 26.2, so it will not trigger on the Frame's 26.3.

== 5. Getting ISOs and BIOS onto the device ==
- SSH, SCP, rsync or SFTP to steamos@frame. Valve suggests WinSCP on Windows. rsync is on the image.
- Devkit Client Title Upload is for the app folder; it uses rsync.
- USB-C networking: the Frame shows up as a USB network device (usb0 10.86.200.233; community).
- Desktop mode: Dolphin file manager, browser downloads, Flathub user installs (Syncthing, for example).
- microSD: Valve says cards move between Deck, Machine and Frame. The Deck formats cards as ext4; the Frame's format is unconfirmed.
- adb push reaches only Lepton's Android storage.
- Keep everything under ~, because the root filesystem is read-only and gets replaced by OS updates. Updates install automatically and ~ survives them (community).
- PS2 BIOS and ISOs should go in ARMSX2's XDG data/config directory, or a folder the app is pointed at. Never in the uploaded title folder: `steamos-delete` removes that folder.
Sources: https://partner.steamgames.com/doc/steamhardware/steamframe/debugging ; https://borecraft.com/2025/11/13/valve-recommends-sharing-your-game-library-on-microsd-card-between-steam-deck-steam-frame-and-steam-machine-theyre-fully-interoperable-just-bring-it-along-with-you/ ; frame-control docs/file-transfer.md and how-the-frame-works.md.

== 6. PS2 emulation performance expectations on Snapdragon 8 Gen 3 ==
Flat PS2 emulation (no direct Frame measurements exist):
- Android guides (lower quality, partly SEO) put 8 Gen 2/8 Gen 3 at essentially the full library at full speed, with 2x-3x upscaling. Sources: https://retrohandheldhq.com/posts/nethersx2-setup-guide/ ; https://www.androidauthority.com/optimized-ps2-emulator-3659642/
- ARMSX2 2.6.9 (native ARM64 recompiler on the PCSX2 2.7 core): on Snapdragon 865, 20 of 22 test games fit their worst-case frame budget. On 8 Gen 2 PS2 was "already comfortable". Source: https://heldgames.com/guides/armsx2-2-6-9-handheld-performance
- The 8 Gen 3 CPU is about 25% faster multi-core and about 11% faster single-core than 8 Gen 2 (https://www.gsmarena.com/we_benchmark_the_snapdragon_8_gen_3-news-60774.php).
- Resident Evil Outbreak is light on the EE. The PCSX2 wiki has test entries of 300% speed (1.1.0) and a steady 60 at 6x (1.3.1) for File #2 (https://wiki.pcsx2.net/Resident_Evil_Outbreak_File_№2).
- Nobody has published ARMSX2 or PCSX2 running on a Frame.
VR adds:
- Two-eye GS rendering. ARMSX2 outbreak-vr already uses multiview with 2-layer targets, which suits Turnip.
- Headset-rate submission at 72-90 Hz while the game itself runs at 30-60.
- SteamVR's compositor, tracking and eye-tracking load inside a roughly 10 W SoC envelope.
Recommendation:
- Start at 72 Hz, about 1x-2x internal resolution per eye, MTVU on, Vulkan only.
- Measure with Valve's in-headset Perf Overlay and "Record VR Performance", which writes a CSV with fps, CPU/GPU times, reprojection, GPU clock, power and throttle to the logs folder.
- 1728x1728 per eye is the eye-buffer target; that is separate from PS2 internal resolution.

== 7. Thermal and battery ==
- Valve and press describe the 8 Gen 3 here as roughly a 10 W sustained part (UploadVR; https://vr.org/articles/steam-frame-verified-90fps-stricter-than-quest-pico). It has a fan; Ars Technica noted fan noise.
- Measured standalone battery life: Road to VR 1-1.5 h mixed use (https://roadtovr.com/valve-steam-frame-review/). Engadget about 2 h light use and under 90 min for Half-Life: Alyx (https://www.engadget.com/2257659/valve-steam-frame-review/). The Verge about 1 h under demanding use.
- Expect about 1-1.5 h unplugged for PS2 plus VR. Use a 45 W charger or a powered cable for long sessions; a PC's USB port can't keep it running (Valve's adb page).
- Sustained clocks and throttle points are unpublished. The perf CSV has a throttle column, and the battery and temperature sysfs node is /sys/class/power_supply/max1720x_bat_7-36 (community).
- Steam's idle timer suspends the Frame (system_idle_suspend_battery_sec 900, ac 3600). In standby it drops off the network.

== 8. Android/Lepton as an alternative ==
- The ARMSX2 APK could run in Lepton, but SteamVR's Android OpenXR runtime is 1.0 only.
- Lepton needs a MAIN+LAUNCHER activity, has no clipboard service, uses container overhead, and still needs an OpenXR integration.
- Native Linux is the better path.

== 9. Frame-specific gotchas for porting PenguinScreen2's VR layer onto ARMSX2 ==
(a) SteamVR-for-Linux #989: a second XrSession on the same XrInstance with a new VkDevice aborts the process (assert at sharedresource_linux.cpp:1692).
- PenguinScreen2 already avoids it: D:\dev\PenguinScreen2\pcsx2\VR\VRVulkanBridge.cpp AbortBootstrap calls DestroySession and DestroyInstance together, and the VkInstance and VkDevice are created through xrCreateVulkanInstanceKHR / xrCreateVulkanDeviceKHR.
- Keep that when porting. Expect about 2.1 s for each xrCreateInstance and a Theater flash on each GS device restart.
(b) #975: quad layers ignore imageArrayIndex, so layer 0 is always shown.
- PenguinScreen2 uses arraySize=1 everywhere (XRCompositor.cpp), which is safe.
- ARMSX2's 2-layer per-eye targets should be submitted as two arraySize-1 projection swapchains, or projection layers with arraySize 2 must be verified on the device. ValveSoftware/openvr#1822 also reports a layout quirk with array swapchains.
(c) The fdm_injection implicit OpenXR/Vulkan layer can hang vkCreateDevice outside Steam (#972). It may also inject fragment density maps into PCSX2's many offscreen passes. Test with and without DISABLE_VULKAN_FDM_INJECTION_LAYER=1.
(d) The Gamescope WSI layer breaks apps that create a swapchain outside gamescope. mGBA crashes; Handy needed DISABLE_GAMESCOPE_WSI=1 (https://github.com/cjpais/Handy/issues/2264). ARMSX2's Qt window swapchain is at risk when launched from the desktop.
(e) No native OpenGL, only zink. Keep Qt and ARMSX2 off GL.
(f) Build baseline: D:\dev\ARMSX2\.github\workflows\outbreak-vr.yml builds on ubuntu-24.04-arm (glibc 2.39, which matches the image exactly) with HOST_PAGE_SIZE=4096.
- The kernel page size is not published. 4K is very likely, because FEX needs 4K pages; confirm with `getconf PAGESIZE`.
- Moving the runner to a newer Ubuntu would break the glibc match. Star Fox's approach, cross-building against the Steam Runtime Sniper arm64 sysroot, is more robust.
(g) A SteamVR health-check hazard while developing: repeated short or failed SteamVR runs (3 or more in a boot) trigger steam-health-check --repair-now, which wipes ~/.local/share/Steam. This has caused boot loops (frame-control docs). Avoid killing and restarting SteamVR in a loop.
(h) The headset must be worn, or standby defeated, for the session to reach VISIBLE/FOCUSED.

Local files referenced: D:\dev\ARMSX2\OUTBREAK-VR.md ; D:\dev\ARMSX2\.github\workflows\outbreak-vr.yml ; D:\dev\ARMSX2\cmake\BuildParameters.cmake ; D:\dev\ARMSX2\pcsx2\GS\Renderers\Vulkan\VKLoader.cpp ; D:\dev\ARMSX2\pcsx2\arm64\ ; D:\dev\PenguinScreen2\pcsx2\VR\XRSession.cpp ; D:\dev\PenguinScreen2\pcsx2\VR\XRCompositor.cpp ; D:\dev\PenguinScreen2\pcsx2\VR\VRVulkanBridge.cpp. Nothing in either repo was edited, built or run.

Key sources:
- Valve Steamworks: https://partner.steamgames.com/doc/steamframe and the steamhardware/steamframe pages setup, loadgames, debugging, input, engines/custom, engines/godot, engines/unity, compat, compat/perf_criteria, compatibility, vrpreferences, adb_lepton
- SteamVR-for-Linux issues #975, #989 and #972: https://github.com/ValveSoftware/SteamVR-for-Linux/issues/975 , /989 , /972
- Native-port precedents: https://github.com/kandowontu2/starfox-enhanced/blob/main/docs/STEAM-FRAME.md ; https://github.com/iChris4/dolphinXR/issues/7
- Community field notes: https://github.com/saphid/frame-control
- Igalia / Phoronix / vr.org on Turnip: https://www.igalia.com/chats/steamy ; https://www.phoronix.com/news/Steam-Frame-Turnip-Vulkan ; https://vr.org/articles/steam-frame-steamos-android-handhelds-rocknix-lepton-2026
- ARMSX2 performance: https://heldgames.com/guides/armsx2-2-6-9-handheld-performance ; https://www.gamingonlinux.com/2026/07/playstation-2-emulator-for-arm-devices-armsx2-gets-an-initial-linux-release/
- Reviews and teardown: Road to VR, Engadget, iFixit via HotHardware (URLs above)
- Hardware: Wikipedia and UploadVR (URLs above)

### Risks

- The on-device OpenXR extension list is unverified. The list in this report comes from strings in SteamVR's PC vrclient binary, not from an enumeration on the Frame, so XR_KHR_composition_layer_cylinder is probably missing and the flat-screen layer will fall back to quads. Run xrEnumerateInstanceExtensionProperties on the device first.
- SteamVR-for-Linux #989: a second XrSession with a new VkDevice on the same XrInstance hard-aborts the process. Any ARMSX2 GS device restart (renderer change, VM reboot) must tear down and recreate the whole XrInstance. That takes about 2.1 s, and the app flashes to Theater mode each time.
- SteamVR-for-Linux #975: quad layers always sample array layer 0. ARMSX2's 2-layer per-eye targets need a separate swapchain per eye, or verification on the device that projection layers honour imageArrayIndex (see also openvr#1822 on array-swapchain layouts).
- The implicit XR_APILAYER_VALVE_fdm_injection layer is active for every OpenXR app. It hangs vkCreateDevice outside Steam in reported Proton cases, and it may inject fragment density maps into PCSX2's offscreen render passes. Launch through Steam, or test with DISABLE_VULKAN_FDM_INJECTION_LAYER=1.
- The Gamescope WSI Vulkan layer crashes or breaks apps that create swapchains outside gamescope (mGBA, Handy). ARMSX2's Qt flat window may need DISABLE_GAMESCOPE_WSI=1 when started from the desktop. There is no native OpenGL (zink only), so Qt and ARMSX2 must avoid GL paths.
- glibc baseline: the image ships glibc 2.39, which matches exactly the ubuntu-24.04-arm runner in outbreak-vr.yml. A newer runner image would produce an AppImage that fails to start. Cross-building against the Sniper arm64 sysroot (the Star Fox approach) is safer.
- Kernel page size is not published. The CI hardcodes HOST_PAGE_SIZE=4096. 4K is very likely because FEX requires it, but confirm with getconf PAGESIZE before relying on it.
- The devkit runtime differs between sources: Valve's docs say Steam Linux Runtime 3.0 ARM64 (Sniper), while the community found the alias SteamLinuxRuntime_4-arm64 and aarch64 titles launched natively without the container. If Valve starts enforcing the container, FUSE-mounted AppImages will fail. Ship the extracted AppDir.
- Thermal and battery: the SoC runs at roughly 10 W sustained and the battery is 21.6 Wh, giving about 1-1.5 h standalone. Stereo GS rendering at 72-90 Hz on top of SteamVR's compositor and tracking may push the SoC into throttling. Sustained clocks are unpublished.
- Development hazard: repeated short or failed SteamVR runs trigger SteamOS's steamvr-health-check, which can wipe ~/.local/share/Steam and has caused boot loops (community report). Do not script SteamVR kill/restart loops on the device.
- Bringing your own Vulkan driver is undocumented and risky. The system already uses Valve's Turnip build (Mesa 26.3-devel), and ARMSX2's adrenotools custom-driver path is Android-only. SteamVR compositor interop with a different Mesa build is untested.
- Every PS2-on-Frame performance figure here is extrapolated from Android 8 Gen 2/3 reports and ARMSX2 handheld numbers. Nobody has published ARMSX2 or PCSX2 running on a Frame, and some sources are low-quality SEO guides.

### Open questions

- What does xrEnumerateInstanceExtensionProperties actually return on the device? In particular: cylinder, XR_EXT_eye_gaze_interaction for native Linux apps, XR_FB_display_refresh_rate (full 72-144 list vs current rate only), XR_EXT_frame_synthesis and XR_KHR_composition_layer_depth.
- Do projection layers with an arraySize-2 swapchain (imageArrayIndex 0/1) render correctly on the Frame, or does the quad-layer bug in #975 also affect projection layers?
- What does getconf PAGESIZE report on the Frame, and does the kernel expose /dev/kgsl-3d0 or only the upstream msm DRM render node?
- Does a devkit title registered with SteamLinuxRuntime_4-arm64 always run outside the container (so the AppImage's FUSE mount works), or only in the build the community tested?
- Which microSD filesystem does the Frame format to (ext4 like the Deck?), and where does it mount?
- Will Alex deploy through the SteamOS Devkit Client from his Windows PC (it needs Developer Mode and pairing), or through SSH/rsync plus a desktop launcher?
- What are the sustained CPU and GPU clocks under combined PS2 emulation and SteamVR load, and when does throttling start? Valve's Record VR Performance CSV can answer this once a build runs.
- Does the ARMSX2 Qt AppImage start cleanly in the Frame's nested Plasma desktop and as a Steam-launched gamescope panel, or does it hit the Gamescope WSI layer or GL issues?

## 4. Milestone M0 brief (ready to hand to an engineer)

M0: OPENXR SESSION IN ARMSX2 ON THE STEAM FRAME, GAME ON A FLAT SCREEN, PLAYABLE WITH FRAME CONTROLLERS

GOAL
Produce one CI-built Linux arm64 build of ARMSX2 (D:\dev\ARMSX2, branch outbreak-vr). Launched on the Frame as a devkit title with --vr, it must:
- open an OpenXR session on SteamVR's linuxarm64 runtime;
- show Outbreak on a fixed flat screen in front of a seated player;
- take Frame controller input through PenguinScreen2's VR Gamepad mapping.

Also ship a small standalone probe, frame-xrprobe, that answers every open runtime question in one headset session.

Out of scope for M0: the first-person camera, stereo, hands, HUD, pouches and the Qt settings page.

GROUND RULES
- Repository and branch:
  - Work in D:\dev\ARMSX2 on outbreak-vr (HEAD 5fc05a01a9; remote beadbeed/ARMSX2).
  - Commit straight to outbreak-vr in a few batches, each of which compiles. A new branch cannot use outbreak-vr's dependency cache, so its first build would rebuild Qt 6.11.2 from source.
  - outbreak-vr.yml cancels a running build when you push again, so batch your pushes.
  - Commits use the beadbeed noreply identity and the attribution lines.
- Source of the VR layer:
  - D:\dev\PenguinScreen2 at 97bd99c (branch pc-firstperson). Read-only: never edit it from this task.
  - Do not git-merge or cherry-pick. PenguinScreen2 commit 990b456 stripped comments from 938 files, which makes three-way merges noise.
  - Copy pcsx2/VR files whole. Port hooks by reading PenguinScreen2's code at the line numbers below and inserting it at ARMSX2's matching spots.
- Guard rule:
  - Every hook added to a shared ARMSX2 file goes under #ifdef ENABLE_OPENXR, not ENABLE_VR.
  - Reason: ARMSX2 defines ENABLE_VR on every platform whenever ENABLE_VR_STEREO is ON (the default), including Android, iOS, macOS and Windows, none of which have OpenXR.
  - ENABLE_OPENXR defaults OFF. Only outbreak-vr.yml turns it on.
- Data: never put a BIOS, disc image, memory card, savestate or GS dump into the repo, CI, artifacts or any upload.
- Testing: there is no local ARM device. Verify through CI (ubuntu-24.04-arm, clang-17, glibc 2.39, which matches the Frame image) and one headset session by Alex.

STEP 0: BASELINE (30 MIN)
- Run: gh run list -R beadbeed/ARMSX2 --workflow outbreak-vr.yml -L 5
- outbreak-vr builds green: run 37707272283 on 4d22c34 (2026-10-08). The later 5fc05a0 only adds OUTBREAK-VR.md (ignored by the workflow's paths-ignore).
- Note how long a cached build takes.

STEP 1: FRAME-XRPROBE (SHIP THIS IN THE FIRST PUSH SO ALEX CAN RUN IT WHILE THE PORT CONTINUES)

Build and packaging:
- Location: tools/frame-xrprobe/frame-xrprobe.cpp, a single C++20 file of about 600-900 lines.
- CMake: built when ENABLE_OPENXR AND LINUX; links the vendored static openxr_loader (Step 2) and dl. Load libvulkan.so.1 with dlopen and get vkGetInstanceProcAddr from it, using the 3rdparty/vulkan headers.
- Install it as usr/bin/frame-xrprobe in the AppDir.
- Arguments: --no-session, --seconds-per-phase N (default 8), --log-dir DIR (default $HOME/frame-probe).
- Log to DIR/probe-YYYYmmdd-HHMMSS.log and mirror to stderr. The Frame does not keep stdout.

Host section, logged:
- uname
- sysconf(_SC_PAGESIZE)
- gnu_get_libc_version()
- CPU count and models
- the environment variables XDG_*, XR_*, STEAM*, DISABLE_*, VK_*, WAYLAND_DISPLAY, DISPLAY

OpenXR section, logged:
- Every active_runtime file that exists, with its contents: XR_RUNTIME_JSON, $XDG_CONFIG_HOME/openxr/1/active_runtime(.aarch64).json, ~/.config/openxr/1/..., /etc/xdg/openxr/1/...
- API layers, and instance extensions with versions. If the runtime is unavailable, log that and exit 0.
- Create the instance with XR_API_VERSION_1_0 and XR_KHR_vulkan_enable2, plus each of these that is offered: XR_KHR_composition_layer_cylinder, XR_VALVE_frame_controller_interaction, XR_FB_display_refresh_rate, XR_KHR_composition_layer_depth, XR_EXT_eye_gaze_interaction. Time xrCreateInstance.
- Log the runtime name and version, and the system properties: maxLayerCount, max swapchain size, tracking.
- Log the stereo view configuration views (recommended and max sizes) and the blend modes.

Vulkan section, logged, using the same path as PenguinScreen2 XRSession.cpp (around lines 360-392):
- xrGetVulkanGraphicsRequirements2KHR.
- xrCreateVulkanInstanceKHR (Vulkan 1.1).
- xrGetVulkanGraphicsDevice2KHR.
- Device name, driverID, driverInfo, apiVersion and conformance version.
- Every queue family with its flags and queueCount.
- The multiview feature.
- xrCreateVulkanDeviceKHR with multiview on, asking for 2 queues on the graphics family when it has them. Log whether 2 were available; that decides between the shared-queue lock and a dedicated XR queue later.

Session section (skipped with --no-session):
- xrCreateSession with XrGraphicsBindingVulkan2KHR, queueIndex 0.
- Which reference spaces exist (LOCAL, STAGE, VIEW).
- Every swapchain format offered. Pick R8G8B8A8_SRGB, then R8G8B8A8_UNORM, as XRCompositor does.
- Refresh rates (xrEnumerateDisplayRefreshRatesFB and the current rate).
- One action set with a trigger action and an aim-pose action. Suggested bindings, in this order: /interaction_profiles/valve/frame_controller_valve, oculus/touch_controller, khr/simple_controller. After FOCUSED, log xrGetCurrentInteractionProfile for both hands.
- Log every session state change with a timestamp.

Frame loop phases (log each phase's start time, frame count, predictedDisplayPeriod statistics, xrEndFrame errors, and time spent in vkQueueSubmit and in xrEndFrame):
- Phase A: one projection layer, one swapchain with arraySize 2. imageArrayIndex 0 (left eye) is cleared to solid RED, index 1 (right eye) to solid BLUE, using vkCmdClearColorImage per layer.
- Phase B: one projection layer, two swapchains with arraySize 1. Left eye GREEN, right eye MAGENTA.
- Phase C: a black projection background plus a quad layer 2 m ahead, 1.6 x 0.9 m, solid YELLOW.
- Phase D, only if the cylinder extension is present: the same with a CYAN cylinder layer.

Exit:
- On STOPPING, call xrEndSession, then destroy the session, then destroy the instance. Never create a second session on the same instance (SteamVR-for-Linux issue #989 aborts the process).
- Then exit 0.

STEP 2: BUILD GLUE
1. Copy D:\dev\PenguinScreen2\3rdparty\openxr to D:\dev\ARMSX2\3rdparty\openxr unchanged (Khronos SDK 1.1.61, with its LICENSE files).
2. cmake/BuildParameters.cmake:
   - Next to ENABLE_VR_STEREO (line 48), add: option(ENABLE_OPENXR "OpenXR headset session (Steam Frame)" OFF)
   - When it is ON, stop with FATAL_ERROR unless ENABLE_VR_STEREO and USE_VULKAN are on and the platform is neither Apple nor Android.
   - Then add list(APPEND PCSX2_DEFS ENABLE_OPENXR), next to the ENABLE_VR define at line 284.
3. cmake/SearchForStuff.cmake:
   - After add_subdirectory(3rdparty/vulkan) (line 187), add PenguinScreen2's static-loader block (its SearchForStuff.cmake lines 113-128) inside if(ENABLE_OPENXR).
   - That block sets DYNAMIC_LOADER OFF and API layers and tests OFF, calls add_subdirectory(3rdparty/openxr EXCLUDE_FROM_ALL), and applies target_compile_options(openxr_loader PRIVATE -fexceptions -frtti).
4. pcsx2/CMakeLists.txt:
   - Inside if(ENABLE_OPENXR), add PenguinScreen2's pcsx2VRSources and pcsx2VRHeaders lists (its pcsx2/CMakeLists.txt lines 1063-1121). Leave out StereoState, which ARMSX2 already builds in pcsx2GSSources around line 790.
   - Add them with target_sources(PCSX2 PRIVATE ...) outside the LTO list, and link openxr_loader into PCSX2_FLAGS.
   - Copy the exception island (PenguinScreen2 lines 1539-1555): VR/XRSession.cpp and VR/SeatSession.cpp get COMPILE_OPTIONS -fexceptions and SKIP_PRECOMPILE_HEADERS ON.
5. .github/workflows/outbreak-vr.yml: add -DENABLE_OPENXR=ON to cmakeflags.

STEP 3: COPY THE VR LAYER
- Copy every file in D:\dev\PenguinScreen2\pcsx2\VR\ into D:\dev\ARMSX2\pcsx2\VR\.
  - This overwrites StereoState.h/.cpp with PenguinScreen2's version, which has the same functions plus first_person, near_floor, HeadRoll, RollParallax and DumpAllowed. I checked that every ARMSX2 StereoState function exists in it.
  - Keep ARMSX2's StereoSettings.h/.cpp. In M0 it stays the only code that writes StereoState, and GSRenderer, GSRendererHW, GSTextureCache and VMManager line 737 still call it.
- Copy pcsx2/Input/VRInputSource.cpp/.h.
- Copy bin/resources/vr-profiles (5 YAML files plus README and LICENSE). appimage-qt.sh already copies bin/resources whole.
- Leave SeatSession and MemWatch as they are. SeatSession compiles on Linux and only runs with --vr-seat. MemWatch stays inactive without an arm64 memcheck.
- Expect fixes for clang-17. Waves 4-7 have only ever been compiled with MSVC, so expect missing includes, narrowing conversions and designated-initializer order. Keep each fix minimal.
- Write pcsx2/VR/VR-SOURCE.md containing:
  - the source repo and commit;
  - the list of copied files;
  - a table of every local edit (file, line, reason), to be ported back to PenguinScreen2 later;
  - a table of hook sites: PenguinScreen2 site, ARMSX2 site, and status (ported in M0, or deferred to which milestone).

STEP 4: CORE SHIMS
- VMManager::IsOnlineLocked(): declare it in pcsx2/VMManager.h and copy PenguinScreen2's body (VMManager.cpp lines 332-342): EmuConfig.DEV9.EthEnable, or PCSX2_TEST_ONLINE_LOCKS=1. Do not port the limiter locks yet.
- Config:
  - Copy Pcsx2Config::VROptions (PenguinScreen2 Config.h around line 1282; about 35 fields).
  - Copy its LoadSave (Pcsx2Config.cpp around lines 1925-1960; ini section "VR"), operator== and !=, and the VR member of Pcsx2Config.
  - Copy EmuFolders::VRProfiles (LoadPathFromSettings "VRProfiles"/"vrprofiles", around line 2362) and add its folder creation.
  - The ini keys do not clash with StereoSettings' keys (StereoEnable, Separation, Convergence, ZDrivenDepth, PinUniformQ, CollimateDisparity, InterleaveEyes, DebugView), so both can live in [VR] for now.
- GSDevice::ShadeBoost(const GSVector4& params): add this overload next to ARMSX2's ShadeBoost(), copied from PenguinScreen2 GSDevice.cpp lines 1049-1070. It runs per layer through GetLayerProxyTexture, which ARMSX2 has.

STEP 5: HOOKS (ALL UNDER #ifdef ENABLE_OPENXR)

GSDeviceVK.cpp (pcsx2/GS/Renderers/Vulkan):
- The seven bootstrap sites already exist (lines 43, 239, 1011, 3555, 4128, 4204, 4261) and compile once VRVulkanBridge.h exists.
- Compare each one with PenguinScreen2's: CreateVulkanInstance around 136, CreateDevice around 666, Destroy around 2614, Create around 2974, the physical-device override around 3035, OnVulkanDeviceCreated around 3085. OnGSDeviceDestroyed must run before the device is destroyed.
- Add VR::ScopedXrQueueLock exactly as PenguinScreen2 does at its lines 1138, 1328 and 2024:
  - WaitForGPUIdle (ARMSX2 line 1750);
  - SubmitCommandBuffer, held from vkQueueSubmit (line 2118) through vkQueuePresentKHR (line 2158), using std::optional<VR::ScopedXrQueueLock>;
  - SubmitSpinCommand (line 2925, m_spin_queue).
- On Turnip the present and spin queues are probably the same VkQueue as the graphics queue, and the runtime uses that queue too.
- Frame generation: when VR::LaunchRequestedVR() is true, force LSFG (GSLsfg.cpp) and FrameGen off where they are turned on, and log it once. Do not add locks inside them.

GSRenderer.cpp (pcsx2/GS/Renderers/Common):
- VR::PresentationLift, then ShadeBoost(lift) (PenguinScreen2 line 239).
- VR::EnsureFrameSubmitted in the frame-skip path when the frame is not blank (PenguinScreen2 626; ARMSX2 around 1158, which has an extra skip_blank case).
- VR::EndOfFrame, then StereoDump::MaybeCapture, after the present (PenguinScreen2 696; ARMSX2 around 1341).
- VR::EndOfFrame in PresentCurrentFrame (PenguinScreen2 956; ARMSX2 around 1569).
- The PCSX2_VR_SNAPSHOT_LAYER hook waits until M1.

VMManager.cpp:
- VR::UpdateSettings in CPUThreadInitialize (PenguinScreen2 437) and UpdateDiscDetails (1125).
- In CheckForConfigChanges: if (EmuConfig.VR != old_config.VR) VR::UpdateSettings() (PenguinScreen2 3113).
- VR::ClearSpatialControls in ClearDiscDetails (1155).
- CameraDriver::OnStateLoaded and GameFeedback::OnStateLoaded in DoLoadState (1864).
- Do NOT add the VSyncOnCPUThread block (CameraDriver::Apply, GameFeedback::Poll, HudCards::Poll, ApplySceneStereo, SplitState::Apply). That is M1.

InputManager.cpp/.h (pcsx2/Input), 11 guarded sites and about 105 lines in PenguinScreen2:
- InputSourceType::VR, plus every name or table indexed by that enum.
- VRInputSource registration.
- The VR binding overlay merged into binding parsing.
- Rumble hooks: SetPadVibrationIntensity calls GameFeedback::OnPadRumble, and PauseVibration calls PauseRumble.

pcsx2-qt/QtHost.cpp:
- --vr and -vr: VR::SetLaunchRequestedVR(true).
- -vr-info: PrintCommandLineVersion, print VR::GetRuntimeInfoReport(), then exit (PenguinScreen2 line 2228).
- Help text.
- The VR::ProfileDB::ValidateAtLaunch dialog (PenguinScreen2 line 2527).
- The OnlineSafe rule: no pause when a VR controller disconnects (PenguinScreen2 line 1763).
- Skip --vr-seat, --vr-cast, the qhist arguments, and the SettingsWindow/VRSettingsWidget page.

Deferred: PadDualshock2, Hotkeys, PINE 0xE0-0xE5, the first-person changes in GSRendererHW, GSTextureCache and tfx.glsl, and gsrunner. List them in VR-SOURCE.md.

STEP 6: PACKAGING AND LAUNCHER

AppDir artifact:
- In appimage-qt.sh, gated by an input or env var set only by outbreak-vr.yml so build-all is unchanged, also produce outbreak-vr-frame-<sha>.tar.zst containing the AppDir and the frame/ files.
- Upload it as an artifact. Devkit uploads use the extracted AppDir, so FUSE is not needed and it still works if Valve later runs titles inside the runtime container.

frame/launch-outbreak-vr.sh (the devkit Start Command):
- Logging:
  - Send all output to $HOME/armsx2-vr/logs/launch-<timestamp>.log.
  - Log the date, uname -a, getconf PAGESIZE, the first line of ldd --version, /etc/os-release, id, and the same environment variables the probe logs. This shows whether the title ran inside a container.
- Environment:
  - export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
  - FRAME_NO_FDM=1 exports DISABLE_VULKAN_FDM_INJECTION_LAYER=1.
  - FRAME_NO_GSWSI=1 exports DISABLE_GAMESCOPE_WSI=1.
  - Alex sets these through Steam launch options, for example: FRAME_NO_FDM=1 %command%
- First run, only if $HOME/.config/ARMSX2/inis/PCSX2.ini does not exist (QtHost.cpp line 1325 uses EmuFolders::Settings/PCSX2.ini):
  - Write a minimal ini with [UI] SetupWizardIncomplete=false, so the first-run setup wizard does not open inside the headset.
  - [EmuCore/GS]: Renderer=14 (Vulkan), upscale_multiplier=1.
  - [EmuCore/Speedhacks]: vuThread=true.
  - [VR]: Enable=true, HeadCamera=false, StereoMode=false, HeadsetRateSubmit=true. HeadCamera and StereoMode both default to true, so they must be turned off for M0.
  - Check that, with this ini, the VR binding overlay still binds the pad and the BIOS folder still resolves to ~/.config/ARMSX2/bios.
  - Later runs leave the ini alone.
- Game:
  - Take the game path from $1, or else from the first line of $HOME/armsx2-vr/game.txt.
  - If the game or the BIOS is missing, log a clear message and exit 2.
- Run: exec the AppRun or usr/bin/armsx2-qt with --vr -fullscreen -batch -- "$GAME". All three flags exist in ARMSX2's QtHost.

frame/frame-probe.sh: same logging, then runs usr/bin/frame-xrprobe.

frame/README-FRAME.md: the steps for Alex (deploy, launch options, where the logs are, the test checklist).

STEP 7: HEADSET-FREE CHECKS IN CI
Add a new job, frame-smoke, to outbreak-vr.yml: runs-on ubuntu-24.04-arm, needs frame-linux-arm64, downloads the artifact. It checks:
1. QT_QPA_PLATFORM=offscreen <AppDir>/AppRun -vr-info exits, and its output contains the version line and 'No OpenXR runtime is available'. This proves the static loader and the VR layer link and start on aarch64.
2. frame-xrprobe --no-session --log-dir $RUNNER_TEMP exits 0, and the log shows page size 4096 and 'runtime unavailable'.
3. No ELF in the AppDir references a GLIBC_ symbol version above 2.39 (check with objdump -T).
4. usr/bin/resources/vr-profiles/SLPM-65428.yaml and SLPM-65692.yaml exist.
5. bash -n passes on the frame/*.sh scripts.
6. Stretch goal, capped at half a day: build Monado from a pinned release with its simulated HMD and null compositor, install mesa-vulkan-drivers (lavapipe), and run frame-xrprobe with --seconds-per-phase 3. It must reach a running session and exit 0. If this fails, drop it and note why in VR-SOURCE.md.

Also add tools/outbreak-vr/hook-audit.sh. It fails if any line added since 3cf66e03aa in shared files (outside pcsx2/VR, 3rdparty/openxr, VRInputSource and tools/frame-xrprobe) references VR:: or OpenXR without being under #ifdef ENABLE_OPENXR. ARMSX2's existing ENABLE_VR stereo code is exempt.

DONE WHEN: ENGINEER (NO ALEX)
- The outbreak-vr 'Steam Frame' build and frame-smoke are green on the same commit.
- The artifact contains the AppImage, plus the AppDir tarball with launch-outbreak-vr.sh, frame-probe.sh, frame-xrprobe and README-FRAME.md.
- hook-audit passes, and ENABLE_OPENXR still defaults to OFF.
- VR-SOURCE.md is complete: copied files, local edits, hook-site table.
- OUTBREAK-VR.md has a 'Steam Frame: M0' section with status, deploy steps, launch options, log locations and the test checklist, and no game data.
- The clang fixes are listed for porting back to PenguinScreen2.

DONE WHEN: ALEX ON THE FRAME (ONE SESSION OF ABOUT 45 MINUTES, HEADSET ON, 45 W CHARGER CONNECTED)

Probe:
- Launch 'Devkit Game: frame-xrprobe'.
- Answer: Phase A, left eye red and right eye blue (or the same colour in both eyes)? Phase B, green and magenta? Phase C, a yellow rectangle ahead? Phase D, if it runs, a cyan curved screen?
- Copy ~/frame-probe/*.log back to the PC over the LAN.

Game:
1. 'Devkit Game: Outbreak VR' launches.
2. After a brief flash of SteamVR's flat Theater view, the game appears on a flat screen about 2 m ahead and stays put when he turns his head.
3. The title screen comes up. The Frame controllers work the menus and move the character.
4. The OSD shows at least 95% speed for 5 minutes in a room at 1x.
5. Quitting returns to SteamVR without a crash.
6. He copies back ~/armsx2-vr/logs/* and the files in ~/.local/share/Steam/logs (xrclient_*.txt, vrcompositor and vrserver logs). Recording a Record VR Performance CSV is optional.
- Pass: points 1-5 hold, and the log shows no '(VR) ... abandon' and no XrQueueLock timeouts.

DECISIONS THE PROBE SETTLES
- XR_KHR_vulkan_enable2 missing (not expected): add a vulkan_enable (v1) fallback to XRSession.
- Phase A shows one colour in both eyes: M1 must use two per-eye swapchains. PenguinScreen2 already uses arraySize 1 everywhere.
- No cylinder extension: quad fallback, which is already in XRSession and XRCompositor.
- Two or more graphics queues: keep the shared queue for M0 and consider a dedicated XR queue in M4.
- maxLayerCount below 16: lower kLayerCapacity before M3.
- Page size 16K: switch outbreak-vr.yml to -DHOST_PAGE_SIZE=16384 (build-all.yml already has a 16K variant).
- Session never reaches FOCUSED: check the headset standby setting; try SteamVR's 'Launch a program' list or Konsole in desktop mode.
- vkCreateDevice hangs: launch with FRAME_NO_FDM=1.
- Qt window crashes or appears as an extra panel: launch with FRAME_NO_GSWSI=1, or add a VR-only Surfaceless mode.

KNOWN TRAPS
- SteamVR-for-Linux #989: a new VkDevice on the same XrInstance aborts the process. Every GS device restart must tear down the whole instance; VRVulkanBridge already destroys the session and instance together. Expect about 2.1 s of xrCreateInstance and a flash of the flat Theater view. Do not switch renderers during the test.
- Never script SteamVR kill-and-restart loops on the device. Three or more failed runs in one boot trigger steam-health-check, which wipes ~/.local/share/Steam.
- The headset must be worn, or standby defeated, before a session goes past VISIBLE.
- The devkit title folder is deleted by steamos-delete, so keep the BIOS, games and settings under ~/.config/ARMSX2 and ~/armsx2-vr.
- Moving the runner to a newer Ubuntu breaks the glibc 2.39 match with the Frame image.

EFFORT
4-6 engineer days, mostly waiting on CI. Push the probe first.
Alex: about 30 minutes of one-time setup, and the one test session above (the probe part can happen early, as soon as Step 1 is green).
