// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

// vr_selftests: the VR layer's pure self-tests (no OpenXR session, no Vulkan device, no running VM) in one console run,
// for CI. In the emulator they run once with PCSX2_VR_SELFTEST (and its siblings) set and are read from the emulog by
// hand; here each runs once, its own summary line is printed with PASS or FAIL, and the exit code is 0 only when every
// one passed (1 when any failed, 2 when the shipped profiles are missing).
//
// Most of them are public and called directly. Two groups are not, and are reached the way the emulator reaches them,
// with nothing to act on: CameraDriver::Apply with no VM (its first act each vsync is the matrix, hook, predict,
// comfort and FOV self-tests and SpatialControls' Gamepad line, and with no VM it then returns at its "no VM" disarm
// before any game memory or profile is read), and XRCompositor::Initialize with no XR session (the pacer's menu pointer
// and the edge fill's plan self-tests, then its "no XR session" return before any runtime call). Their results are read
// from the lines they log.
//
// Not run here: GSDeviceVK's multiview self-test (PCSX2_VR_MV_SELFTEST, needs a Vulkan device) and the texture cache's
// source guard (PCSX2_VR_SRCGUARD_SELFTEST, frees a draw source in a running GS on purpose).
//
// Usage: vr_selftests [resources directory]   (default: this checkout's bin/resources, for the shipped profiles)

#include "VR/BeltPouches.h"
#include "VR/DepthExport.h"
#include "VR/DepthWarp.h"
#include "VR/CameraDriver.h"
#include "VR/GameFeedback.h"
#include "VR/HandModel.h"
#include "VR/HudCards.h"
#include "VR/ItemReach.h"
#include "VR/MenuPanel.h"
#include "VR/MenuPointer.h"
#include "VR/MeshRaster.h"
#include "VR/PhysicalMelee.h"
#include "VR/ReloadGesture.h"
#include "VR/SpatialControls.h"
#include "VR/StereoAuto.h"
#include "VR/VRProfileDB.h"
#include "VR/ZoneMarkers.h"
#include "VR/XRCompositor.h"

#include "Config.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef VR_SELFTESTS_RESOURCES
#define VR_SELFTESTS_RESOURCES ""
#endif

namespace
{
	std::mutex s_log_mutex;
	std::vector<std::string> s_log;

	// Every line the emulator logs (Console, DevCon) is printed and kept, so a self-test that only logs its result can be
	// read back.
	void OnLog(LOGLEVEL, ConsoleColors, std::string_view message)
	{
		std::lock_guard<std::mutex> lock(s_log_mutex);
		s_log.emplace_back(message);
		std::fwrite(message.data(), 1, message.size(), stdout);
		std::fputc('\n', stdout);
		std::fflush(stdout);
	}

	size_t LogMark()
	{
		std::lock_guard<std::mutex> lock(s_log_mutex);
		return s_log.size();
	}

	// The first line logged since mark that starts with prefix, or "" when there is none.
	std::string FindLine(size_t mark, std::string_view prefix)
	{
		std::lock_guard<std::mutex> lock(s_log_mutex);
		for (size_t i = mark; i < s_log.size(); i++)
		{
			if (std::string_view(s_log[i]).starts_with(prefix))
				return s_log[i];
		}
		return std::string();
	}

	// The numbers in text, in order.
	std::vector<int> NumbersIn(std::string_view text)
	{
		std::vector<int> numbers;
		size_t i = 0;
		while (i < text.size())
		{
			if (text[i] < '0' || text[i] > '9')
			{
				i++;
				continue;
			}
			int value = 0;
			while (i < text.size() && text[i] >= '0' && text[i] <= '9')
				value = value * 10 + (text[i++] - '0');
			numbers.push_back(value);
		}
		return numbers;
	}

	struct SuiteResult
	{
		std::string name;
		bool passed = false;
		std::string detail;
	};
	std::vector<SuiteResult> s_results;

	void Begin(const char* what)
	{
		std::printf("\n--- %s\n", what);
		std::fflush(stdout);
	}

	void Record(const char* name, bool passed, std::string detail)
	{
		std::printf("[%s] %s: %s\n", passed ? "PASS" : "FAIL", name, detail.c_str());
		std::fflush(stdout);
		s_results.push_back({name, passed, std::move(detail)});
	}

	// A summary line as the result's detail, without the "(VR) " every VR line starts with.
	std::string Detail(const std::string& line)
	{
		if (line.empty())
			return "its summary line was not logged";
		return line.starts_with("(VR) ") ? line.substr(5) : line;
	}

	// A self-test that returns whether every check passed and logs one summary line starting with summary.
	void RunLogged(const char* name, bool (*test)(), const char* summary)
	{
		Begin(name);
		const size_t mark = LogMark();
		const bool passed = test();
		Record(name, passed, Detail(FindLine(mark, summary)));
	}

	// A self-test that returns whether every check passed and logs nothing.
	void RunSilent(const char* name, bool (*test)())
	{
		Begin(name);
		const bool passed = test();
		Record(name, passed, passed ? "passed (it returns pass or fail only, no check count)" : "FAIL (it names no check)");
	}

	// A self-test that only logs, a line "<summary>N ..." or "<summary>P passed, N failed": it passes when the line was
	// logged and the number at fail_index is 0 (with fail_index 1, after at least one passed check).
	void ReadCountLine(const char* name, size_t mark, const char* summary, size_t fail_index)
	{
		const std::string line = FindLine(mark, summary);
		const std::vector<int> n = line.empty() ? std::vector<int>() : NumbersIn(std::string_view(line).substr(std::string_view(summary).size()));
		const bool passed = n.size() > fail_index && n[fail_index] == 0 && (fail_index == 0 || n[0] > 0);
		Record(name, passed, Detail(line));
	}

	// A self-test that only logs, one line "<summary>: passed ..." or "<summary> FAIL: ...".
	void ReadPassedLine(const char* name, size_t mark, const char* summary)
	{
		const std::string line = FindLine(mark, summary);
		Record(name, line.starts_with(std::string(summary) + ": passed"), Detail(line));
	}

	// [VR] PhysicalMelee's caves, run as the EE would run them: the File #1 profile's camera.silence words in a small
	// interpreter of just the instructions they use (lui, ori, addiu, sltiu, sll, addu, subu, lw, lbu, sw, sh, sb, sd, lwc1, swc1, mul.s,
	// beq, bne, bgez, bgtz, blez, j, jal, jr, with delay slots; the FPU loads and stores only move bits). A stub stands in
	// for game code the caves call or jump to: Run asks it where control goes on from an address outside the cave.
	struct MiniEE
	{
		std::map<u32, u32> mem; // word address -> word
		u32 r[32] = {};
		u32 f[32] = {};
		// Game code outside the cave: true and *next to run it (a call returns to *next), false to stop there.
		bool (*stub)(MiniEE& ee, u32 pc, u32* next) = nullptr;

		u32 Load32(u32 a) const
		{
			const auto it = mem.find(a & ~3u);
			return it == mem.end() ? 0u : it->second;
		}
		void Store32(u32 a, u32 v) { mem[a & ~3u] = v; }
		void Store16(u32 a, u16 v)
		{
			const u32 sh = (a & 2u) * 8u;
			Store32(a, (Load32(a) & ~(0xFFFFu << sh)) | (static_cast<u32>(v) << sh));
		}
		static float F(u32 bits)
		{
			float x;
			std::memcpy(&x, &bits, 4);
			return x;
		}
		static u32 Bits(float x)
		{
			u32 b;
			std::memcpy(&b, &x, 4);
			return b;
		}
		void Store8(u32 a, u8 v)
		{
			const u32 sh = (a & 3u) * 8u;
			Store32(a, (Load32(a) & ~(0xFFu << sh)) | (static_cast<u32>(v) << sh));
		}

