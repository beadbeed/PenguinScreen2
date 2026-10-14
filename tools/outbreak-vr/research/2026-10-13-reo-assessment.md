# REO (sp00nznet/reo) as a native-VR foundation: assessment (2026-10-13)

Read-only code review of https://github.com/sp00nznet/reo, cloned to `D:\dev\research\reo`:
- `main` at 73ad3c9 (2026-04-10);
- `pivot-selfexec` at 92a9bc3 (2026-07-15);
- the PS2Recomp submodule (sp00nznet fork) at c46d1c7.

Nothing was built or run, and no game data was used.

## Verdict

Not a foundation yet. Use selected research from it, write none of its code into ours, and continue the emulator approach.

## What works

- **`main`: a puppet.** `recomp/overrides/reo_override.cpp` (around line 779) loads PCSX2 RAM snapshots at frame 5:
  - `pcsx2_mainmenu_8mb.bin` at 0xAEA3C0;
  - 0x324000, 0x340000, 0x236700;
  - the DMA channels and render pool;
  - fake `TIM2` headers;
  - hard-coded display-list and render-table pointers.

  At frame 900 it starts the demo thread by hand (`sub_001CBD00`). The title and demo images are captured state and GS dump
  replays (`tools/gs_dump_replay`), not the game's own live simulation. Its README says the same.
- **`pivot-selfexec`:**
  - the injection is disabled with `REO_NATURAL`, and the stub-era bindings with `REO_NO_OVERRIDES`;
  - the ELF runs its own boot code and stops in IOP/SIF module and resource init;
  - the README: "Not yet at the title screen or gameplay under self-execution".
- **Placeholders:**
  - `RdtParser::parse` (room data) is a TODO;
  - the `Rdt*` structs are guesses;
  - many init functions are bound to `reo_ret0`.

## How native it is

- **Recompiled:** only the ELF, `0x100008`-`0x1F73B0`, plus one title-screen overlay (`0x370000`-`0x37E920`) dumped
  from PCSX2.
- **Not recompiled: the gameplay code.**
  - Outbreak loads its in-game code as a runtime overlay. In SLPM-65428 that is the GAME overlay at 0x540000+, id u32
    `0x540004` == 3, where all our action, pickup, camera and co-op addresses live.
  - None of that overlay exists in REO, so characters, enemies, collision, items and rooms do not exist natively there.
- **Still emulated:**
  - IOP: HLE;
  - VIF1 and VU1: an interpreter (`ps2xRuntime/src/lib/ps2_vu1.cpp`);
  - GS: a basic software rasteriser (`ps2_gs_rasterizer.cpp`);
  - the result is shown as one raylib texture (`ps2_runtime.cpp` around line 2147).
- **Per-eye rendering would need:**
  - a VR presenter in place of raylib;
  - a GPU GS, or a native renderer for the game's VU1 programs;
  - running the scene's VU1 draw pass twice with two view matrices.

  The VU1 work is the same as our "true per-eye" item in PCSX2. Only the EE-side hooking is easier.

## Fit with SLPM-65428

- **Addresses:** every address in REO's overrides is SLUS-20765-specific (for example `0x117818`, `0x3445E8`,
  `0x316598`), and so is its recompiled output. Nothing carries over without remapping.
- **Recompiler:** PS2Recomp is generic and could be run on the SLPM ELF and overlay, but it would still have the same
  overlay gap.
- **Licences:**
  - REO has no licence (GitHub `license: null`, no LICENSE file), so its code is all rights reserved. We can read it
    for facts about formats, but must not copy it.
  - PS2Recomp is GPL-3.0, which is compatible with PCSX2/PenguinScreen2's GPL-3.0+.
- **Disc-derived code:** REO commits about 65 MB of C++ recompiled from Capcom's ELF (`recomp/config/output`). Under our
  rules that counts as extracted from the disc, so it must never go in our repo.
- **Useful knowledge:**
  - the NBD chunk directory (TEX / TID / AMO / AHI);
  - TEX = OB-LZ77-compressed TIM2;
  - the NETBIO/AFS layout;
  - the PINE overlay-dump method;
  - how the boot gates were cleared, as a guide if we ever recompile.

## Smallest decisive experiment (not started; needs approval)

"Native one-room simulation" is premature: REO cannot boot to a menu, and the gameplay overlay has never been
recompiled. The first blocker is that the GAME overlay is not native code, and the experiment below tests that
directly, without REO's boot:

1. **Recompile one known routine.** Take the pickup search core `FUN_005ABBE0` (a pure walk over the trigger list from a
   point) from our own J's Bar RAM image, local only, never committed. Recompile it with PS2Recomp.
2. **Run it natively against the same RAM image** for a grid of probe points.
3. **Compare** with the result from the instruction-level interpreter (the same answers PCSX2 gives).
4. **If it agrees, a one-frame lockstep test.** From a File #1 slot-7 RAM state, run one GAME-overlay frame natively and
   diff guest RAM against PCSX2's next frame. Agreement there would be the first real evidence of native room
   simulation.

**Needs approval:**
- the toolchain (already proposed);
- building PS2Recomp;
- extracting overlay code from our own dump, locally;
- an offline PCSX2 run, for step 4 only.
