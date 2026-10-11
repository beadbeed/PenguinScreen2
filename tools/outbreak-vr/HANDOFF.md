# Hand-off to the Windows PC session (from the Mac session, 2026-10-10)

Read this first when you resume. It covers what happened after the PC shut off, Alex's
decisions, what the Mac found, and what to do next. Alex's Mac holds the full reports in
`~/Documents/github/outbreak-vr/docs/reviews/` (not on GitHub). This file is the short form.

## PC update 2 (Windows PC, 2026-10-11 afternoon): waves 4 and 5

Branch `wave4-integration` holds both waves on top of `pc-firstperson`. Merge it into `pc-firstperson` and
`coop-firstperson` once its checks pass.

**Wave 4 (the real-headset problems from the 10 Oct logs):**
- **Frames come from the VR pacer at the headset's rate** (`[VR] HeadsetRateSubmit`, default on). Before, one frame went
  out per GS present (59.94/s on a 90 Hz display), and nothing at all while the GS was idle, so SteamVR timed the app
  out and showed Home. The GS publishes layer sets, and the pacer submits every display period, with a stale rule and
  smooth-turn extrapolation.
- **An opaque black projection layer** goes under the screens (`OpaqueBackground`), so SteamVR Home no longer shows around
  the quad.
- **A bounded queue lock** protects the shared VkQueue. The GS never waits for a pacer inside `xrEndFrame`: the publish
  is deferred to the next present instead. All of a present's swapchain waits share a 3 ms budget. VR is abandoned only
  after 500 ms of no answer.
- **Loss and fault handling:** losing the session falls back to flat play, `PCSX2_VR_XRFAULT` can inject faults, and
  the Frames, Eyes and First-person screen lines are logged.
- **Controller presence:**
  - toasts for an asleep or missing controller;
  - a binding report naming each hand's interaction profile;
  - a stale headset pose disarms first person;
  - the right stick drives the menu D-pad.
- **Weapon feedback:**
  - the pistol and laser show only when a weapon is equipped;
  - shots, dry fire and reloads give haptics, a recoil kick, the slide and a muzzle flash;
  - the wrist card shows ammo;
  - hurt and heartbeat cues keep working while grabbed.
- **Co-op safety:**
  - pause, state loads and saves, and speed changes are refused at the VMManager chokepoints while the adapter is on;
  - the local record's identity is checked through `+0x03` (both Files);
  - the focus hold is given back when its condition fails;
  - File #1's releaseWhen status range is fixed.
- **Stereo:** true near depth to 0.75 m, parallax aligned with head roll, and a stereo dump with `stereo_metric.py`.

**Wave 5:**
- **Hands, laser and wrist card are placed at the display rate** on the pacer (`HandsDisplayRate`).
- **Menu pointer:** a beam and dot on the world screen, hands drawn in menus, and an optional chest tablet.
- **Point-to-select on the item screen:** File #1 maps the four cells and the equipped box; File #2 maps the four cells
  (cursor struct `0x7671F0`). It uses closed-loop D-pad steering and never confirms anything itself.
- **Auto-orthostereo** from the headset's IPD and the placed screen.

**Measured on the null HMD (offline):**
- Speed: the legacy path ran the emulation at about 50% speed (its xrWaitFrame blocked the GS). The pacer path with
  the deferred publish and the wait budget held 96%, even with a null compositor that only takes about 22 frames/s.
- Earlier wave-4 checks passed: frame submission, the background, the eye and screen lines, the bindings report,
  shot and hurt haptics, the grab hand-back, online locks, record identity and the focus give-back.

**Caveat about this afternoon's numbers:** from about 13:40 the PC hitched about 300 ms every ~1 s, even in plain flat
mode with yesterday's build. It isn't CPU, disk, audio or SDL; most likely the monitors were off or the session was
locked. Timing-sensitive harness checks (smooth turn, walk-and-shoot) failed for that reason, not because of the code.

**New savestates:**
- File #1 slot 5: gun equipped, a zombie grabs Kevin 5-7 s after loading.
- File #1 slot 6: calm, gun equipped.
- File #2 slot 2: Training Ground with the .45 equipped.

