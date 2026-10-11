# File #2 (SLPM-65692) live research, 2026-10-11 (orchestrator, offline, VR test input, first person off unless noted)

Savestates: slot 1 = Training Ground (Kevin, local slot 0x4912BD = 0, HP 2300/2300, .45 not equipped);
NEW slot 2 = same place with Kevin's .45 Auto EQUIPPED (+0xC8C = 0x0D), calm.

- M4/C1 equip: Kevin's item cell 0 holds item record 0x0A = id 0xD2 (210) ".45 AUTO MAGAZINE", 7 rounds (submenu COMBINE/PRESENT:
  backed out with Cross). His special box holds record 0x0D = id 0x190 (400) ".45 AUTO FOR KEVIN", 7 rounds, submenu EQUIP/RELOAD.
  EQUIP sets local +0xC8C = 0x0D; record 0x397B40 + 0x0D*60: present 1, index 0x0D, id 0x190, count +0x24 = 7, owner +0x28 = 1.
- M8/C2 near-cull: f32 0x718DB4 = 80.0 with first person off (0x718DB0 = 20.0). restoreF32 80.0 is right.
- M9/C3: HP +0x540/+0x542 = 2300/2300, char id +0xBB0 = 0 (Kevin), virus counter +0xBC4 = 0 (stays 0 over 20 s in Training
  Ground), bleed +0xC6A = 0, status +0xBB8 = 0. Virus table 0x728500 (u32 x8): 0x24EA0 0x42CC0 0x189C0 0x54600 0x54600 0x31380
  0x54600 0x189C0.
- M6/C4 held word b+0xF90 (u32): idle 0x0; VR B (= PS2 Cross) held -> 0x10; trigger (= Circle) held -> 0x20.
- M10/C5: 0x4912BE = 1 with the map (Triangle) open and with pause (Select) open (a second press of the same button did not close
  either; Cross closes them). So pauseWhen 0x4912BE covers item screen, map and pause.
- M1/C6 raise step: stance 1 -> 2 within one 50 ms sample; position moves ~20 units forward over ~0.3 s (slot 1: 0.3, 3.2, 6.1,
  13.8, 19.4, 20.5; slot 2: 0, 5.1, 14.0, 16.0, 18.8, 20.0) then holds. raise_settle_s ~0.4 s.
- M2/C7 the game's own aim response (weapon raised, first person off): PS2 left stick right for 1 s turns the heading (156 -> 44..64
  deg), the left stick up does NOT move Kevin (0 units). Heading writer PC = 0x64F41C (memwatch). File #2 does not aim-walk natively.
- M3/C8 pitch hold with first person ARMED (scripted head), slot 2, aim -20 deg: +0xBE0 = -3641 every sample (expect -3641); all
  seven servo words read 0 while aiming and their originals (0x0C0D278C x3, 0xA6400BE0, 0xA6430BE0 x2, 0xA6400BE0) after lowering.
- M5/C9 action byte +0x54B (sub-action +0x09): idle 0 (sub 0); walk 1 (sub 4/3); run (B + forward) 3 (sub 6/7); stop 0; raise
  0 -> 128 (stance 2, sub 0 then 1); shot 128 sub 2; after shot sub 1; lower 128 sub 7 then 0 stance 1. Same values as File #1 +0x54F.
- M7/C10 shots: each shot drops the gun's +0x24 by 1 (7 -> 0 over 7 shots; the first trigger press right after the raise did not
  fire); at 0 rounds further trigger presses change nothing (no dry-fire byte seen); L1 (left trigger) does NOT reload (ammo stays 0,
  the magazine record keeps 7). Reload is the item screen's RELOAD. No reload action value was observed: leave feedback.weapon
  action/reload unset for File #2 (reload is still detected by the count rising).
- M11/C11, M12/C12: not done (no enemy save for File #2).
- M13/C13 item-screen cursor struct: 0x7671F0 (same layout as File #1's 0x720B80): +0x00 u32 = 1 while open, +0x04 u32 = node
  pointer, +0x0C u16 index, +0x0E/+0x10 u16 x/y. Moves: open (498,108) -> Right (550,108) -> Down (550,160) idx 3, node 0x767448.
  Nodes 0x98 apart and cell 0 = base + 0x90 = 0x767280 (as File #1: 0x720B80 + 0x90 = 0x720C10), so cells 0x767280, 0x767318,
  0x7673B0, 0x767448; the special box node by analogy 0x767280 + 49*0x98 = 0x768F98 (UNVERIFIED).
