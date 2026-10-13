# Why SteamVR's xrEndFrame blocked a full frame for two minutes (research, 2026-10-12)

Question: in Alex's first headset session the pacer's `xrEndFrame` took about 0.8 ms normally, but about 10.7 ms
(one 90 Hz period) on every frame from about 64 s to 190 s, right after the session went FOCUSED -> VISIBLE -> FOCUSED.
It ended by itself with nothing logged. Why does SteamVR do that, and what should the pacer do about it?

Read-only: no code changed, no SteamVR setting changed, emulator and SteamVR not run. The scripts I used to parse the
logs are in this session's scratchpad, not the repo.

## Short answer

1. **Most likely cause: SteamVR (its compositor, and/or the Steam Link "vrlink" driver that feeds the Steam Frame)
   changed its frame-timing mode for the app when the app lost focus to the dashboard, and kept it for about two
   minutes.** In that mode the runtime lets `xrWaitFrame` return at once and makes `xrEndFrame` wait one display period
   instead. The same runtime parks an unfocused or standby app in `xrEndFrame` too (100 ms there, 10 Hz), so blocking in
   `xrEndFrame` is how this runtime throttles an app, not a one-off. The log shows no late frames, no reprojection and
   no event at the end. I cannot see inside the runtime, so the exact rule is inferred (section 3).
2. **Our code did not cause the plateau, but it makes it expensive and may keep the runtime from relaxing.** The pacer
   holds the shared Vulkan queue lock across `xrEndFrame`, so queue-lock waits (summed over all waiters, mostly the GS
   thread) rose from 3.7 ms/s to 576 ms/s and GS frames fell from 59.8 to 57.6 per second (41 - 43 in the second plateau).
   GS-driven image releases that are not tied to `xrWaitFrame`, and a layer array that changes every few seconds
   (150 SteamVR shared-texture re-creations in 11 minutes), are the two things in our submission pattern that look like
   "late or irregular frames" to a runtime.
3. **Our swapchain handling is not the cause.** No XR errors or warnings, no swapchain timeouts, no reprojection. Reusing
   a released image across frames is legal. Late GPU work on the queue does not fit either: the block is the same length
   every frame whatever the GS load.
4. **A mistake in our own stats:** `wait->begin` is measured to the moment *before* `xrBeginFrame` is called, so it can
   never show `xrWaitFrame` or `xrBeginFrame` blocking. "wait->begin 0.0" proves nothing. The 90 frames/s arithmetic is
   what shows that the wait moved into `xrEndFrame`.

## 1. What the logs show

Clock: the emulog's `t` is seconds since start. I fitted it to wall time from five session-state lines that SteamVR
logs with a time stamp: `t = 0` is 18:43:41.43 local, good to about 0.1 s.

### Our Frames lines (`emulog-headset-20261012.txt`)

| Period (t, wall time) | State | xrEndFrame mean (max) | Queue-lock wait total | GS frames/s |
|---|---|---|---|---|
| 19 - 63 s (18:44:00 - 18:44:45) | normal | 0.7 - 0.9 ms (1.3 - 2.5) | 1.5 - 10 ms/s | 59.9 |
| 63.3 - 189.3 s (18:44:45 - 18:46:51) | **plateau 1**, 126 s | 10.56 ms avg over 24 windows (11.2 - 11.9 typical) | 576 ms/s avg (361 - 789) | 57.6 avg (49.9 - 59.6) |
| 189.3 - 635.1 s (18:46:51 - 18:54:16) | normal | 0.83 ms avg over 98 windows | 3.7 ms/s avg | 59.8 |
| 635.1 - 651.2 s | **plateau 2**, dashboard open the whole time | 10.6 - 10.8 ms | 556 - 746 ms/s | 41 - 43 |
| 651.3 s to the end of the log (t = 984, 19:00; the app ran until 19:35:37) | SYNCHRONIZED, headset in standby | 100.1 - 100.4 ms | about 590 ms/s | 41 - 50 (6.4 submits/s) |