**Research results** (offline, 2026-10-11) are in `tools/outbreak-vr/research/`:
- `2026-10-11-file1-research.md`: record identity, weapon records and shots, the item-screen cursor graph, and R1 lock-on
  vs point-to-aim (the lock-on doesn't take over the heading);
- `2026-10-11-file2-research.md`: File #2's equip, held word, action byte, pitch sign, raise step, reload and cursor
  struct.

## PC reply (Windows PC session, 2026-10-11)

**Section 0:**
- Nothing was unpushed (63cc827 had gone up before the shutdown).
- `steamvr.vrsettings` is the original: no forcedDriver, requireHmd, driver_null, power or dashboard changes.
- No leftover `vrprofiles` override for either Outbreak serial.

**Branches:** `coop-firstperson` was fast-forwarded into `pc-firstperson`, and both now point at the same commit. Keep using
`coop-firstperson` for Mac work; the PC merges it.

**Changed (331dde9):**
- **Private co-op plays first person in full.** Walking while aiming and aim pitch follow the Online-Safe setting
  there, instead of being forced off by the network adapter. Alex's 10 Oct rule allows game writes online, and
  partners take your character as state values. `OnlineSafeNow()` = setting, or adapter on and not PrivateCoop.
  obsrv and unknown servers stay Restricted (3D screen).
- **A headset toast once per game:** "Private co-op: first person online" or "Online: 3D screen (not private co-op)".
  The DISARMED log now names the online restriction (it used to say "profile guard failed").
- **Task 3 (never focus a partner):**
  - The focus hold goes to slot 3 only while that record is empty, checked as max HP 0 (File #1 `+0x546`, File #2
    `+0x542`).
  - When you are slot 3, it goes to slot 2 if that is empty.
  - A full room holds nothing, so your body may show there.
  - Checked offline: unused records are all zero; used ones have max HP and `+0x00` = `0x01`.
- **Tools:**
  - `vrtest.py`, `fp_proto.py` and `test_fakevr.py` refuse to run with the adapter on or the game's network flag set
    (`pine.require_offline`).
  - `vrtest.py`'s online-safe scenarios now use the VR setting only.
  - The sweeps `trap` their override away.
  - `null_driver.py on` keeps the real backup if the null HMD is already on.
  - `netmode.py` finds the PC's `inis\PenguinScreen2.ini` (it only looked for `PCSX2.ini`) and refuses while the
    emulator runs.
- **File #2 profile:** the stale heading comment is fixed.

**Checked your File #1 heartbeat patch against memory (offline, File #1 slot 4):**
- The continue path now also runs `0x633590`. That word is the exit branch's delay slot, `ld ra,0(sp)` (the exit is
  `0x6337A8` = the same `ld ra`, so the `b` went to `0x6337AC`). No call comes before it, so `ra` still equals its saved
  copy: harmless.
- The sub-action filter is a list of compares that branch to the send (`0x63379C`: `a1=2`, `jal 0x64C620`).
  It ends with `beq v1,a1` (sub-action 1) and `beq v1,zero` (sub-action 0).
- The patch leaves `a1 = 1` on the continue path, so **aim idle (stance 2, sub-action 1 — read live) is sent**.
  That was intended or lucky, but either way it works.
- File #2 `0x638F00` reads `2403001E` in-game, as the profile expects.
- Note: sub-action 0 (standing idle) is sent too, so "only when moving" in section 3 is not quite right.

**Tested on the PC (CI build of 331dde9, offline).** Each mode booted File #1 straight into Single Play slot 4 from the
command line, with the fake head and no runtime. Nothing was contacted, and the ini was restored byte for byte.

| ini (set by netmode.py) | Log | Armed | Online-safe | Heartbeat words | Camera NOPs | Focus |
|---|---|---|---|---|---|---|
| adapter off | `network mode offline` | yes | off | original | on | 3 |
| `obsrv` | `online, restricted` + toast | no (reason 7) | on | original | off | 0 (local) |
| `private 192.168.1.14` | `private co-op` + toast | yes | **off** | `24030003`, `2465FFFF`, ... | on | 3 |