		// One instruction at pc. False for one it does not know. A branch or jump sets *branch, and *target when taken.
		bool Exec(u32 pc, bool* branch, bool* taken, u32* target)
		{
			const u32 i = Load32(pc);
			const u32 op = i >> 26, rs = (i >> 21) & 31u, rt = (i >> 16) & 31u, rd = (i >> 11) & 31u, fn = i & 63u;
			const u32 imm = i & 0xFFFFu;
			const u32 simm = static_cast<u32>(static_cast<s32>(static_cast<s16>(imm)));
			const u32 bt = pc + 4u + (simm << 2);
			*branch = false;
			*taken = false;
			switch (op)
			{
				case 0x00:
					if (i == 0)
						break;
					if (fn == 0x00)
						r[rd] = r[rt] << ((i >> 6) & 31u);
					else if (fn == 0x21)
						r[rd] = r[rs] + r[rt];
					else if (fn == 0x23)
						r[rd] = r[rs] - r[rt];
					else if (fn == 0x08)
					{
						*branch = *taken = true;
						*target = r[rs];
					}
					else
						return false;
					break;
				case 0x01:
					if (rt != 1)
						return false;
					*branch = true;
					*taken = static_cast<s32>(r[rs]) >= 0;
					*target = bt;
					break;
				case 0x02:
				case 0x03:
					*branch = *taken = true;
					*target = (pc & 0xF0000000u) | ((i & 0x3FFFFFFu) << 2);
					if (op == 0x03)
						r[31] = pc + 8u;
					break;
				case 0x04: *branch = true; *taken = r[rs] == r[rt]; *target = bt; break;
				case 0x05: *branch = true; *taken = r[rs] != r[rt]; *target = bt; break;
				case 0x06: *branch = true; *taken = static_cast<s32>(r[rs]) <= 0; *target = bt; break;
				case 0x07: *branch = true; *taken = static_cast<s32>(r[rs]) > 0; *target = bt; break;
				case 0x09: r[rt] = r[rs] + simm; break;
				case 0x0B: r[rt] = r[rs] < simm ? 1u : 0u; break;
				case 0x0D: r[rt] = r[rs] | imm; break;
				case 0x0F: r[rt] = imm << 16; break;
				case 0x23: r[rt] = Load32(r[rs] + simm); break;
				case 0x21: r[rt] = static_cast<u32>(static_cast<s32>(static_cast<s16>(static_cast<u16>(Load32(r[rs] + simm) >> (((r[rs] + simm) & 2u) * 8u))))); break; // lh
				case 0x24: r[rt] = (Load32(r[rs] + simm) >> (((r[rs] + simm) & 3u) * 8u)) & 0xFFu; break;
				case 0x2B: Store32(r[rs] + simm, r[rt]); break;
				case 0x29: Store16(r[rs] + simm, static_cast<u16>(r[rt])); break;
				case 0x28: Store8(r[rs] + simm, static_cast<u8>(r[rt])); break;
				case 0x3F: Store32(r[rs] + simm, r[rt]); Store32(r[rs] + simm + 4u, 0); break; // sd: these values are 32-bit
				case 0x11:
					// mul.s fd, fs, ft only
					if (((i >> 21) & 31u) != 0x10 || fn != 0x02)
						return false;
					f[(i >> 6) & 31u] = Bits(F(f[rd]) * F(f[rt]));
					break;
				case 0x31: f[rt] = Load32(r[rs] + simm); break;
				case 0x39: Store32(r[rs] + simm, f[rt]); break;
				default: return false;
			}
			r[0] = 0;
			return true;
		}

		// From entry until control leaves [lo, hi) (after a branch's delay slot): where it went, or 0 on an unknown
		// instruction, a branch in a delay slot or more than 500 steps.
		u32 Run(u32 entry, u32 lo, u32 hi)
		{
			u32 pc = entry;
			for (int n = 0; n < 500; n++)
			{
				if (pc < lo || pc >= hi)
				{
					u32 next = 0;
					if (stub && stub(*this, pc, &next))
					{
						pc = next;
						continue;
					}
					return pc;
				}
				bool br = false, tk = false, br2 = false, tk2 = false;
				u32 tg = 0, tg2 = 0;
				if (!Exec(pc, &br, &tk, &tg))
					return 0;
				if (!br)
				{
					pc += 4;
					continue;
				}
				if (!Exec(pc + 4, &br2, &tk2, &tg2) || br2)
					return 0;
				pc = tk ? tg : pc + 8;
			}
			return 0;
		}
	};

	// The caves' memory: the profile's cave words, the host's block (magic, the stamp, the local record, the VR weapon's
	// matrix and position), the game counter, and a weapon object (the canned pose) with its own matrix and position.
	constexpr u32 kLocalRecord = 0x476DD0, kPartnerRecord = 0x476DD0 + 0x10E0, kWeapon = 0x500000, kAttack = 0x600000;
	constexpr u32 kReturn = 0x00ABC0, kSpawnEvent = 0x565C80;
	bool MeleeCaveMemory(MiniEE& ee, bool magic, u8 counter, u8 stamp)
	{
		const VR::ProfileDB::Profile* p = VR::ProfileDB::Lookup("SLPM-65428", 0x32088394u);
		if (!p || !p->camera.has_value())
			return false;
		int words = 0;
		for (const VR::ProfileDB::CameraSilence& sil : p->camera->silence)
		{
			if (sil.when_setting == "PhysicalMelee" && sil.ee_address >= 0xFE000u && sil.ee_address < 0xFF000u)
			{
				ee.Store32(sil.ee_address, sil.value_on);
				words++;
			}
		}
		ee.Store32(0xFF000, magic ? 0x4D454C31u : 0u);
		ee.Store8(0xFF004, stamp);
		ee.Store32(0xFF008, kLocalRecord);
		for (u32 k = 0; k < 16; k++)
		{
			ee.Store32(0xFF040 + 4 * k, 0x1000 + k);      // the VR weapon's matrix
			ee.Store32(kWeapon + 0x1C0 + 4 * k, 0x4000 + k); // the canned one
		}
		for (u32 k = 0; k < 3; k++)
		{
			ee.Store32(0xFF080 + 4 * k, 0x2000 + k);
			ee.Store32(kWeapon + 0x200 + 4 * k, 0x3000 + k);
		}
		ee.Store8(0x48BF93, counter);
		return words == 39 + 23 + 22;
	}

	// Cave A (every frame of the swing): the attack object's position and matrix come from the VR weapon (from_vr) or
	// from the weapon object, exactly as the game copies them.
	bool MeleeCaveA(u32 owner, bool magic, u8 counter, u8 stamp, bool from_vr)
	{
		MiniEE ee;
		if (!MeleeCaveMemory(ee, magic, counter, stamp))
			return false;
		ee.r[4] = kAttack;  // a0: the attack object
		ee.r[6] = owner;    // a2: its owner (the delay slot of the game's own branch set it)
		ee.r[8] = kWeapon;  // t0: the weapon object
		ee.r[31] = kReturn;
		ee.r[16] = 0x11111111;
		if (ee.Run(0xFE000, 0xFE000, 0xFE0A0) != kReturn || ee.r[16] != 0x11111111)
			return false;
		const u32 pos = from_vr ? 0x2000 : 0x3000, mat = from_vr ? 0x1000 : 0x4000;
		for (u32 k = 0; k < 3; k++)
			if (ee.Load32(kAttack + 0x38 + 4 * k) != pos + k)
				return false;
		for (u32 k = 0; k < 16; k++)
			if (ee.Load32(kAttack + 0x50 + 4 * k) != mat + k)
				return false;
		return true;
	}

	// Cave B (the attack object is built): v1 the object to copy from, and its +0x200 already in f2.
	bool MeleeCaveB(u32 owner, bool magic, bool from_vr)
	{
		MiniEE ee;
		if (!MeleeCaveMemory(ee, magic, 10, 9))
			return false;
		ee.Store32(0x700000, kWeapon);
		ee.r[16] = 0x700000; // s0: the item's weapon object pointer
		ee.r[21] = owner;    // s5: the record that swings
		ee.r[31] = kReturn;
		if (ee.Run(0xFE0A0, 0xFE0A0, 0xFE100) != kReturn)
			return false;
		const u32 obj = from_vr ? 0xFEE80u : kWeapon;
		return ee.r[3] == obj && ee.f[2] == (from_vr ? 0x2000u : 0x3000u);
	}

	// Cave C (the spawn event of a melee motion): on to FUN_00565C80 with the frame (a0) 2 or as the game had it.
	bool MeleeCaveC(u32 record, bool magic, u8 counter, u8 stamp, bool early)
	{
		MiniEE ee;
		if (!MeleeCaveMemory(ee, magic, counter, stamp))
			return false;
		ee.r[4] = 16;      // a0: the motion's spawn frame (a pipe's)
		ee.r[5] = record;  // a1: the record
		ee.r[6] = 2;       // a2: the event code (spawn the attack)
		ee.r[7] = 0;
		ee.r[31] = 0x56950C;
		ee.r[16] = record;
		if (ee.Run(0xFE100, 0xFE100, 0xFE160) != kSpawnEvent)
			return false;
		return ee.r[4] == (early ? 2u : 16u) && ee.r[5] == record && ee.r[6] == 2 && ee.r[7] == 0 &&
		       ee.r[31] == 0x56950C && ee.r[16] == record;
	}

