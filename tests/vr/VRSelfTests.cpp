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
#include <cstdio>
#include <cstdlib>
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
	// interpreter of just the instructions they use (lui, ori, addiu, sltiu, subu, lw, lbu, sw, lwc1, swc1, beq, bne,
	// bgez, bgtz, j, jr, with delay slots; the FPU loads and stores only move bits).
	struct MiniEE
	{
		std::map<u32, u32> mem; // word address -> word
		u32 r[32] = {};
		u32 f[32] = {};

		u32 Load32(u32 a) const
		{
			const auto it = mem.find(a & ~3u);
			return it == mem.end() ? 0u : it->second;
		}
		void Store32(u32 a, u32 v) { mem[a & ~3u] = v; }
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
					if (fn == 0x23)
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
					*branch = *taken = true;
					*target = (pc & 0xF0000000u) | ((i & 0x3FFFFFFu) << 2);
					break;
				case 0x04: *branch = true; *taken = r[rs] == r[rt]; *target = bt; break;
				case 0x05: *branch = true; *taken = r[rs] != r[rt]; *target = bt; break;
				case 0x07: *branch = true; *taken = static_cast<s32>(r[rs]) > 0; *target = bt; break;
				case 0x09: r[rt] = r[rs] + simm; break;
				case 0x0B: r[rt] = r[rs] < simm ? 1u : 0u; break;
				case 0x0D: r[rt] = r[rs] | imm; break;
				case 0x0F: r[rt] = imm << 16; break;
				case 0x23: r[rt] = Load32(r[rs] + simm); break;
				case 0x24: r[rt] = (Load32(r[rs] + simm) >> (((r[rs] + simm) & 3u) * 8u)) & 0xFFu; break;
				case 0x2B: Store32(r[rs] + simm, r[rt]); break;
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
					return pc;
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
	RunLogged("Menu panel", VR::MenuPanel::SelfTest, "(VR) Menu panel self-test");
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
			if (sil.when_setting.empty() || sil.when_setting == "PhysicalMelee")
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
	RunGamepad();
	RunSilent("CameraDriver::SelfTestAssembler (hook)", VR::CameraDriver::SelfTestAssembler);
	RunSilent("CameraDriver::SelfTestMath (math, predict, comfort, FOV)", VR::CameraDriver::SelfTestMath);
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