The game ran with the co-op patches in place without trouble (Single Play loopback). The headset-free harness from
slot 4 passed 11 of 14 on the first run:
- walk-and-shoot and run-latch-b-tap passed on rerun (known flakiness);
- pitch passes from slot 3, but not from slot 4 (a test-save quirk, now noted in `vrtest.py`).

The final build, **041ce54** (adds the Qt Pause lock), is installed on the PC. A smoke run of the harness passes; point-to-aim and pitch failed once each on timing, then passed twice. SteamVR wasn't touched today.

**Small audit correction:** "body hidden / body follows view checked on File #2 only" — File #1 was checked too, on 10 Oct
(`test_fakevr.py features`: focus slot 3, body-follow within 0.6°). The harness's body-follow scenario is File #1 too.

**Still open:**
- Task 5 (the two-machine proof) waits for Alex's go, and only he logs in to the server.
- With the adapter on, the Qt menu/toolbar Pause and the Game List button (which pauses) are now refused with an OSD
  message. Dialogs that pause through `pauseAndLockVM` (change disc, input recording, ...) still can; they are
  deliberate actions, so they were left as they are.

## 0. Before anything else

1. **Commit and push unpushed work.** Run `git status` and `git log origin/pc-firstperson..` in the
   PenguinScreen2 checkout. Anything the PC made after 20:16 UTC on 10 Oct was never pushed. Commit it,
   rebase onto `pc-firstperson` (this file arrived there), and push.
2. **Check what the tools may have left behind when the PC shut off:**
   - **SteamVR stuck on the null (simulated) headset.** Run `python tools/outbreak-vr/null_driver.py off`,
     then open `steamvr.vrsettings`. Check that there is no `forcedDriver: null` and no `requireHmd: false`,
     and that `enableHomeApp`, `dashboard.enableDashboard` and `power.turnOffScreensTimeout` are back to
     normal. `null_driver.py on` overwrites its own backup, so a double `on` means the backup holds the
     null settings and has to be fixed by hand.
   - **A leftover profile override.** Delete `D:\Games\PS2\PenguinScreen2\vrprofiles\SLPM-65428.yaml` and
     `SLPM-65692.yaml` if they exist. `sync_sweep.sh` and `lag_sweep.sh` write them and only remove them at
     the end, with no trap. A leftover silently replaces the shipped profile.
3. Branches on beadbeed/PenguinScreen2:
   - `windows-steamvr`: the base; pushes build automatically.
   - `pc-firstperson`: your first-person work.
   - `coop-firstperson`: the Mac's co-op work. It branches from `pc-firstperson`; merge it when it is green.

   Pushes to `pc-firstperson` and `coop-firstperson` do not start CI. Use
   `gh workflow run build-windows.yml -R beadbeed/PenguinScreen2 --ref <branch>`.

## 1. What the Mac's audit of your 48 commits found

Full report: `docs/reviews/2026-10-10-pc-firstperson-audit.md` on the Mac. Each safety item below was
confirmed by 2 or 3 of 3 independent reviewers.

- **Only one real headset test is recorded** (around build `9f83905`). It had sickening judder, SteamVR
  home showing, and dead controllers; SteamVR only saw the right controller, 12 s before the emulator
  closed. Every fix since was tested only on the null headset, a fake head or self-tests. Nothing after
  that test has run on the Frame.
- **Online:**
  - Nothing reads the game's network flag (`0x3065C2` File #1, `0x3137D2` File #2).
  - Heading is written online, with no check, by point-to-aim (`CameraDriver.cpp:1613-1614`) and
    body-follow (`:1685-1686`).
  - The seven camera/stick-latch code NOPs have no `offlineOnly`.
  - Eye, target, FOV and the holds also run online.
  - The Online-Safe help text ("nothing other players would see") isn't accurate.
- **Pausing online:**
  - `3386d1d` covers only VR devices. A DualSense or virtual pad disconnecting can still pause online,
    and so can focus loss. Both options are off by default, but they are still existing PCSX2 paths.
  - `vrtest.py` online-safe scenarios can pause (MemWatch) and load savestates with the network adapter on.
  - `fp_proto.py` and `test_fakevr.py` have no online check.