	// Contact mode's caves D and E (the File #1 profile's PhysicalMeleeContact words) in the same interpreter. The block
	// and the game around them are scripted: the attack spawn FUN_005521B0 (a stub that appends a node and an attack
	// object owned by a0, when told to), the node list's tail, and FUN_00636990's own body (a stub returning 2.0).
	constexpr u32 kBlock = 0xFF000, kTail = 0x23BD9C, kNode = 0x3B9450, kObject = 0x3C4FC0, kItem = 11;
	struct ContactCaveRun
	{
		bool spawned = false;   // the stub ran
		u32 a[4] = {};          // its arguments
	};
	ContactCaveRun s_contact_run;
	bool s_contact_spawn_ok = true; // the stub appends a node (else the game refused: no weapon object)
	struct CaveECall
	{
		bool called = false;
		u32 sp = 0, ra = 0, a0 = 0, a1 = 0;
	};
	CaveECall s_cave_e; // what FUN_00636990's body (the stub) was entered with
	struct CaveFCall
	{
		u32 pc = 0, a1 = 0, a2 = 0, action = 0, ba0 = 0;
		u16 last_type = 0;
	};
	std::vector<CaveFCall> s_cave_f_calls; // cave F's calls into the game, in order
	u32 s_cave_f_filter = 0;               // what the filter FUN_006337C0 answers (1: the game would not send)
	int s_hit_tests = 0;                   // how often the frame's hit test FUN_005A9DB0 ran
	struct HitNotice
	{
		u8 notice = 0, kind = 0, idx = 0;
	};
	HitNotice s_hit_notice; // what the hit test writes into the local record (notice 0: nothing hit)

	bool ContactStub(MiniEE& ee, u32 pc, u32* next)
	{
		if (pc == 0x5521B0)
		{
			s_contact_run.spawned = true;
			for (int k = 0; k < 4; k++)
				s_contact_run.a[k] = ee.r[4 + k];
			if (s_contact_spawn_ok)
			{
				ee.Store32(kTail, kNode);
				ee.Store32(kNode + 0x08, kObject);
				ee.Store32(kObject + 0x28, ee.r[4]);
				ee.Store32(kObject + 0x10, 0xFFFF0000u); // +0x12 life -1, as the game leaves it
			}
			for (int k : {1, 2, 3, 8, 9, 10, 11, 12, 13})
				ee.r[k] = 0xDEAD0000u + static_cast<u32>(k); // the callee may clobber any temporary
			*next = ee.r[31];
			return true;
		}
		if (pc == 0x5A9DB0)
		{
			// The frame's hit test (cave F calls it, or cave D or F jumps on to it): a strike's hit writes its notice
			// into the attacker's record, as FUN_00548E40 does (+0x116 + 1, then the target's kind and index).
			s_hit_tests++;
			if (s_hit_notice.notice)
			{
				ee.Store8(kLocalRecord + 0x116, s_hit_notice.notice);
				ee.Store8(kLocalRecord + 0x117, s_hit_notice.kind);
				ee.Store8(kLocalRecord + 0x118, s_hit_notice.idx);
			}
			for (int k : {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13})
				ee.r[k] = 0xDEAD0000u + static_cast<u32>(k);
			*next = ee.r[31];
			return true;
		}
		if (pc == 0x64C620 || pc == 0x6337C0 || pc == 0x62CDA0)
		{
			// Cave F's calls: the state-message sender (what it would copy: the record's action bytes +0x08-+0x0B and
			// the message type, with the frame's last-type halfword), the game's filter (its answer scripted), and the
			// item-use mark FUN_0062CDA0.
			CaveFCall c;
			c.pc = pc;
			c.a1 = ee.r[5];
			c.a2 = ee.r[6];
			c.action = ee.Load32(ee.r[4] + 0x08);
			c.ba0 = ee.Load32(ee.r[4] + 0xBA0);
			c.last_type = static_cast<u16>(ee.Load32(0x23BF0C));
			s_cave_f_calls.push_back(c);
			if (pc == 0x64C620)
			{
				ee.Store32(ee.r[4] + 0xBA0, ee.Load32(ee.r[4] + 0xBA0) | 0x08000000u); // the sender's own +0xBA0 bit 27
				ee.Store16(0x23BF0C, static_cast<u16>(ee.r[5]));                       // and its last-type mark
			}
			ee.r[2] = pc == 0x6337C0 ? s_cave_f_filter : 0u;
			for (int k : {1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13})
				ee.r[k] = 0xDEAD0000u + static_cast<u32>(k);
			*next = ee.r[31];
			return true;
		}
		if (pc == 0x636998)
		{
			// FUN_00636990 after its first two words (in cave E's trampoline): returns its multiplier.
			s_cave_e.called = true;
			s_cave_e.sp = ee.r[29];
			s_cave_e.ra = ee.Load32(ee.r[29] + 0x20);
			s_cave_e.a0 = ee.r[4];
			s_cave_e.a1 = ee.r[5];
			ee.f[0] = MiniEE::Bits(2.0f);
			ee.r[31] = ee.Load32(ee.r[29] + 0x20);
			ee.r[29] += 48;
			*next = ee.r[31];
			return true;
		}
		return false;
	}

	bool ContactCaveMemory(MiniEE& ee)
	{
		const VR::ProfileDB::Profile* p = VR::ProfileDB::Lookup("SLPM-65428", 0x32088394u);
		if (!p || !p->camera.has_value())
			return false;
		int words = 0;
		for (const VR::ProfileDB::CameraSilence& sil : p->camera->silence)
		{
			if (sil.when_setting == "PhysicalMeleeContact" && sil.ee_address >= 0xFE000u && sil.ee_address < 0xFF000u)
			{
				ee.Store32(sil.ee_address, sil.value_on);
				words++;
			}
		}
		ee.stub = ContactStub;
		ee.r[29] = 0x01F00000; // sp
		return words > 0;
	}

	struct CaveDCase
	{
		bool magic = true;
		u8 counter = 30, stamp = 29, strike_stamp = 28;
		u32 seq = 1, served = 0, left = 0, live_obj = 0;
		u8 stance = 1, sub = 0;
		bool spawn_ok = true;
	};
	struct CaveDResult
	{
		bool ok = false; // left for FUN_005A9DB0 with ra and sp kept
		bool spawned = false;
		u32 served = 0, left = 0, obj = 0, made = 0;
		u16 life = 0;
		u32 args[4] = {};
	};
	CaveDResult RunCaveD(const CaveDCase& c)
	{
		CaveDResult res;
		MiniEE ee;
		if (!ContactCaveMemory(ee))
			return res;
		s_contact_run = {};
		s_contact_spawn_ok = c.spawn_ok;
		ee.Store32(kBlock, c.magic ? 0x4D454C31u : 0u);
		ee.Store8(kBlock + 0x04, c.stamp);
		ee.Store32(kBlock + 0x08, kLocalRecord);
		ee.Store32(kBlock + 0x0C, c.seq);
		ee.Store32(kBlock + 0x10, c.served);
		ee.Store32(kBlock + 0x14, c.live_obj);
		ee.Store32(kBlock + 0x18, 6);
		ee.Store32(kBlock + 0x20, c.left);
		ee.Store8(kBlock + 0x28, c.strike_stamp);
		ee.Store8(0x48BF93, c.counter);
		ee.Store8(kLocalRecord + 0x08, c.stance);
		ee.Store8(kLocalRecord + 0x09, c.sub);
		ee.Store8(kLocalRecord + 0xC7C, kItem);
		ee.Store32(kTail, 0x3B0000); // the tail before the spawn
		ee.r[31] = 0x5CDF6C;
		const u32 sp = ee.r[29];
		ee.r[16] = 0x11111111;
		s_hit_tests = 0;
		s_hit_notice = {};
		const u32 out = ee.Run(0xFE200, 0xFE200, 0xFE800); // cave D, then cave F (nothing for it to do here), then back
		res.ok = out == 0x5CDF6C && s_hit_tests == 1 && ee.r[29] == sp && ee.r[16] == 0x11111111;
		res.spawned = s_contact_run.spawned;
		for (int k = 0; k < 4; k++)
			res.args[k] = s_contact_run.a[k];
		res.served = ee.Load32(kBlock + 0x10);
		res.obj = ee.Load32(kBlock + 0x14);
		res.left = ee.Load32(kBlock + 0x20);
		res.made = ee.Load32(kBlock + 0x24);
		res.life = static_cast<u16>(ee.Load32(kObject + 0x10) >> 16);
		return res;
	}

	// Cave E for a node: the multiplier it returns (f0), and whether the game's own body ran as if called directly
	// (*original: its frame made by its own first two words, returning straight to the caller) or inside cave E.
	bool RunCaveE(bool magic, u32 live_obj, u32 node_obj, float strength, float* f0, bool* original)
	{
		MiniEE ee;
		if (!ContactCaveMemory(ee))
			return false;
		s_cave_e = {};
		ee.Store32(kBlock, magic ? 0x4D454C31u : 0u);
		ee.Store32(kBlock + 0x14, live_obj);
		ee.Store32(kBlock + 0x1C, MiniEE::Bits(strength));
		ee.Store32(kNode + 0x08, node_obj);
		ee.r[4] = kLocalRecord;
		ee.r[5] = kNode;
		ee.r[31] = kReturn;
		const u32 sp = ee.r[29];
		const u32 out = ee.Run(0xFE400, 0xFE400, 0xFE500);
		*f0 = MiniEE::F(ee.f[0]);
		*original = s_cave_e.sp == sp - 48 && s_cave_e.ra == kReturn;
		return out == kReturn && ee.r[29] == sp && s_cave_e.called && s_cave_e.a0 == kLocalRecord && s_cave_e.a1 == kNode;
	}

