# Hand-off to the Windows PC session (from the Mac session, 2026-10-10)

Read this first when you resume. It covers what happened after the PC shut off, Alex's
decisions, what the Mac found, and what to do next. Alex's Mac holds the full reports in
`~/Documents/github/outbreak-vr/docs/reviews/` (not on GitHub). This file is the short form.

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

1. **First person must work in online co-op** (with his brother). Don't just disable it online.
   Make it correct and prove it on the private server first.
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
- **Not done:** the PINE online refusals (savestate, load, MemWatch), task 3's 4-player focus-slot
  case, task 4 (heartbeat) and task 5 (proof).
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
6. **The project rule needs Alex's sign-off.** AGENTS.md hard rule 1 currently forbids game-memory
   writes online. The proposed replacement:

   > First-person writes may run online only in PrivateCoop mode (our private server, both players
   > consenting) after the two-player proof passes. Never on obsrv or an unknown server. Never pause,
   > change speed or touch another player's slot.

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