- **Small items:**
  - The stereo row (0.0132 / 150) was never tested; only 0.0188 / 150 ran.
  - "Body hidden" and "body follows view" were checked on File #2 only.
  - The File #2 heading comment in `SLPM-65692.yaml` ("not confirmed") is stale; `6c4fbdc` checked it
    live.

## 2. Alex's decisions (10 Oct)

1. **First person must work in online co-op** (with his brother). Alex, 10 Oct: **first person is NOT
   forbidden from touching the game online.** It may write game memory and patch game code online.
   The only online limits that stay:
   - never pause;
   - never change speed;
   - never load or save a state;
   - never write another player's character.
2. **Less verification.** Heavy multi-agent checking is only for the network question; do everything
   else in one pass.
3. **Getting off the emulator is the long-term goal, not the next step.** The recommended order is:
   - emulator co-op now;
   - cheap tests;
   - a native-renderer hybrid (emulator runs the game, a native OpenXR renderer draws whole rooms at
     headset rate);
   - a full PS2Recomp port later, as a time-boxed spike.

   No existing native Outbreak project is usable:
   - RaccoonRecomp is US File #1 only, binary-only, with no online;
   - sp00nznet/reo is stuck before the title screen when running on its own;
   - caprado/REOF2 only reaches the 2D menus.

## 3. What the game sends over the network (decompiled from File #1, offline)

Network wrappers live in the main ELF at `0x1AEDC0-0x1AF440`. They check `0x3065C2`. `SINGLE.BIN`
(`0x820000`) is the Single Play loopback stand-in for `NETAQ.BIN`, with the same message API.

**Confirmed by 3 of 3 skeptics:**
- Your character goes out **as values (state snapshots), never as pad input**.
- `FUN_0064C620(player, type)` builds the message and sends it on channel `(+3)+1`. The main types:
  - type 1: action bytes, position, heading `+0x92`, HP, status `+0xBA0`, aim pitch `+0xBC8`,
    target `+0xCDC`, weapon and more;
  - type 2: a heartbeat with position, status and heading;
  - type 5: damage and knock-down.
- Nothing goes out during event sequences (`0x48BF88`) or before `FUN_001AF510` (`0x3065CB` is 1 or 2).
- Remote players are **never simulated from input**:
  - `FUN_001AFC60` builds no input block for remote slots;
  - `FUN_006383E0` clears their input words;
  - `FUN_006399A0` skips the input state machine for remote humans.
- `+0x54F`, the camera globals and the input block are never sent.

**When state goes out (2 of 2 skeptics):**
- `FUN_006399A0` sends type 1 when `(+8, +9)` changes, except that `FUN_006337C0` suppresses the
  free-stance locomotion sub-actions and weapon-ready 0/1/5/6/7/0x0A.
- `FUN_00633530` sends the type-2 heartbeat only when the frame counter `0x48BF93 % 30 == 0`, `+8 == 1`
  and the character is moving (a locomotion sub-action).
- Nothing periodic is sent while the weapon is raised.

**Unverified (the run stopped before the skeptics got to these):**
- The receiver applies messages with `FUN_0064D140`.
- `FUN_00632DC0` dead-reckons a remote player toward the last snapshot.
- `FUN_006321D0` picks the remote's walk/turn animation from that motion.

**What this means for first person:**
- It cannot cause input-replay desync.
- The cost is lag and snapping on the partner's screen:
  - **Look around while standing or walking:** heading reaches him only with the heartbeat, about
    once a second, then gets smoothed.
  - **Aim with the weapon raised:** yaw and pitch reach him only when a non-suppressed transition is
    sent, such as firing, so he sees your gun snap when you shoot.
  - **Camera writes, culling holds and camera NOPs:** never sent. They change only how your own game
    moves your character.

## 4. Co-op plan (summary)

**Day one, no new code: obsrv.**
- A passworded two-player room on File #1.
- Both on wired Ethernet and holding exactly 100% speed:
  - frame limiter on, Sync to Host Refresh off, EE cycle rate/skip 0;
  - turbo, slow-motion, frame-limiter, pause and savestate hotkeys unbound;
  - Pause on Focus Loss and Pause on Controller Disconnect off;
  - no savestates online.