	// Cave F (after cave D, every frame): the strike's hit notice spent as the swing's own stage FUN_006474A0 does it, and
	// online the partners' swing (a state message with the swing's action bytes, then its end).
	constexpr u32 kTypeTable = 0x6E5940, kLocalSlot = 0x48BF7D;
	struct CaveFCase
	{
		u8 weapon = 34;                      // the record's weapon type (+0xCA8): 34 iron pipe (class 4)
		u8 notice = 1, kind = 2, idx = 49;   // +0x116, +0x117, +0x118 (the enemy hit); 0xFF, 0xFF in +0x119, +0x11A
		bool all_ff = false;                 // +0x117-+0x11A all 0xFF (a hit that told the owner: FUN_00548FC0)
		bool before = false;                 // the notice already there before this frame's hit test (else written by it)
		u32 watch = 9, made = 1, seen = 1;   // block +0x38, +0x24 (cave D's strikes), +0x34 (cave F's)
		u32 partner = 0, left = 0;           // block +0x30 (the host: partners' swing frames, 0 off), +0x2C
		u8 stance = 1, sub = 0, slot = 0, local_slot = 0;
		u32 ba0 = 0x00000100;
		u32 filter = 0;
	};
	struct CaveFResult
	{
		bool ok = false;
		u8 notice = 0, kind = 0;
		u32 ba0 = 0, action = 0, uses = 0, left = 0, watch = 0, seen = 0, sends = 0;
		u16 last_type = 0;
		std::vector<CaveFCall> calls;
	};
	CaveFResult RunCaveF(const CaveFCase& c)
	{
		CaveFResult res;
		MiniEE ee;
		if (!ContactCaveMemory(ee))
			return res;
		s_cave_f_calls.clear();
		s_cave_f_filter = c.filter;
		ee.Store32(kBlock, 0x4D454C31u);
		ee.Store32(kBlock + 0x08, kLocalRecord);
		ee.Store32(kBlock + 0x18, 6);
		ee.Store32(kBlock + 0x24, c.made);
		ee.Store32(kBlock + 0x2C, c.left);
		ee.Store32(kBlock + 0x30, c.partner);
		ee.Store32(kBlock + 0x34, c.seen);
		ee.Store32(kBlock + 0x38, c.watch);
		const u32 R = kLocalRecord;
		ee.Store8(R + 0x02, 0);
		ee.Store8(R + 0x03, c.slot);
		ee.Store8(kLocalSlot, c.local_slot);
		ee.Store8(R + 0x08, c.stance);
		ee.Store8(R + 0x09, c.sub);
		ee.Store8(R + 0x0A, 7);
		ee.Store8(R + 0x0B, 1);
		s_hit_tests = 0;
		s_hit_notice = {};
		if (c.before)
		{
			ee.Store8(R + 0x116, c.notice);
			ee.Store8(R + 0x117, c.all_ff ? 0xFF : c.kind);
			ee.Store8(R + 0x118, c.all_ff ? 0xFF : c.idx);
		}
		else
		{
			s_hit_notice.notice = c.notice;
			s_hit_notice.kind = c.all_ff ? 0xFF : c.kind;
			s_hit_notice.idx = c.all_ff ? 0xFF : c.idx;
			ee.Store8(R + 0x117, 0xFF);
			ee.Store8(R + 0x118, 0xFF);
		}
		ee.Store8(R + 0x119, 0xFF);
		ee.Store8(R + 0x11A, 0xFF);
		ee.Store32(R + 0xBA0, c.ba0);
		ee.Store32(R + 0xCA8, c.weapon);
		ee.Store16(0x23BF0C, 0x0007); // the frame's last message type, whatever it was
		static constexpr u8 kClass[][2] = {{32, 1}, {33, 1}, {34, 4}, {35, 4}, {41, 5}, {42, 4}, {44, 5}, {1, 2}};
		for (const auto& k : kClass)
			ee.Store8(kTypeTable + 0x44u * k[0] + 0x05u, k[1]);
		ee.r[31] = 0x5CDF6C;
		const u32 sp = ee.r[29];
		ee.r[16] = 0x11111111;
		const u32 out = ee.Run(0xFE500, 0xFE500, 0xFE800);
		res.ok = out == 0x5CDF6C && s_hit_tests == 1 && ee.r[29] == sp && ee.r[16] == 0x11111111;
		res.notice = static_cast<u8>(ee.Load32(R + 0x114) >> 16);
		res.kind = static_cast<u8>(ee.Load32(R + 0x114) >> 24);
		res.ba0 = ee.Load32(R + 0xBA0);
		res.action = ee.Load32(R + 0x08);
		res.uses = ee.Load32(kBlock + 0x3C);
		res.left = ee.Load32(kBlock + 0x2C);
		res.watch = ee.Load32(kBlock + 0x38);
		res.seen = ee.Load32(kBlock + 0x34);
		res.sends = ee.Load32(kBlock + 0x8C);
		res.last_type = static_cast<u16>(ee.Load32(0x23BF0C));
		res.calls = s_cave_f_calls;
		return res;
	}

	bool MeleeCaveF()
	{
		bool ok = true;
		const auto check = [&ok](bool c) { ok = ok && c; };
		const u32 real = 0x01070001u; // stance 1, sub 0, +0x0A 7, +0x0B 1
		{
			// A pipe's strike hit an enemy: the notice cleared (the rest kept), +0xBA0 bit 31 and FUN_0062CDA0, as the
			// swing's stage does, so the game's next character loop spends the durability.
			const CaveFResult r = RunCaveF(CaveFCase{});
			check(r.ok && r.notice == 0 && r.kind == 2 && r.ba0 == 0x80000100u && r.uses == 1 && r.watch == 8 &&
				  r.calls.size() == 1 && r.calls[0].pc == 0x62CDA0 && r.calls[0].ba0 == 0x80000100u);
		}
		for (u8 w : {35, 41, 42, 44})
		{
			CaveFCase c;
			c.weapon = w; // the curved pipe, the brush, the pole, the spear: the same
			const CaveFResult r = RunCaveF(c);
			check(r.ok && r.uses == 1 && r.ba0 == 0x80000100u);
		}
		for (u8 w : {32, 33, 1})
		{
			CaveFCase c;
			c.weapon = w; // the knives (and a gun): the notice cleared, nothing spent, as in the game
			const CaveFResult r = RunCaveF(c);
			check(r.ok && r.notice == 0 && r.uses == 0 && r.ba0 == 0x100u && r.calls.empty());
		}
		{
			CaveFCase c;
			c.all_ff = true; // a hit that told the owner (code 1 + 1 = 2): the pipe is used too
			check(RunCaveF(c).uses == 1);
			c.notice = 2; // code 3: nothing
			const CaveFResult r = RunCaveF(c);
			check(r.ok && r.notice == 0 && r.uses == 0 && r.calls.empty());
		}
		{
			CaveFCase c;
			c.stance = 2;
			c.sub = 3; // the canned swing: its own stage reads the notice
			CaveFResult r = RunCaveF(c);
			check(r.ok && r.notice == 1 && r.uses == 0);
			c.sub = 1; // aim idle: spent
			check(RunCaveF(c).uses == 1);
			c = CaveFCase{};
			c.ba0 = 0x80000100u; // a use already pending: next frame
			r = RunCaveF(c);
			check(r.ok && r.notice == 1 && r.uses == 0 && r.calls.empty());
			c = CaveFCase{};
			c.watch = 0; // no strike lately: the notice is not this mode's
			r = RunCaveF(c);
			check(r.ok && r.notice == 1 && r.uses == 0);
			c = CaveFCase{};
			c.notice = 0; // no hit: nothing
			r = RunCaveF(c);
			check(r.ok && r.uses == 0 && r.watch == 8 && r.calls.empty());
			c = CaveFCase{};
			c.slot = 1; // not this console's slot (the game's own test): cleared, not spent
			r = RunCaveF(c);
			check(r.ok && r.notice == 0 && r.uses == 0 && r.calls.empty());
		}
		{
			// A new strike (cave D's count moved): watched for its window + 3 frames; offline (partner 0) nothing sent.
			CaveFCase c;
			c.made = 2;
			c.notice = 0;
			CaveFResult r = RunCaveF(c);
			check(r.ok && r.seen == 2 && r.watch == 8 && r.calls.empty() && r.left == 0 && r.sends == 0);
			// Online: one type-1 message built with stance 2, sub 3, phase 0, then everything put back.
			c.partner = 30;
			r = RunCaveF(c);
			check(r.ok && r.calls.size() == 1 && r.calls[0].pc == 0x64C620 && r.calls[0].a1 == 1 &&
				  r.calls[0].action == 0x00000302u && r.calls[0].last_type == 0 && r.action == real && r.ba0 == 0x100u &&
				  r.last_type == 0x0007 && r.left == 30 && r.sends == 1);
			c.left = 12; // a swing still showing: no second one
			r = RunCaveF(c);
			check(r.ok && r.calls.empty() && r.left == 11);
			c.left = 0;
			c.stance = 3; // hurt: no swing
			r = RunCaveF(c);
			check(r.ok && r.calls.empty() && r.left == 0);
			c.stance = 2;
			c.sub = 1; // aim idle: the swing
			r = RunCaveF(c);
			check(r.ok && r.calls.size() == 1 && r.calls[0].action == 0x00000302u && r.action == 0x01070102u);
		}
		{
			// The swing's end for partners after its frames: the game's filter as for a swing ending (2, 3), then the
			// message with the real action bytes, unless the filter drops it (aim idle) or the record is hurt.
			CaveFCase c;
			c.notice = 0;
			c.left = 1;
			CaveFResult r = RunCaveF(c);
			check(r.ok && r.left == 0 && r.calls.size() == 2 && r.calls[0].pc == 0x6337C0 && r.calls[0].a1 == 2 &&
				  r.calls[0].a2 == 3 && r.calls[1].pc == 0x64C620 && r.calls[1].a1 == 1 && r.calls[1].action == real &&
				  r.calls[1].last_type == 0 && r.last_type == 0x0007 && r.sends == 0x100);
			c.filter = 1;
			r = RunCaveF(c);
			check(r.ok && r.left == 0 && r.calls.size() == 1 && r.calls[0].pc == 0x6337C0 && r.sends == 0);
			c.filter = 0;
			c.stance = 4;
			r = RunCaveF(c);
			check(r.ok && r.left == 0 && r.calls.empty());
			c.stance = 1;
			c.left = 5; // still counting
			r = RunCaveF(c);
			check(r.ok && r.left == 4 && r.calls.empty());
		}
		return ok;
	}