Details worth knowing:

- Submitted frames stay at 89.9 - 90.2/s through plateau 1. `display 90.0 Hz` never changes. `shouldRender=false` stays 0.
- Period budget: 11.1 ms per frame. In plateau 1, `xrEndFrame` is 10.56 ms, leaving 0.55 ms for `xrWaitFrame`,
  `xrBeginFrame` and our pose queries. In the normal state `xrEndFrame` is 0.83 ms and the other 10.3 ms is
  `xrWaitFrame` (or `xrBeginFrame`) blocking. So the pacing block moved from the wait into `xrEndFrame`.
- The plateau is very flat: max is only about 0.6 ms above the mean. That is a wait for a fixed point in the frame
  schedule, not work taking time. Work (GPU copies, lock contention) would vary with GS load, and it did not.
- Onset: the 5 s window ending 64.68 has mean 3.5 ms, max 100.2 ms, 87.6 frames/s. Working back from the mean, the plateau
  starts at about t = 63.3, the moment of FOCUSED -> VISIBLE (63.33), not when focus came back (64.26). One frame took
  100 ms at that moment. In plateau 2 the window ending 635.18 (FOCUSED -> VISIBLE at 635.08) is still normal, and the next
  window is fully slow, so onset is within about 0.1 s of the focus loss again.
- End: the window ending 189.775 has mean 9.7 ms (90% slow frames), so the plateau ended at about t = 189.3
  (18:46:50.7).
- The second FOCUSED -> VISIBLE -> FOCUSED blip (110.6 - 112.1, 1.5 s) happened inside plateau 1 and changed nothing
  visible. The window means around it (8.9 and 9.0) suggest about 1.75 s of fast frames, ending right at that blip
  (roughly 108.8 - 110.55). That lines up with the Steam client's UI process reconnecting to the compositor at
  18:45:30.05 (t = 108.6) and its overlays being recreated. This is an inference from two bucket means, not a measurement.
- The startup case did **not** produce a plateau: the session came up unfocused (dashboard open since 18:43:31.6) and went
  VISIBLE -> FOCUSED at t = 12.8 (18:43:54.2); by t = 19.6 `xrEndFrame` was 0.8 ms. So "dashboard closed" does not always
  trigger it. Focus *loss* during steady running did, both times it happened outside startup.
- No XR errors, no `xrWaitSwapchainImage` timeouts, `copies timed out 0` throughout.

### What SteamVR logged (`D:\programfile\steam\logs`, runtime at `E:\ProgramFile\Steam\steamapps\common\SteamVR`)

The headset is a **Steam Frame ("Deckard MP") streamed over Steam Link (driver `vrlink`)**, not a direct-mode HMD. SteamVR
2.17.10. App key: `system.generated.openxr.penguinscreen2.pcsx2-qt.exe`.

- **Plateau 1 start.** 18:44:44.617 `vrserver`/`driver_vrlink`: `received EVTYPE_OPEN_DASHBOARD` (Steam button on the Frame).
  18:44:44.664 `xrclient_pcsx2-qt`: FOCUSED -> VISIBLE. 18:44:44.664 - .799 `vrclient_vrcompositor`: system behaviour flags
  `LaserMouse`, `DriverIsDrawingControllers`, `DriverRequestsApplicationPause`, `DriverRequestsReducedRendering` added.
  18:44:45.67 - .69: all removed, VISIBLE -> FOCUSED. The dashboard log shows `ShowDashboardOverlay ... bSourceIsVRLinkRemote true`
  then `HideDashboard`.
- **Plateau 1 end (18:46:50.7): nothing.** No line in `vrserver`, `vrcompositor`, `driver_vrlink`, `xrclient`, `vrmonitor`, or
  the other Steam logs. Closest: two shared-texture creations at 18:46:49.3 (t = 187.9).