- Both load the same way: plain ISO, Fast CDVD off, no HDD install.
- Network settings: Sockets, Device Auto, DNS1 208.72.237.31 Manual.
- The steadiest machine walks into rooms first. Whoever enters first probably becomes room master and
  runs the zombies; that is inferred, not verified.
- Until co-op first person (section 5) is proven, Alex uses the VR stereo screen with Head-Tracked
  Camera **off** online, which writes no game memory.
- Auto-login works for only one obsrv user per household IP. The other player types ID and password.

**The private Bioserver** lives on Alex's Mac, in `private-server/` (Docker). Its scripted checks pass,
but **no real game has ever connected to it.** First connect Mac ARMSX2 to it, then the Windows PC
(flat). The Windows settings for it:
- Internal DNS: `kddi-mmbb.jp` and `www01.kddi-mmbb.jp` → the server, `gate1.jp.dnas.playstation.org`
  → `153.121.44.150`;
- DNS1 Manual `192.0.2.1`;
- Device Auto.

For the brother remotely, use Tailscale (`tailscale ping` must show a **direct** path).

**Desync causes:**
- The relay keeps no game state.
- One player's game runs each room's zombies.
- An emulator dipping below 100% probably makes it worse; that is inferred.

Our private server runs the same unpatched relay code as obsrv (Nagle on, the 0x82 framing drop, an
8 KB buffer). A clean LAN test therefore doesn't fully rule out the relay.

## 5. Co-op first person: what is being built (branch `coop-firstperson`)

**Status (Mac, 10 Oct), from the first commit on `coop-firstperson`:**
- **Done (task 1):** the mode gate. A `[VR] PrivateCoopFirstPerson` setting (default off) with a
  checkbox in Settings → VR. `camera.networkFlag` is in both profiles. `CameraDriver::ResolveNetMode`
  picks Offline, PrivateCoop or Restricted every vsync. Restricted means first person doesn't arm
  (`DisarmReason::OnlineRestricted` = 7, also known to `pine.py`). Each mode change is logged as
  `(VR) CameraDriver: network mode ...`.
- **Done (part of task 2):** no pause on focus loss or on any controller disconnect while the network
  adapter is on (`QtHost.cpp`). The Online-Safe help text is corrected.
- **Done (more of task 2, second commit):** while the network adapter is on, these hotkeys do nothing
  and show a short OSD message (`OnlineLocked()` in `Hotkeys.cpp`):
  - frame advance, frame limiter toggle, turbo (toggle and hold), slow motion, speed up/down;
  - pause and the pause menu (`CanPause()`);
  - every savestate save or load hotkey.
- **Done (third commit):** PINE refuses MsgSaveState, MsgLoadState and MsgVRMemWatch while the
  network adapter is on.
- **Done (fourth commit; task 4 for File #1 only):** new silence flag `coopOnly` (active only in
  NetMode::PrivateCoop). SLPM-65428 patches `FUN_00633530` so your heading heartbeat goes out every 3
  game frames instead of 30 (`0x633548`: `addiu v1,zero,3`). It now also goes out with the weapon raised
  (`0x633580-0x63358C`: stance in {1, 2}). The original words were checked against the File #1
  savestate, and the patch words were disassembled back.
  - **Untested.** Offline these patches never apply; only a private-server session exercises them.
  - **The receiving side is unverified:** whether `FUN_0064D140` applies type 2 well while the partner
    copy is in weapon-ready. Watch for your brother seeing your character jitter or snap while aiming;
    if so, drop the three stance words and keep only the interval patch.
  - **File #2 (fifth commit):** its heartbeat is at `0x638ED8`, and the 30 is at `0x638F00`, found by
    instruction pattern in the File #2 savestate. It is patched the same way (30 → 3, coopOnly). File #2's
    send check (`0x638BB0`) already accepts the aim-idle state, so it needs no stance patch.
- **Also new:** `tools/outbreak-vr/netmode.py` switches a PC between the private server, obsrv and off
  (see 5b).
- **Not done:**
  - task 3's 4-player focus-slot case (2-player rooms should be fine: the hold uses slot 3, or 0 if you
    are slot 3);
  - task 5 (proof);
  - first person on obsrv is not wanted (see task 6).