	bool MeleeContactCaves()
	{
		bool ok = true;
		const auto check = [&ok](bool c) { ok = ok && c; };
		{
			// A fresh request, the record idle: the game's spawn with (record, +0x50, +0x38, its item), the object's life the
			// window, the strike noted.
			const CaveDResult r = RunCaveD(CaveDCase{});
			check(r.ok && r.spawned && r.args[0] == kLocalRecord && r.args[1] == kLocalRecord + 0x50 &&
				  r.args[2] == kLocalRecord + 0x38 && r.args[3] == kItem && r.served == 1 && r.obj == kObject && r.left == 6 &&
				  r.made == 1 && r.life == 6);
		}
		{
			CaveDCase c;
			c.served = 1; // already served
			const CaveDResult r = RunCaveD(c);
			check(r.ok && !r.spawned && r.obj == 0 && r.made == 0);
		}
		{
			CaveDCase c;
			c.stance = 2;
			c.sub = 3; // the canned swing runs: consumed, not spawned
			const CaveDResult r = RunCaveD(c);
			check(r.ok && !r.spawned && r.served == 1);
			c.sub = 1; // aim idle: spawned
			check(RunCaveD(c).spawned);
		}
		{
			CaveDCase c;
			c.strike_stamp = 20; // 10 frames old: consumed, not spawned
			const CaveDResult r = RunCaveD(c);
			check(r.ok && !r.spawned && r.served == 1);
			c.counter = 2;
			c.stamp = 1;
			c.strike_stamp = 58; // 4 frames old across the wrap: spawned
			check(RunCaveD(c).spawned);
		}
		{
			CaveDCase c;
			c.magic = false;
			const CaveDResult r = RunCaveD(c);
			check(r.ok && !r.spawned && r.served == 0);
			c.magic = true;
			c.stamp = 10; // the block itself stale (20 frames): nothing taken
			const CaveDResult s = RunCaveD(c);
			check(s.ok && !s.spawned && s.served == 0);
		}
		{
			CaveDCase c;
			c.left = 3;
			c.live_obj = kObject; // a strike live: counted down, the new request consumed but not spawned
			const CaveDResult r = RunCaveD(c);
			check(r.ok && !r.spawned && r.left == 2 && r.obj == kObject && r.served == 1);
			c.left = 1;
			c.served = 1; // the last frame: the strike is over
			const CaveDResult s = RunCaveD(c);
			check(s.ok && s.left == 0 && s.obj == 0);
			c.stamp = 0; // ... even with a stale block
			const CaveDResult t = RunCaveD(c);
			check(t.ok && t.left == 0 && t.obj == 0);
		}
		{
			CaveDCase c;
			c.spawn_ok = false; // the game made no node (no weapon object): nothing noted
			const CaveDResult r = RunCaveD(c);
			check(r.ok && r.spawned && r.obj == 0 && r.left == 0 && r.made == 0);
		}
		{
			float f0 = 0.0f;
			bool original = true;
			check(RunCaveE(true, kObject, kObject, 1.5f, &f0, &original) && !original && std::fabs(f0 - 3.0f) < 1e-6f);
			check(RunCaveE(true, kObject, kObject + 0x200, 1.5f, &f0, &original) && original && f0 == 2.0f);
			check(RunCaveE(false, kObject, kObject, 1.5f, &f0, &original) && original && f0 == 2.0f);
			check(RunCaveE(true, 0, 0, 1.5f, &f0, &original) && original && f0 == 2.0f);
		}
		check(MeleeCaveF());
		return ok;
	}

	void RunGamepad()
	{
		Begin("Gamepad (SpatialControls::SelfTestGamepad)");
		const char* failed = nullptr;
		const bool passed = VR::SpatialControls::SelfTestGamepad(&failed);
		Record("Gamepad", passed,
			passed ? std::string("passed (it names the first failed check only, no check count)") :
					 std::string("FAIL: ") + (failed ? failed : "?"));
	}

	void RunAutoStereo()
	{
		Begin("Auto-stereo (StereoAuto::MaybeRunSelfTest, PCSX2_VR_SELFTEST set)");
		const size_t mark = LogMark();
		VR::StereoAuto::MaybeRunSelfTest();
		ReadCountLine("Auto-stereo", mark, "(VR) Auto-stereo self-test: ", 0);
	}

	void RunCameraDriverLogged()
	{
		Begin("CameraDriver's logged self-tests (CameraDriver::Apply with no VM, PCSX2_VR_SELFTEST set)");
		const size_t mark = LogMark();
		VR::CameraDriver::Apply();
		ReadCountLine("CameraDriver matrix", mark, "(VR) CameraDriver matrix self-test: ", 1);
		ReadCountLine("CameraDriver hook", mark, "(VR) CameraDriver hook self-test: ", 0);
		ReadCountLine("CameraDriver predict", mark, "(VR) CameraDriver predict self-test: ", 0);
		ReadCountLine("CameraDriver comfort", mark, "(VR) CameraDriver comfort self-test: ", 0);
		ReadCountLine("CameraDriver FOV", mark, "(VR) CameraDriver FOV self-test: ", 0);
		ReadPassedLine("Gamepad (CameraDriver's run)", mark, "(VR) Gamepad self-test");
	}

#ifdef ENABLE_VULKAN
	void RunCompositorLogged()
	{
		Begin("XRCompositor's self-tests (XRCompositor::Initialize with no XR session, PCSX2_VR_SELFTEST set)");
		const size_t mark = LogMark();
		if (VR::XRCompositor::Initialize())
			VR::XRCompositor::Shutdown(); // not reached: this process has no XR session
		ReadPassedLine("Menu pointer (pacer)", mark, "(VR) Menu pointer (pacer) self-test");
		ReadCountLine("Edge fill plan", mark, "(VR) Edge fill self-test: ", 1);
	}
#endif
} // namespace