- **Plateau 2 start.** 18:54:16.485 `EVTYPE_OPEN_DASHBOARD`, flags added 18:54:16.52, FOCUSED -> VISIBLE 18:54:16.510. No
  matching "closed" this time. 18:54:27.9 Steam Link rescans its links, 18:54:31.6 `Connection inactive`, 18:54:32.684
  VISIBLE -> SYNCHRONIZED, 18:54:37.6 `entering standby`.
- **Compositor totals at exit (19:35:37):** `Total 65480 presents. 8185 dropped. 0 reprojected.` `ApplicationTime CPU 0.621 ms /
  GPU 2.878 ms`. The standby period alone (about 3.2 compositor timeouts/s for 2466 s) explains about 7,900 of the 8,185
  dropped, leaving about 290 for the other 11 minutes. **Neither plateau made the compositor drop or reproject frames.**
- `vrcompositor.txt` has `WaitForAcquire timed out (FAILED); rendering the next frame before the driver took the sync
  texture` every ~310 ms during standby (the vrlink driver stopped taking frames: that is the 10 Hz park). One earlier line:
  18:48:51.255 `[Compositor] AcquireSync FAILED with WAIT_TIMEOUT` and `ReleaseSync FAILED with unknown hr 0x887a0001`
  (see 3.2: it lines up with our one big normal-state `xrEndFrame` spike, 14.7 ms at t = 309.9).
- `driver_vrlink.txt`: 18 `THROTTLE EVENT: <us>` lines. Nine are in the standby period (300 - 311 ms). The other nine (one
  59 ms at startup, the rest 11 - 23 ms, i.e. one or two display periods) are each followed by an NVENC encoder reset and a "Bad
  link event" bitrate trim of about 1%. There were about a dozen encoder resets and several "frame ID was not what we expected"
  stream resets (18:45:32, 18:45:42, 18:48:59, 18:49:43, 18:53:21, 18:53:29). Link estimate 185 - 232 Mbit/s. **None falls on
  63.3, 189.3 or 635.1.** I do not think these are the plateau.
- No motion smoothing, throttling or prediction lines at default log level. `steamvr.vrsettings` is default for this app
  (no section for pcsx2-qt; global `motionSmoothing` not overridden), so no per-app setting is in play.
- `xrclient_pcsx2-qt.txt`: `Initialized Vulkan transfer ring buffer` (suggests the runtime copies our Vulkan images itself,
  on our queue), and `Created shared texture 'Scene create Vulkan, -417' 256x256` style lines, discussed in 3.2.
- The dashboard's web UI log names our quad layers: `[Overlays] Created: openxr_quadlayer_0` ... `_6`, destroyed at exit.
  So this runtime shows our quad layers as OpenVR overlays.

## 2. What SteamVR documents about this

Nothing I found describes our exact symptom, so these are building blocks, not a confirmed explanation.