- **Tested:** only `clang -fsyntax-only` on the Mac for `CameraDriver.cpp`, `VRProfileDB.cpp` and
  `Pcsx2Config.cpp`, plus CI. `QtHost.cpp` and the `.ui` file are checked only by CI. Nothing has been
  run.
- **Offline test, no headset needed** (launch with the fake head):
  - adapter off: the log says `network mode offline` and first person arms;
  - adapter on with no private host override: `online, restricted` and it doesn't arm;
  - adapter on, the checkbox on, and Internal DNS `www01.kddi-mmbb.jp` set to a non-208.72.237.x
    address: `private co-op` and it arms.

  Turn the adapter back off afterwards.
- CI: pushes to `coop-firstperson` now build (added to `build-windows.yml`).

Carry on from this list, in order:

1. **Mode and gate.** `[VR] FirstPersonOnline = Off | PrivateCoop` (default Off). Resolve the mode every
   vsync:
   - **Offline:** network flag 0 and DEV9 off. Everything is allowed.
   - **PrivateCoop:** network flag 1 and the DEV9 Internal DNS points `kddi-mmbb.jp` at a non-obsrv
     address, and the user chose PrivateCoop.
   - **Anything else** (obsrv, an unknown server, flag unreadable): screen-only first person, with
     no game-memory writes and no code patches.

   Fail safe: unknown means the most restrictive mode.
2. **Online hard guarantees in every mode:**
   - never pause (extend the no-pause exemption to all devices and to focus loss);
   - never change speed;
   - block the turbo, slow-motion, frame-limiter, pause and savestate hotkeys while the network flag is
     1 or DEV9 is on;
   - PINE refuses savestate, load and MemWatch while online;
   - restore patches on disarm without stalls.