int main(int argc, char* argv[])
{
	// The emulator runs the logged self-tests below only with this set; read where they are called, not at startup.
#ifdef _WIN32
	_putenv_s("PCSX2_VR_SELFTEST", "1");
#else
	setenv("PCSX2_VR_SELFTEST", "1", 1);
#endif
	Log::SetHostOutputLevel(LOGLEVEL_DEV, OnLog);

	// The item names and pouch self-tests check the shipped profiles (resources/vr-profiles); no user profile folder.
	EmuFolders::Resources = (argc > 1) ? std::string(argv[1]) : std::string(VR_SELFTESTS_RESOURCES);
	const std::string profiles = Path::Combine(EmuFolders::Resources, "vr-profiles");
	if (!FileSystem::DirectoryExists(profiles.c_str()))
	{
		std::fprintf(stderr, "vr_selftests: no shipped profiles at %s (give the resources directory as the argument).\n",
			profiles.c_str());
		return 2;
	}
	std::printf("vr_selftests: the shipped profiles from %s\n", profiles.c_str());
	std::fflush(stdout);

	RunLogged("MenuPointer", VR::MenuPointer::SelfTest, "(VR) MenuPointer self-test");
	RunLogged("Item names", VR::GameFeedback::ItemSelfTest, "(VR) Item names self-test");
	RunLogged("BeltPouches", VR::BeltPouches::SelfTest, "(VR) BeltPouches self-test");
	RunLogged("HUD", VR::HudCards::SelfTest, "(VR) HUD self-test: ");
	RunLogged("Feedback", VR::GameFeedback::SelfTest, "(VR) Feedback self-test: ");
	RunLogged("Hands", VR::HandModel::SelfTest, "(VR) Hands self-test: ");
	RunLogged("Pickup cue and hand targeting", VR::ItemReach::SelfTest, "(VR) Pickup cue self-test");
	RunLogged("Zone markers", VR::ZoneMarkers::SelfTest, "(VR) Zone markers self-test");
	RunLogged("MeshRaster", +[]() { return VR::MeshRaster::SelfTest() == 0; }, "(VR) MeshRaster self-test");
	{
		Begin("Reload gesture (ReloadGesture::SelfTest)");
		const std::vector<std::string> failed = VR::ReloadGesture::SelfTest();
		Record("Reload gesture", failed.empty(),
			failed.empty() ? std::string("passed (it names the failed checks only, no check count)") :
							 "FAIL: " + std::to_string(failed.size()) + " check(s), first: " + failed.front());
	}
	RunLogged("Depth export", VR::DepthExport::SelfTest, "(VR) Depth export self-test");
	RunLogged("Depth warp", VR::DepthWarp::SelfTest, "(VR) Depth warp self-test");
	RunLogged("Menu panel", VR::MenuPanel::SelfTest, "(VR) Menu panel self-test");
	RunLogged("Menu panel room and placement", VR::MenuPanel::FeelSelfTest, "(VR) Menu panel feel self-test");
	// [VR] GameWeaponModels: File #1's profile names the game's weapon-type table and the scenario's item table, with the
	// layout the weapon models are placed by (tools/outbreak-vr/research/2026-10-14-weapon-models-in-hand.md).
	RunSilent("Weapon hold (File #1 profile, feedback.weapon.hold)", +[]() {
		const VR::ProfileDB::Profile* p = VR::ProfileDB::Lookup("SLPM-65428", 0x32088394u);
		if (!p || !p->feedback.has_value())
			return false;
		const VR::ProfileDB::FeedbackParams& f = p->feedback.value();
		return f.has_weapon_hold && f.has_hold_items && f.hold_type_offset == 0xCA8 && f.hold_table == 0x6E5944 &&
		       f.hold_stride == 0x44 && f.hold_grip_offset == 0x0C && f.hold_rotation_offset == 0x24 &&
		       f.hold_muzzle_offset == 0x34 && f.hold_items == 0x716980 && f.hold_item_stride == 0x18 &&
		       f.hold_item_id == 0x12 && f.hold_item_type == 0x0F;
	});
	// [VR] SeamlessDoors: File #1's camera.silence entries gated on the switch (whenSetting), the arrival ones kept
	// through disarm; each NOPs one jal 0x590B40 (tools/outbreak-vr/research/2026-10-14-doors.md).
	RunSilent("Seamless doors (File #1 profile, camera.silence whenSetting)", +[]() {
		const VR::ProfileDB::Profile* p = VR::ProfileDB::Lookup("SLPM-65428", 0x32088394u);
		if (!p || !p->camera.has_value() || !VR::ProfileDB::IsSilenceSettingName("SeamlessDoors") ||
			VR::ProfileDB::IsSilenceSettingName("NoSuchSwitch"))
			return false;
		int gated = 0, kept = 0;
		for (const VR::ProfileDB::CameraSilence& sil : p->camera->silence)
		{
			if (sil.when_setting.empty() || sil.when_setting == "PhysicalMelee" || sil.when_setting == "PhysicalMeleeContact" ||
				sil.when_setting == "MeleeHitJolt")
			{
				if (sil.keep_through_disarm)
					return false;
				continue;
			}
			if (sil.when_setting != "SeamlessDoors" || !sil.offline_only || sil.value_off != 0x0C1642D0u ||
				sil.value_on != 0 || sil.when.empty())
				return false;
			gated++;
			if (sil.keep_through_disarm)
			{
				kept++;
				if (sil.ee_address != 0x550494u && sil.ee_address != 0x5504E0u && sil.ee_address != 0x55052Cu)
					return false;
			}
		}
		return gated == 15 && kept == 3;
	});
	{
		Begin("Physical melee (PhysicalMelee::SelfTest)");
		const std::vector<std::string> failed = VR::PhysicalMelee::SelfTest();
		Record("Physical melee", failed.empty(),
			failed.empty() ? std::string("passed (it names the failed checks only, no check count)") :
							 "FAIL: " + std::to_string(failed.size()) + " check(s), first: " + failed.front());
	}
	RunLogged("Melee profile parser", VR::ProfileDB::SelfTestMeleeParse, "(VR) Melee profile self-test");
	// [VR] PhysicalMelee: File #1's melee block (with melee.online), and its camera.silence entries: the five cave A/B
	// sites and the seven spawn-event calls (each guarded by its cave's first word), then the three caves in free RAM; no
	// shared word changes for anyone but the local player any more (the spawn frames themselves stay stock)
	// (tools/outbreak-vr/research/2026-10-14-melee.md, 2026-10-14-melee-coop.md).
	RunSilent("Physical melee (File #1 profile, melee and camera.silence whenSetting)", +[]() {
		const VR::ProfileDB::Profile* p = VR::ProfileDB::Lookup("SLPM-65428", 0x32088394u);
		if (!p || !p->melee.has_value() || !p->camera.has_value() || !VR::ProfileDB::IsSilenceSettingName("PhysicalMelee"))
			return false;
		const VR::ProfileDB::MeleeParams& m = p->melee.value();
		if (m.attack_button != "Circle" || m.sub_offset != 0x09 || m.sub_idle != 1 || m.sub_attack != 3 ||
			m.stance_normal != 1 || m.enemy_table != 0x3FFCA0 || m.enemy_stride != 0xD90 || m.enemy_count != 64 ||
			m.enemy_hp != 0x544 || !m.has_override || m.block != 0xFF000 || m.magic != 0x4D454C31 || m.counter != 0x48BF93)
			return false;
		if (!m.has_online || m.online_room != 0x3065AC || m.online_masters != 0x825810 || m.online_tag_addr != 0x820020 ||
			m.online_tag != 0x6174656E || m.online_free != 0xFE000)
			return false;
		static constexpr u32 kSpawnCalls[] = {0x568544, 0x5694E8, 0x569560, 0x5695F4, 0x5696B0, 0x56973C, 0x5697B4};
		int sites = 0, calls = 0, cave = 0;
		size_t first_cave = 0, last_site = 0;
		for (size_t i = 0; i < p->camera->silence.size(); i++)
		{
			const VR::ProfileDB::CameraSilence& sil = p->camera->silence[i];
			if (sil.when_setting != "PhysicalMelee")
				continue;
			if (!sil.offline_only || sil.keep_through_disarm || sil.when.empty())
				return false;
			const u32 a = sil.ee_address;
			const bool call = std::find(std::begin(kSpawnCalls), std::end(kSpawnCalls), a) != std::end(kSpawnCalls);
			if (a >= 0xFE000u && a < 0xFE160u)
			{
				if (sil.value_off != 0)
					return false;
				if (cave++ == 0)
					first_cave = i;
			}
			else if (a == 0x552D8Cu || a == 0x552D90u || a == 0x5523C4u || a == 0x5523C8u || a == 0x5523E0u)
			{
				if (sil.when.size() != 2)
					return false; // the overlay and its cave in place
				sites++;
				last_site = i;
			}
			else if (call)
			{
				// jal FUN_00565C80 -> jal 0xFE100 (cave C), only with cave C in place.
				if (sil.value_off != 0x0C159720u || sil.value_on != 0x0C03F840u || sil.when.size() != 2 ||
					sil.when[1].ee_address != 0xFE100u || sil.when[1].equals != 0x3C0B0010u)
					return false;
				calls++;
				last_site = i;
			}
			else
				return false; // nothing else, the shared spawn frames above all
		}
		return sites == 5 && calls == 7 && cave == 39 + 23 + 22 && last_site < first_cave;
	});
	{
		Begin("Physical melee online (PhysicalMelee::SelfTestOnline)");
		const std::vector<std::string> failed = VR::PhysicalMelee::SelfTestOnline();
		Record("Physical melee online", failed.empty(),
			failed.empty() ? std::string("passed (it names the failed checks only, no check count)") :
							 "FAIL: " + std::to_string(failed.size()) + " check(s), first: " + failed.front());
	}
	// The caves themselves, run on the profile's words: only the local record, with the magic and a fresh stamp, takes the
	// VR weapon's pose (A, B) and the early hit window (C); every other record (an AI partner offline, another player
	// online) runs exactly what the game's own code does.
	RunSilent("Physical melee caves (File #1 profile words, interpreted)", +[]() {
		const bool a = MeleeCaveA(kLocalRecord, true, 10, 8, true) && MeleeCaveA(kPartnerRecord, true, 10, 8, false) &&
		               MeleeCaveA(kLocalRecord, false, 10, 8, false) && MeleeCaveA(kLocalRecord, true, 20, 8, false) &&
		               MeleeCaveA(kLocalRecord, true, 2, 58, true);
		const bool b = MeleeCaveB(kLocalRecord, true, true) && MeleeCaveB(kPartnerRecord, true, false) &&
		               MeleeCaveB(kLocalRecord, false, false);
		const bool c = MeleeCaveC(kLocalRecord, true, 10, 8, true) && MeleeCaveC(kPartnerRecord, true, 10, 8, false) &&
		               MeleeCaveC(kPartnerRecord + 0x10E0, true, 10, 8, false) && MeleeCaveC(kLocalRecord, false, 10, 8, false) &&
		               MeleeCaveC(kLocalRecord, true, 20, 8, false) && MeleeCaveC(kLocalRecord, true, 2, 58, true) &&
		               MeleeCaveC(kLocalRecord, true, 14, 8, true) && MeleeCaveC(kLocalRecord, true, 15, 8, false);
		return a && b && c;
	});
	// Contact mode ([VR] PhysicalMeleeMode = Contact; tools/outbreak-vr/research/2026-10-14-vr-melee.md): the pure parts
	// (strength from speed, the shape against hurt spheres, the strike and re-arm logic on scripted swings), File #1's
	// melee.contact and its PhysicalMeleeContact entries (three sites, each guarded by its cave's first and last non-zero
	// word, before caves D and E in free RAM), and the two caves run on the profile's words.
	{
		Begin("Physical melee contact (PhysicalMelee::SelfTestContact)");
		const std::vector<std::string> failed = VR::PhysicalMelee::SelfTestContact();
		Record("Physical melee contact", failed.empty(),
			failed.empty() ? std::string("passed (it names the failed checks only, no check count)") :
							 "FAIL: " + std::to_string(failed.size()) + " check(s), first: " + failed.front());
	}
	RunSilent("Physical melee contact (File #1 profile, melee.contact and camera.silence whenSetting)", +[]() {
		const VR::ProfileDB::Profile* p = VR::ProfileDB::Lookup("SLPM-65428", 0x32088394u);
		if (!p || !p->melee.has_value() || !p->camera.has_value() || !VR::ProfileDB::IsSilenceSettingName("PhysicalMeleeContact"))
			return false;
		const VR::ProfileDB::MeleeParams& m = p->melee.value();
		if (!m.has_contact || m.shape_types != 0x6E5940 || m.shape_type_stride != 0x44 || m.shape_attack != 0x06 ||
			m.shape_params != 0x6E7C90 || m.shape_param_stride != 0x20 || m.shape_index != 0x0E || m.shape_table != 0x6EBFA0 ||
			m.weapon_type_offset != 0xCA8 || m.sphere_count != 0x1E0 || m.sphere_shapes != 0x1D8 || m.sphere_centres != 0x1DC ||
			m.kind_offset != 0x02 || m.enemy_kind != 2 || m.reaction_offset != 0x140 || m.zone_offset != 0x13E ||
			m.hook != 0x5CDF64 || m.hook_on != 0x0C03F880)
			return false;
		// The knives have their own (lower) speeds; the pipe takes the block's.
		if (!(m.ContactFor(33).speed < m.ContactFor(34).speed) || m.ContactFor(34).speed != m.contact.speed)
			return false;
		int sites = 0, cave_d = 0, cave_e = 0, cave_f = 0;
		size_t first_cave = 0, last_site = 0;
		bool any_cave = false;
		for (size_t i = 0; i < p->camera->silence.size(); i++)
		{
			const VR::ProfileDB::CameraSilence& sil = p->camera->silence[i];
			if (sil.when_setting != "PhysicalMeleeContact")
				continue;
			if (!sil.offline_only || sil.keep_through_disarm || sil.when.empty())
				return false;
			const u32 a = sil.ee_address;
			if (a >= 0xFE200u && a < 0xFE360u)
				cave_d++;
			else if (a >= 0xFE400u && a < 0xFE470u)
				cave_e++;
			else if (a >= 0xFE500u && a < 0xFE7B4u)
				cave_f++;
			else if ((a == 0x5CDF64u && sil.value_off == 0x0C16A76Cu && sil.value_on == 0x0C03F880u) ||
					 (a == 0x636990u && sil.value_off == 0x27BDFFD0u && sil.value_on == 0x0803F900u) ||
					 (a == 0x636994u && sil.value_off == 0xFFBF0020u && sil.value_on == 0))
			{
				if (sil.when.size() != (a == 0x5CDF64u ? 5u : 3u))
					return false; // the overlay and its caves' first and last non-zero words (cave D's site: D and F)
				sites++;
				last_site = i;
				continue;
			}
			else
				return false; // nothing else
			if (sil.value_off != 0)
				return false;
			if (!any_cave)
				first_cave = i;
			any_cave = true;
		}
		return sites == 3 && cave_d == 87 && cave_e == 25 && cave_f == 173 && last_site < first_cave;
	});
	RunSilent("Physical melee contact caves (D, E, F: File #1 profile words, interpreted)", MeleeContactCaves);
	// [VR] MeleeHitJolt (a prototype; tools/outbreak-vr/research/2026-10-14-vr-melee.md sections 8 and 9): the pure parts
	// (the magnitude from the swing's speed, the chain for the struck sphere's bone, each bone's turn in its parent's
	// frame, the entries as the cave reads them), File #1's melee.jolt and its MeleeHitJolt entries (the zombie's update
	// site guarded by cave G's first and last words, before the cave), and cave G itself run on the profile's words.
	{
		Begin("Physical melee hit jolt (PhysicalMelee::SelfTestJolt)");
		const std::vector<std::string> failed = VR::PhysicalMelee::SelfTestJolt();
		Record("Physical melee hit jolt", failed.empty(),
			failed.empty() ? std::string("passed (it names the failed checks only, no check count)") :
							 "FAIL: " + std::to_string(failed.size()) + " check(s), first: " + failed.front());
	}
	RunSilent("Physical melee hit jolt (File #1 profile, melee.jolt, cave G interpreted)", +[]() {
		const VR::ProfileDB::Profile* p = VR::ProfileDB::Lookup("SLPM-65428", 0x32088394u);
		if (!p || !p->melee.has_value() || !p->camera.has_value() || !VR::ProfileDB::IsSilenceSettingName("MeleeHitJolt"))
			return false;
		const VR::ProfileDB::MeleeParams& m = p->melee.value();
		if (!m.has_jolt || m.jolt_block != 0xFF100 || m.jolt_magic != 0x4A4C5431 || m.jolt_hook != 0x6644D0 ||
			m.jolt_hook_on != 0x0C03FA00 || m.jolt_bones != 0x34 || m.jolt_bone_table != 0x820 || m.jolt_bone_count != 36 ||
			m.jolt.chains.size() != 3)
			return false;
		const VR::PhysicalMelee::JoltChain* head = VR::PhysicalMelee::JoltChainFor(m.jolt, 21);
		const VR::PhysicalMelee::JoltChain* belly = VR::PhysicalMelee::JoltChainFor(m.jolt, 10);
		if (!head || head->bones[0] != 20 || head->parents[0] != 11 || !belly || belly->bones[0] != 10 || belly->parents[0] != 2 ||
			!VR::PhysicalMelee::JoltChainFor(m.jolt, 3))
			return false;
		// The entries: the site, then the cave (49 words at 0xFE800, clear of cave F and of the melee block's 0x90 bytes).
		MiniEE ee;
		int site = -1, first_cave = -1, words = 0;
		for (size_t i = 0; i < p->camera->silence.size(); i++)
		{
			const VR::ProfileDB::CameraSilence& sil = p->camera->silence[i];
			if (sil.when_setting != "MeleeHitJolt")
				continue;
			if (!sil.offline_only || sil.keep_through_disarm || sil.when.empty())
				return false;
			if (sil.ee_address == 0x6644D0u)
			{
				if (sil.value_off != 0x0C15DE1Cu || sil.value_on != 0x0C03FA00u || sil.when.size() != 3 || first_cave >= 0)
					return false;
				site = static_cast<int>(i);
				continue;
			}
			if (sil.ee_address < 0xFE800u || sil.ee_address >= 0xFE900u || sil.value_off != 0)
				return false;
			if (first_cave < 0)
				first_cave = static_cast<int>(i);
			ee.Store32(sil.ee_address, sil.value_on);
			words++;
		}
		if (site < 0 || words != 49)
			return false;
		// Cave G with the game around it scripted: FUN_00577870 (the game's own jolt) and FUN_0055C450 (the apply) record
		// their arguments and clobber the temporaries.
		struct Call
		{
			u32 pc, a0, a1, a2;
			s16 x, y, z;
		};
		static std::vector<Call> s_calls;
		s_calls.clear();
		ee.stub = +[](MiniEE& e, u32 pc, u32* next) {
			if (pc != 0x577870 && pc != 0x55C450)
				return false;
			Call c{pc, e.r[4], e.r[5], e.r[6], 0, 0, 0};
			if (pc == 0x55C450)
			{
				const auto h = [&](u32 a) { return static_cast<s16>(static_cast<u16>(e.Load32(a) >> ((a & 2u) * 8u))); };
				c.x = h(e.r[6]);
				c.y = h(e.r[6] + 2);
				c.z = h(e.r[6] + 4);
			}
			s_calls.push_back(c);
			for (int k : {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15})
				e.r[k] = 0xDEAD0000u + static_cast<u32>(k);
			*next = e.r[31];
			return true;
		};
		constexpr u32 kEnemy = 0x429530, kOther = 0x42A2C0, kBlk = 0xFF100;
		const auto entry = [&](int k, u32 enemy, u8 bone, u8 frames, s16 x, s16 y, s16 z) {
			const s16 ang[3] = {x, y, z};
			VR::PhysicalMelee::JoltEntry je = VR::PhysicalMelee::MakeJoltEntry(enemy, bone, ang, frames > 0 ? frames : 1);
			je.frames = frames;
			u32 w[6];
			VR::PhysicalMelee::PackJoltEntry(je, w);
			for (int q = 0; q < 6; q++)
				ee.Store32(kBlk + 0x10u + 24u * static_cast<u32>(k) + 4u * static_cast<u32>(q), w[q]);
		};
		const auto run = [&]() {
			ee.r[4] = kEnemy;
			ee.r[5] = 0x6E4C78;
			ee.r[6] = kEnemy + 0xD1C;
			ee.r[29] = 0x01F00000;
			ee.r[31] = 0x6644D8;
			ee.r[16] = 0x5555;
			return ee.Run(0xFE800, 0xFE800, 0xFE900) == 0x6644D8 && ee.r[29] == 0x01F00000 && ee.r[16] == 0x5555;
		};
		entry(0, kEnemy, 20, 3, 300, -60, 9);
		entry(1, kOther, 20, 5, 1000, 0, 0);
		entry(2, kEnemy, 11, 0, 500, 0, 0);
		entry(3, kEnemy, 10, 1, -40, 0, 0);
		// Without the magic: the game's own jolt only.
		ee.Store32(kBlk, 0);
		if (!run() || s_calls.size() != 1 || s_calls[0].pc != 0x577870 || s_calls[0].a0 != kEnemy || s_calls[0].a1 != 0x6E4C78 ||
			s_calls[0].a2 != kEnemy + 0xD1C || ((ee.Load32(kBlk + 0x14) >> 8) & 0xFF) != 3)
			return false;
		// With it: the game's jolt, then this enemy's live entries (0 and 3), each applied, a frame fewer, less its rate.
		s_calls.clear();
		ee.Store32(kBlk, 0x4A4C5431);
		if (!run() || s_calls.size() != 3 || s_calls[0].pc != 0x577870 || s_calls[0].a2 != kEnemy + 0xD1C)
			return false;
		const Call& c1 = s_calls[1];
		const Call& c3 = s_calls[2];
		if (c1.pc != 0x55C450 || c1.a0 != kEnemy || c1.a1 != 20 || c1.a2 != kBlk + 0x18 || c1.x != 300 || c1.y != -60 || c1.z != 9 ||
			c3.pc != 0x55C450 || c3.a1 != 10 || c3.a2 != kBlk + 0x10 + 72 + 8 || c3.x != -40)
			return false;
		const auto h16 = [&](u32 a) { return static_cast<s16>(static_cast<u16>(ee.Load32(a) >> ((a & 2u) * 8u))); };
		if (((ee.Load32(kBlk + 0x14) >> 8) & 0xFF) != 2 || h16(kBlk + 0x18) != 200 || h16(kBlk + 0x1A) != -40 || h16(kBlk + 0x1C) != 6 ||
			((ee.Load32(kBlk + 0x2C) >> 8) & 0xFF) != 5 || h16(kBlk + 0x30) != 1000 || ((ee.Load32(kBlk + 0x44) >> 8) & 0xFF) != 0 ||
			((ee.Load32(kBlk + 0x5C) >> 8) & 0xFF) != 0 || h16(kBlk + 0x60) != 0)
			return false;
		// Two more frames: entry 0 runs out (applied at 200, then 100), entry 3 stays out.
		s_calls.clear();
		if (!run() || !run() || s_calls.size() != 4 || s_calls[1].x != 200 || s_calls[3].x != 100 ||
			((ee.Load32(kBlk + 0x14) >> 8) & 0xFF) != 0)
			return false;
		s_calls.clear();
		if (!run() || s_calls.size() != 1)
			return false;
		// Armed at the strike: an entry that waits holds (no apply, no frame taken) until the enemy's hit counter +0x13F is
		// set, then runs on that same update, stops waiting and runs on from there.
		{
			const s16 ang[3] = {600, 0, 0};
			const VR::PhysicalMelee::JoltEntry je = VR::PhysicalMelee::MakeJoltEntry(kEnemy, 20, ang, 3, true);
			u32 w[6];
			VR::PhysicalMelee::PackJoltEntry(je, w);
			for (int q = 0; q < 6; q++)
				ee.Store32(kBlk + 0x28u + 4u * static_cast<u32>(q), w[q]);
		}
		const auto frames_wait = [&]() { return (ee.Load32(kBlk + 0x2C) >> 8) & 0xFFFFu; };
		ee.Store32(kEnemy + 0x13C, 0x00FFFFFFu); // +0x13F (the hit counter) clear, its neighbours set
		s_calls.clear();
		if (!run() || s_calls.size() != 1 || frames_wait() != 0x0103)
			return false;
		ee.Store32(kEnemy + 0x13C, 0x01000000u); // hit this update
		s_calls.clear();
		if (!run() || s_calls.size() != 2 || s_calls[1].a1 != 20 || s_calls[1].x != 600 || frames_wait() != 0x0002)
			return false;
		ee.Store32(kEnemy + 0x13C, 0); // the counter clear again: it no longer waits
		s_calls.clear();
		return run() && s_calls.size() == 2 && s_calls[1].x == 400 && frames_wait() == 0x0001;
	});
	RunGamepad();
	RunSilent("CameraDriver::SelfTestAssembler (hook)", VR::CameraDriver::SelfTestAssembler);
	RunSilent("CameraDriver::SelfTestMath (math, predict, comfort, FOV)", VR::CameraDriver::SelfTestMath);
	RunSilent("CameraDriver::SelfTestStableSlot (the debounced local slot, review M1)", VR::CameraDriver::SelfTestStableSlot);
	RunSilent("CameraDriver::SelfTestCaves (all-or-nothing caves, review M2 / d2)", VR::CameraDriver::SelfTestCaves);
	RunSilent("ProfileDB::SelfTestMultibandResolve", VR::ProfileDB::SelfTestMultibandResolve);
	RunAutoStereo();
	RunCameraDriverLogged();
#ifdef ENABLE_VULKAN
	RunCompositorLogged();
#endif

	size_t failed = 0;
	for (const SuiteResult& r : s_results)
		failed += r.passed ? 0u : 1u;
	std::printf("\n=== VR self-tests: %zu of %zu passed, %zu failed.\n", s_results.size() - failed, s_results.size(), failed);
	for (const SuiteResult& r : s_results)
		std::printf("  %s  %-58s %s\n", r.passed ? "PASS" : "FAIL", r.name.c_str(), r.detail.c_str());

	// Exit without the emulator's static teardown: nothing here started what its frontend would shut down first.
	std::fflush(nullptr);
	std::_Exit(failed == 0 ? 0 : 1);
}