- **`openvr.h`** (Valve, `headers/openvr.h`, the OpenXR runtime sits on this compositor): `Compositor_FrameTiming.m_nReprojectionFlags`
  carries `VRCompositor_PredictionMask` ("The runtime may predict more than one frame ahead if it detects the application is
  taking too long to render") and `VRCompositor_ThrottleMask` ("Number of frames the compositor is throttling the
  application"). `WaitGetPoses` docs: with `DoNotHaveFocus` "this will throttle the call to 10hz". Settings keys in the
  `steamvr` section: `framesToThrottle`, `additionalFramesToPredict`, `motionSmoothing`, `disableAsync` (that they have per-app
  copies is community-documented, see 4.6). A related struct field, `m_flSubmitFrameMs`, is "time spent in IVRCompositor::Submit
  (not near-zero indicates driver issue)".
- **Valve patent US 10,600,236** ("Prediction and throttling adjustments based on application rendering performance"): the
  compositor raises its prediction level by one step when 2 of the last 5 frames are late, can raise a throttle level
  independently, and lowers the prediction level only after 30 consecutive frames that finished early. Level changes clear the
  statistics. These are the patent's example numbers and it dates from about 2018; I do not know what 2.17 uses.
- **SteamVR release notes** (via GamingOnLinux): 2.10 "Improved automatic throttling behavior" ("reduces controller and
  physics jitter from throttling and prediction changes in response to performance variations") and "**Better handling of
  non-pipelined applications**" (the note says it reduces controller jitter for Unity OpenXR apps; it does not define the term).
  2.13, Steam Link: "New frame pacing ... Note this change does affect timing for VR
  applications on the host PC". 2.13, OpenXR: "Fixed a bug where apps that didn't submit projection layers would see a 100ms wait
  on alternating frames with some drivers." 2.17: "Fix an issue causing 1 frame to be dropped every few seconds", "Latency
  improvements", "Fix OpenXR bug with image submission for certain non-standard image sizes".
- **OpenXR spec**: `xrWaitFrame` is where throttling belongs; the runtime "may adjust its predictions and throttling based on
  submission and completion feedback from xrEndFrame"; `xrBeginFrame` "must not perform frame synchronization or throttling";
  `xrEndFrame` "may return immediately", so blocking there is allowed but not what the spec intends (a Khronos forum reply to a
  report of `xrEndFrame` blocking says the spec does not prohibit it but blocking for any significant time is not intended).
- **Valve wiki "SteamVR/Frame Timing"** returned HTTP 403 to my fetch, so I could not read it.

## 3. Likely causes, ranked

### 3.1 SteamVR timing mode change on focus loss (most likely)

Evidence for:

- Onset is within about 0.1 s of FOCUSED -> VISIBLE both times it happened in steady running (63.3, 635.1), and 100 ms-class
  events appear at the same moments (`xrEndFrame` max 100.2 ms at 63.3).
- Signature: `xrWaitFrame` returns at once, `xrEndFrame` holds one period, frame rate and display period unchanged, almost no
  spread. That is what a runtime does when it runs a non-pipelined app one frame deeper: the extra prediction depth has to be
  absorbed somewhere, and it lands in the submit call.
- The same runtime parks a not-visible app in `xrEndFrame` for 100 ms (our standby windows: mean 100.2 ms, 6.5 submits/s,
  which matches the documented 10 Hz throttle for an app without focus). The plateau looks like the same mechanism at one period
  instead of ten.
- It causes no drops and no reprojection (compositor totals), and the app's CPU and GPU times are tiny (0.62 ms / 2.9 ms), so
  this is not an overload response.
- It ended by itself with no log line, as an internal heuristic would. The patent's "30 consecutive early frames to step
  down" fits "recovered on its own", though not a precise 126 s.
- SteamVR has documented, changing logic for exactly this (prediction depth, throttle level, non-pipelined apps, and a Steam
  Link frame-pacing rewrite in 2.13 that Valve says changes host timing).

Evidence against / unknown:

- No log confirms a prediction or throttle change. I cannot say whether the compositor or the Steam Link pacer made it.
- The startup refocus did not trigger it (the compositor had no history yet at that point).
- A 126 s recovery is longer than the patent's rule alone would suggest, so either the rule differs now or something kept
  resetting the "early frames" run (see 3.2).

How to confirm cheaply: log the offset `predictedDisplayTime - steady_clock at xrWaitFrame return` (we already store it as
`display_clock_offset_ns`). If the runtime predicts one frame further out, this offset jumps by about +11.1 ms at 63.3 and
drops back at 189.3. A second check is reading `Compositor_FrameTiming` through OpenVR from a small background client in a
test session (prediction and throttle bits, `m_flClientFrameIntervalMs`, `m_flSubmitFrameMs`, `m_flPresentCallCpuMs`).

### 3.2 Our submission pattern looks irregular to the runtime (possible trigger, likely amplifier)

- **Layer-array churn.** SteamVR keys a shared texture per layer *position* (ids -385, -401, -417, -433, -449, -465, -481)
  and re-creates it when what sits there changes size. There were **150 re-creations in the first 11 minutes**; 127 of 143
  repeats for a slot were a size change (16x16, 16x256, 64x64, 256x160, 256x256, 1024x160, 1536x1344, 1920x1344 all appear). Our
  `BuildSubmitArray` drops absent layers, so every layer after a toggled one shifts position. In the normal state **every 5 s
  window that contains a re-creation has a max `xrEndFrame` of 3.8 - 14.7 ms (median 4.3, 50 windows); windows without one have
  a median max of 1.5 ms and never above 4.0 (38 windows)**. The single largest normal-state spike (14.7 ms, window ending t = 309.87) coincides with
  `AcquireSync FAILED with WAIT_TIMEOUT` in the compositor at 18:48:51.255 and two re-creations at .247 / .257. Inside plateau 1
  the same re-creations cost nothing extra (max stays 11.2 - 11.9), because the runtime was waiting anyway. A frame that takes 4 -
  15 ms in submit is exactly what a lateness heuristic counts.
- **Image readiness is not tied to the frame start.** The GS thread copies, releases and publishes at 60/s on its own clock;
  the pacer re-sends the last set 30 times a second. SteamVR's lateness logic is built around "work between `xrWaitFrame` and
  `xrEndFrame`". We do almost none of that on the pacer, so its view of our frame cost is unrepresentative. In the plateau,
  deferred releases rose from 0.4 to 5.7 per 5 s (up to 37) and skipped copies from 0.8 to 11 (up to 74), so images are
  released later in the plateau than outside it. That is a consequence, but it can also stop the "early frames" run from
  completing.
- **One frame in 100 ms at the focus change** (max 100.2 ms at 63.3). If `LocateEyeViews` fails during the transition,
  `views_ok` is false and that frame has no projection layer (only the opaque background is one). SteamVR 2.13 fixed "apps
  that didn't submit projection layers would see a 100ms wait" for some drivers, so a frame with no projection layer is worth
  ruling out. We do not log how often it happens.

### 3.3 Lock coupling (a consequence, and an amplifier)

`LockedBeginFrame` and `LockedEndFrame` take the global queue lock, and the pacer also holds `submit_mutex`. While it waits in
`xrEndFrame` the GS thread's acquire, copy submit, release, `vkQueueSubmit` and `vkQueuePresentKHR` all wait. Measured:
576 ms/s of lock wait in plateau 1 against 3.7 ms/s normal; GS 57.6 fps against 59.8; in plateau 2, 41 - 43 GS fps with 158 -
184 skipped copies and 79 - 92 deferred publishes per 5 s. The spec does require external sync of the VkQueue for
`xrBeginFrame`, `xrEndFrame`, `xrAcquireSwapchainImage` and `xrReleaseSwapchainImage` (the runtime may use the queue in those
four; `xrWaitFrame` and `xrWaitSwapchainImage` never), so the lock is correct, but it should not be shared with the GS.
A worktree already has this: `.claude/worktrees/agent-a2baabe2fcac60457`, commit `ad2d26a` ("opt-in dedicated Vulkan queue for
the OpenXR session"). It fixes the GS starvation, not the length of the block.

Not a cause of the plateau: `xrEndFrame` as we time it includes any wait for the queue lock. In the normal state the lock
max wait is about 1 ms (at most 5.4 ms), so contention cannot explain a flat 10.7 ms. Worth splitting in the stats anyway.

### 3.4 Steam Link streaming conditions (weak)

Steam Link throttle events, NVENC resets, bitrate trims and frame-id resets all happened, but none at the plateau boundaries,
and the link estimate was steady (232 Mbit/s) through plateau 1. Keep as a control: the Steam Link 2.13 frame-pacing change
could still be what re-phases host timing after a Steam-button press. Testing over USB streaming (new in 2.17) would separate
Steam Link pacing from the compositor.

### 3.5 Ruled out or not supported

- **Releasing swapchain images late / same image twice.** Re-submitting a swapchain with no new acquire/release is legal: each
  swapchain's implicit image is the last released one, and a specific image may be referenced more than once. A third of our
  frames re-send in the fast state too, at 0.8 ms. No `XR_ERROR_LAYER_INVALID`, no failures logged.
- **GPU work arriving late on the queue.** Would vary with GS load. The plateau does not (GS 49.9 - 59.6 fps, `xrEndFrame`
  max 11.2 - 11.9).
- **Motion smoothing / reprojection.** 0 reprojected frames; no per-app section; defaults.
- **Frame-loop misuse.** Wait -> begin -> end in order, one wait per frame, the same `predictedDisplayTime` used for the locate
  calls and for `xrEndFrame`, `shouldRender` honoured (zero layers when false), no overlapped waits, no `XR_FRAME_DISCARDED`.

## 4. Recommended changes to our pacer

Order is by value for cost. 1 first, since it settles which of 3.1 / 3.2 is real.

### 4.1 Fix the stats so they can answer this (small, no behaviour change)

- Time `xrWaitFrame` (start to return) and `xrBeginFrame` (start to return) separately. Today `wait->begin` uses a timestamp
  taken *before* the `xrBeginFrame` call (`begin_ns` in `PacerThreadMain`), so it is always about 0 and hides both. A Khronos forum
  reply (Rectus, "Need help retrieving frame timings from OpenXR") says "some runtimes like SteamVR will block on xrBeginFrame
  instead of xrWaitFrame (against what the spec recommends)", so in the normal state the 10.3 ms block may be in either call.
- Log per window: min / mean / max of `predictedDisplayTime - steady clock at wait return` (relative to the first value), the
  predicted period, and the lock wait inside `LockedEndFrame` separately from the runtime call.
- Log per window the number of layer-array changes (the ordered list of swapchain + size), the number of frames with no
  projection layer, and the session state, so the next log lines up without wall-clock fitting.
- During a test session, press the Steam button once, then wait 3 minutes. Record all of the above. Repeat with each change below.

### 4.2 Keep the layer array stable (medium, likely removes the 4 - 15 ms spikes)

- Always submit the same layers in the same positions, or put the volatile ones (laser or beam, wrist card, toasts, pointer dot)
  at the end of the array so a toggle only changes the tail.
- Give every card and hand swapchain one common size (for example 256x256, or 1024x256) so a slot whose occupant changes
  keeps its shared texture. 127 of 143 logged re-creations were size changes.
- Fewer layers help anyway: Microsoft's OpenXR performance guidance recommends a single projection layer and Qualcomm's says each
  layer adds overhead (both from search-result summaries, not read in full). We are at 3 - 6 layers including the background,
  under SteamVR's limit of 16.

### 4.3 Always have a projection layer when layers are shown (small)

If `LocateEyeViews` fails or the orientation is not valid, reuse the last valid views for the background layer instead of
dropping it. Count the frames where it was missing (4.1).

### 4.4 Try a pipelined loop (small to medium, uncertain)

Spec: `xrWaitFrame` "must block until the previous frame has been begun" and unblocks independent of `xrEndFrame`; it may run
on a separate thread. Put `xrWaitFrame` on its own thread, which waits, hands `XrFrameState` over, then waits again; the
submit thread begins and ends the frame as now. That makes us a "pipelined" app in SteamVR's sense (its 2.10 notes list better
handling of non-pipelined apps) and moves any blocking to where the spec expects it, without a lock held. If the plateau
still shows up, nothing is lost. Run the Khronos best-practice validation layer on it, because overlapping frames are what it
checks.

### 4.5 Make the pacer the only XR caller (larger)

The GS thread currently does `xrAcquireSwapchainImage`, `xrWaitSwapchainImage`, the copy submit and `xrReleaseSwapchainImage`,
then tries `submit_mutex` and defers if the pacer is inside `xrEndFrame`. Let the GS publish "frame N is ready" (a fence or a
timeline value) and let the pacer acquire, copy and release right before building the layers. Then image readiness follows
`xrWaitFrame`, there are no deferred or skipped copies, and the GS never touches the runtime. The dedicated-queue worktree
already orders the two queues with timeline semaphores, which is the same direction. Spec points to respect: the image must be
released (with its writes submitted) before `xrEndFrame`, and after release the runtime treats it as owned by the session queue
in `COLOR_ATTACHMENT_OPTIMAL`.

### 4.6 SteamVR-side experiments (Alex to run; I changed nothing)

- Per-Application Video Settings for this app (the app key above): Motion Smoothing = Off, and the throttling controls if shown.
  Valve's Steam Frame docs describe `preferMotionSmoothingMode` ("off" / "on" / "alwaysOn") and `preferHalfFramerate` as the
  defaults behind that menu, for OpenVR and OpenXR apps alike. The older community-documented controls (SteamVR 1.16.1) write
  `framesToThrottle` and `additionalFramesToPredict` into `steamvr.vrsettings`; I do not know whether Steam Frame exposes
  them. If `additionalFramesToPredict` is pinned to 0 and the plateau vanishes, 3.1 is confirmed.
- A control app: any minimal OpenXR sample that runs the same wait / begin / end loop, with the same three timers, and the same
  Steam-button press. If it also shows an `xrEndFrame` plateau, this is SteamVR behaviour for non-pipelined apps and 4.4 is
  the fix to try. If not, look harder at 3.2.
- SteamVR's in-headset performance graph during a Steam-button press (Valve's wiki describes the "Ready For Use / Present /
  First Viewed" lines; I could not fetch that page).

### 4.7 What not to do

- Do not try to "fix" the block by skipping `xrEndFrame` or sleeping on our side: the spec requires every begun frame to be
  ended, and the runtime then sees a late frame.
- Do not shorten `kXrQueueLockTimeout` (500 ms) or the swapchain waits to hide the plateau; the plateau is far below those.
  Those rules, and the deferred publishing in `ReleaseAndPublish`, exist for a runtime that stalls outright (the 100 ms standby
  park is a mild case of that).

## 5. How our pacer compares with the usual guidance

| Guidance | Our pacer |
|---|---|
| Order wait -> begin -> end; one wait per frame; throttle only in wait | Yes (SteamVR is the one blocking in end) |
| Use `predictedDisplayTime` for all poses and pass it to `xrEndFrame` | Yes (`XRInput::Update`, head, hands, eyes, end all use `fs.predictedDisplayTime`) |
| `shouldRender` false: still begin and end, no layers, skip work | Yes |
| Submit as soon as the draw work is issued; no manual waits for pacing (Meta) | Yes: only a few pose queries and 3 - 6 layer structs between begin and end |
| `xrWaitFrame` may run on another thread; pipelined apps | No: one thread, serial (see 4.4) |
| Few, stable layers; one projection layer; each swapchain has a released image | Partly: 3 - 6 layers, volatile order and sizes (4.2); projection layer sometimes missing (4.3) |
| External sync of the VkQueue for Begin/End/Acquire/Release only | Yes, but on the GS queue (dedicated queue branch exists) |
| Release images after submitting the writes, in `COLOR_ATTACHMENT_OPTIMAL` | Yes (barrier back to attachment layout in `RecordAndSubmitCopies`) |

## 6. Limits of this note

- SteamVR's runtime is closed source. The mechanism in 3.1 is inferred from the frame-timing flags in `openvr.h`, a 2018 Valve
  patent, release notes, and our own numbers. No source I found reports this exact symptom.
- The plateau bounds (63.3 and 189.3) come from 5 s window means; each is good to about a second. The "fast gap" at 108.8 -
  110.55 is weaker still.
- The log says nothing about who owns the pacing change (compositor or Steam Link). The USB-streaming A/B and the OpenVR
  frame-timing read would separate them.

## Sources

Local files read (all read-only):

- `D:\Games\PS2\PenguinScreen2\logs\emulog-headset-20261012.txt` (Frames, session-state and HUD lines)
- `D:\programfile\steam\logs\xrclient_pcsx2-qt.txt`, `vrserver.txt`, `vrcompositor.txt`, `driver_vrlink.txt`,
  `vrclient_vrcompositor.txt`, `vrwebhelper_systemui.txt`, and `D:\programfile\steam\config\steamvr.vrsettings`
- `D:\dev\PenguinScreen2\pcsx2\VR\XRCompositor.cpp` (PacerThreadMain ~5281, LockedBeginFrame / LockedEndFrame ~531 - 548,
  BuildSubmitArray ~4442, MaybeLogFrameStats ~4509, RecordAndSubmitCopies ~716, ReleaseAndPublish ~4275, LocateEyeViews ~3083),
  `pcsx2\VR\VRVulkanBridge.cpp` (ScopedXrQueueLock), `pcsx2\GS\Renderers\Vulkan\GSDeviceVK.cpp` (queue lock around submit and present)
- `.claude/worktrees/agent-a2baabe2fcac60457` commit `ad2d26a` (dedicated XR queue, opt-in)

Web:

- OpenXR 1.1 reference pages: [xrWaitFrame](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrWaitFrame.html),
  [xrBeginFrame](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrBeginFrame.html),
  [xrEndFrame](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrEndFrame.html),
  [xrWaitSwapchainImage](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrWaitSwapchainImage.html),
  [xrReleaseSwapchainImage](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrReleaseSwapchainImage.html),
  [XR_KHR_vulkan_enable concurrency](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XR_KHR_vulkan_enable-concurrency.html),
  [swapchain image state](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XR_KHR_vulkan_enable-swapchain-image-state.html)
- Khronos forum: [Framepacing (Understanding xrEndFrame)](https://community.khronos.org/t/framepacing-understanding-xrendframe/111901),
  [Do you have to do CPU-sync before xrEndFrame?](https://community.khronos.org/t/do-you-have-to-do-cpu-sync-before-xrendframe/110117),
  [Need help retrieving frame timings from OpenXR](https://community.khronos.org/t/need-help-retrieving-frame-timings-from-openxr/110914)
- Layer cost (search-result summaries only): [Microsoft OpenXR performance](https://learn.microsoft.com/en-ca/windows/mixed-reality/develop/native/openxr-performance),
  [Qualcomm composition layers](https://docs.spaces.qualcomm.com/platform/features/composition-layers)
- Khronos blog: [OpenXR best-practice validation layer](https://www.khronos.org/blog/new-openxr-validation-layer-helps-developers-build-robustly-portable-xr-applications)
- Valve: [openvr.h](https://github.com/ValveSoftware/openvr/blob/master/headers/openvr.h) (frame-timing flags, `WaitGetPoses`,
  per-app setting keys); patent [US 10,600,236 B2](https://patents.google.com/patent/US10600236B2/en)
- SteamVR release notes as reported by GamingOnLinux:
  [2.10](https://www.gamingonlinux.com/2025/05/valve-rolls-out-steamvr-2-10-with-quick-recenter-reduced-controller-and-physics-jitter-and-linux-fixes/),
  [2.10.2 beta](https://www.gamingonlinux.com/2025/04/steamvr-beta-2-10-2-released-with-a-linux-vr-fix-and-a-controller-and-physics-improvement/),
  [2.13](http://www.gamingonlinux.com/2025/11/steamvr-2-13-brings-a-whole-lot-of-bug-fixes),
  [2.17](https://www.gamingonlinux.com/2026/09/steamvr-2-17-arrives-ready-to-go-for-the-steam-frame/)
- Community write-up of SteamVR 1.16.1 per-app throttling and prediction overrides:
  [MSFS forum](https://forums.flightsimulator.com/t/steamvr-1-16-1-and-valve-index-observation/351754/11)
- Steamworks Steam Frame docs: [vrpreferences.json](https://partner.steamgames.com/doc/steamhardware/steamframe/vrpreferences),
  [Performance Assessment Overlay](https://partner.steamgames.com/doc/steamhardware/steamframe/compat/perf_criteria)
- Meta: [A VR Frame's Life](https://developers.meta.com/horizon/blog/a-vr-frames-life/)
- Tried but blocked: Valve wiki `SteamVR/Frame_Timing` (HTTP 403), Justia patent page (403).