3. **Never touch another player's slot.** Online, every per-player write (heading, holds, focus slot)
   uses the local slot from `0x48BF7D` (File #2: see the profile), never slot 0. The focus-slot hold
   must not point at a partner.
4. **Make heading and aim reach the partner promptly** (PrivateCoop only). The heartbeat is
   `FUN_00633530`'s `% 30`, and it is free-stance only. Options, simplest first:
   - (a) a profile code patch that lowers the heartbeat interval, for example to every 3 frames
     (about 10 Hz; type 2 is 0x18 bytes, so traffic stays tiny), and allows it while `+8 == 2`;
   - (b) while aiming, briefly let the next aim transition through `FUN_006337C0`'s suppression so a
     type 1 with pitch goes out.

   Decide from the disassembly; the decompiled functions are on the Mac in
   `~/tools/outbreak-re/coop-netcode/decomp/fn/` and stay local.
5. **Two-player proof on the private server.**
   - Machine A: the Windows PC with first person.
   - Machine B: Mac ARMSX2, flat observer, read-only.
   - Run a read-only PINE logger on each at 10 Hz. Windows PINE is TCP on loopback; `pine.py` here
     already speaks it.
   - Log A's slot: position `+0x38`, heading `+0x92`, action `+0x08/+0x09`, aim pitch `+0xBC8`. Also
     log the room `0x3065AC` and the frame counter.
   - Phase 0: both flat. Phase 1: A in first person, with the same script (idle, turn, walk loop
     through a cut and a door, weapon raised, a shot).
   - **Pass:**
     - B's copy of A follows A's heading within the heartbeat interval;
     - no new position jumps over 50 cm;
     - no room or action mismatch longer than 2 s;
     - no drops;
     - both machines at 100% the whole time.
6. **The project rule (Alex, 10 Oct): first person may touch the game online.** AGENTS.md hard rule 1
   on the Mac now says so. First person may write game memory and patch game code online. Online it
   must still never pause, change speed, load or save a state, or write another player's character.

   **What the code does today:** first person runs online when Private Co-op First Person is on and the
   login host points at a non-obsrv server, which is what `netmode.py private` sets up. On obsrv it
   stays off and uses the 3D screen. **That is what Alex wants:** he only plays online co-op with his
   brother on the private server and doesn't need first person on obsrv. Nothing more to do there.

## 5b. The easy private server

**Status (Mac, 10 Oct):**
- The server is **running** on Alex's Mac at its LAN address **192.168.1.14**, and all its checks pass:
  - the login page and encryption;
  - the lobby on 8300 and the game server on 8690;
  - a full login round trip.
- Fixed: the lobby used to die if it started before its database (after the Mac or Docker VM
  rebooted). It now waits.
- **Accounts:** `alex` and `adrian`. The passwords are in `private-server/accounts.local.txt` on the
  Mac only; Alex gives Adrian his.
- **For Alex's PC at home:** `python tools\outbreak-vr\netmode.py private 192.168.1.14`.
- **For Adrian remotely:** Tailscale isn't installed on the Mac yet. Once Alex installs it and signs in,
  restart the server on the Tailscale address (`./server.sh up --ip <100.x>`). Then everyone uses
  `netmode.py private <100.x>`, Alex's PC included.

1. **Server (Alex's Mac, already built):** in `~/Documents/github/outbreak-vr/private-server`, run
   `./server.sh up`. The first run takes 15-25 minutes; later starts take seconds. Make one account each
   with `./server.sh adduser <id> <password>`. Keep the Mac awake while you play
   (`caffeinate -dis ./server.sh up`, or just leave it plugged in with sleep off).
2. **Connecting:** install **Tailscale** (free) on the Mac, Alex's Windows PC and the brother's PC. Alex
   invites his brother to his tailnet, and everyone signs in. The Mac's Tailscale address is `100.x.y.z`
   (`tailscale ip -4`). Check with `tailscale ping <mac>` from each PC; it must say **direct**, not
   **DERP**.
3. **Each Windows PC, with the emulator closed:**
   `python tools\outbreak-vr\netmode.py private 100.x.y.z`
   - This turns on the adapter, points the game's login hosts at the Mac, keeps DNAS on its public
     replay, and turns on Private Co-op First Person.
   - `python tools\outbreak-vr\netmode.py obsrv` switches back to obsrv.
   - `... status` shows the current mode.
4. **In the game:** Network Play → DNAS → the login page. Type the ID and password from step 1; there
   is no auto-login on the private server. Then pick a handle, choose the area, create a passworded
   room (one player), and join it (the other).
5. **First time only:** no real game has ever connected to this server. Try Alex's Windows PC alone
   first. If DNAS, the login page or the lobby fail, save the emulog and run `./server.sh logs` on the Mac.
   Expected failure points, in order:
   - the PS2 browser rejecting the server's TLS;
   - DNAS;
   - reaching the game server address;
   - the menus.

   `private-server/README.md` on the Mac has the details.

**Even easier, if the brother lives with Alex:** skip Tailscale and use the Mac's LAN address
(`192.168.1.14`) in step 3.

## 6. The next real headset test (offline, Single Play, network adapter off)

Do section 0 first. Install the newest green build, and check that SteamVR shows **both** Frame
controllers before launching. Then:

1. First person arms. In the emulog, note which controller binding set was accepted.
2. Turn your head slowly, then fast. Is there judder? Is SteamVR home still visible?
3. Rate depth, brightness and comfort from 1 to 5.
4. Smooth turn, then snap turn with the comfort blink.
5. Walk where you look. Cross a camera cut and go through a door; first person must come back.
6. Check the body is hidden. Recenter with both stick clicks and with the Steam button.
7. Belt zone opens the item screen, Y opens the map, then pause. Do the menus appear in front of you?
8. Raise the weapon. Do the pistol and laser show? Does Kevin face the controller? Fire, and note
   whether the shot lands where the laser points.
9. Take a hit and check the haptics.
10. Take the headset off for 30 s, then put it back. The game must keep running.

Save the emulog, `vrserver.txt` and the session recorder output.

## 7. Talking to the Mac session

It may have hit its usage limit. If it is reachable, ListAgents shows it as "Steam Deck usage
patterns". Otherwise Alex relays. Rules (from AGENTS.md):
- never pause or speed up online;
- never upload discs, BIOS, memory cards or savestates;
- no logins, terms or publishing without Alex;
- commits use the beadbeed noreply email.
